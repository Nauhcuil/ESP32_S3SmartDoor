/**
 * @file    who_human_face_recognition.cpp
 * @brief   人脸门禁应用入口（M2 起降为“编排层”）：只负责创建流水线队列、
 *          启动摄像头采集 / 显示 / 按键任务，并把业务请求转发给 face_service。
 *
 * @note    - 从 M2 开始，MSR01 / MNP01 / FaceRecognition112V1S8 模型只由
 *            face_task（main/face/face_task.cpp）持有；本文件不再直接推理。
 *            - 流水线：
 *              摄像头采集 (Core1, 5) --xQueueAIFrame--> face_task (Core0, 5)
 *                                                       --xQueueLCDFrame--> LCD 任务 (Core0, 5)
 *            - 帧归属：face_task 是唯一把帧送入显示队列的消费者；LCD 任务负责
 *              esp_camera_fb_return()。本文件不再接触帧缓冲区。
 *            - 按键（GPIO 11，低电平有效）：长按 = 注册，单击 = 识别，双击 = 清空人脸库。
 *              事件由 4 状态机 + 20 ms 消抖产生，按键任务通过 face_service 门面执行，
 *              阻塞期间不影响 face_task 的取帧与显示。
 */

#include "who_human_face_recognition.hpp"

#include <string.h>

#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "face_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreatePinnedToCoreWithCaps（PSRAM 栈） */
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd.h"
#include "sys_monitor.h"
#include "ui_lvgl_port.h"
#include "ui_preview.h"

static const char *TAG = "human_face_recognition";

// ======================== 流水线队列 ========================

/** @brief 摄像头 → face_task */
static QueueHandle_t xQueueAIFrame = NULL;
/** @brief face_task → LCD */
static QueueHandle_t xQueueLCDFrame = NULL;

// ======================== 按键定义 ========================

/** @brief 按键 GPIO（低电平有效） */
#define BUTTON_GPIO GPIO_NUM_11

/** @brief 注册超时：在该时间内等待画面出现质量合格的人脸 */
#define BUTTON_ENROLL_TIMEOUT_MS 10000
/** @brief 识别超时 */
#define BUTTON_RECOGNIZE_TIMEOUT_MS 3000

/**
 * @enum button_event_t
 * @brief 按键事件类型
 */
typedef enum
{
    BUTTON_EVENT_NONE = 0,     /**< 无事件 */
    BUTTON_EVENT_SINGLE_CLICK, /**< 单击：识别 */
    BUTTON_EVENT_DOUBLE_CLICK, /**< 双击：清空人脸库 */
    BUTTON_EVENT_LONG_PRESS,   /**< 长按：注册 */
} button_event_t;

/**
 * @enum button_state_t
 * @brief 按键状态机（4 状态）
 */
typedef enum
{
    BUTTON_STATE_IDLE,         /**< 空闲 */
    BUTTON_STATE_PRESSED,      /**< 按下中 */
    BUTTON_STATE_WAIT_RELEASE, /**< 长按触发后等待松开 */
    BUTTON_STATE_WAIT_DOUBLE,  /**< 等待第二次点击 */
} button_state_t;

/**
 * @brief 按键检测任务
 * @param arg 未使用
 * @note  每个事件触发一次 face_service 调用（阻塞至完成或超时），期间状态机暂停检测，
 *        保证同一按键不会重复触发。栈 4 KB 足够 face_service 内部的日志格式化。
 */
static void button_task(void *arg)
{
    (void)arg;

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BUTTON_GPIO);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    button_state_t state = BUTTON_STATE_IDLE;
    TickType_t press_time = 0;
    TickType_t release_time = 0;
    int click_count = 0;

    const TickType_t DEBOUNCE_TIME = pdMS_TO_TICKS(20);
    const TickType_t LONG_PRESS_TIME = pdMS_TO_TICKS(1000);
    const TickType_t DOUBLE_CLICK_TIME = pdMS_TO_TICKS(300);

    ESP_LOGI(TAG, "按键检测任务已启动：长按注册 / 单击识别 / 双击清空");

    /** @brief 执行一次按键动作（阻塞直到 face_service 返回） */
    auto handle = [](button_event_t event) {
        switch (event)
        {
        case BUTTON_EVENT_LONG_PRESS:
        {
            int id = -1;
            ESP_LOGI(TAG, "按键：注册人脸");
            const esp_err_t err = face_service_enroll("", &id, BUTTON_ENROLL_TIMEOUT_MS);
            ESP_LOGI(TAG, "注册结果：%d id=%d", (int)err, id);
            break;
        }
        case BUTTON_EVENT_SINGLE_CLICK:
        {
            face_result_t res;
            ESP_LOGI(TAG, "按键：识别人脸");
            const face_err_t err = face_service_recognize(&res, BUTTON_RECOGNIZE_TIMEOUT_MS);
            if (err == FACE_OK)
            {
                ESP_LOGI(TAG, "识别结果：id=%d name=%s sim=%.2f live=%.0f → %s", res.id, res.name,
                         res.similarity, res.liveness, face_action_text(res.action));
            }
            else
            {
                ESP_LOGW(TAG, "识别未完成：%d", (int)err);
            }
            break;
        }
        case BUTTON_EVENT_DOUBLE_CLICK:
        {
            ESP_LOGI(TAG, "按键：清空人脸库");
            const esp_err_t err = face_service_clear_all();
            ESP_LOGI(TAG, "清空结果：%d", (int)err);
            break;
        }
        default:
            break;
        }
    };

    while (true)
    {
        const int level = gpio_get_level(BUTTON_GPIO);

        switch (state)
        {
        case BUTTON_STATE_IDLE:
            if (level == 0)
            {
                vTaskDelay(DEBOUNCE_TIME);
                if (gpio_get_level(BUTTON_GPIO) == 0)
                {
                    state = BUTTON_STATE_PRESSED;
                    press_time = xTaskGetTickCount();
                }
            }
            break;

        case BUTTON_STATE_PRESSED:
            if (level == 1)
            {
                vTaskDelay(DEBOUNCE_TIME);
                if (gpio_get_level(BUTTON_GPIO) == 1)
                {
                    release_time = xTaskGetTickCount();
                    click_count++;
                    state = BUTTON_STATE_WAIT_DOUBLE;
                }
            }
            else if ((xTaskGetTickCount() - press_time) > LONG_PRESS_TIME)
            {
                handle(BUTTON_EVENT_LONG_PRESS);
                state = BUTTON_STATE_WAIT_RELEASE;
            }
            break;

        case BUTTON_STATE_WAIT_RELEASE:
            if (level == 1)
            {
                vTaskDelay(DEBOUNCE_TIME);
                if (gpio_get_level(BUTTON_GPIO) == 1)
                {
                    state = BUTTON_STATE_IDLE;
                }
            }
            break;

        case BUTTON_STATE_WAIT_DOUBLE:
            if (level == 0)
            {
                vTaskDelay(DEBOUNCE_TIME);
                if (gpio_get_level(BUTTON_GPIO) == 0)
                {
                    click_count++;
                    state = BUTTON_STATE_PRESSED;
                    press_time = xTaskGetTickCount();
                }
            }
            else if ((xTaskGetTickCount() - release_time) > DOUBLE_CLICK_TIME)
            {
                if (click_count >= 2)
                {
                    handle(BUTTON_EVENT_DOUBLE_CLICK);
                }
                else if (click_count == 1)
                {
                    handle(BUTTON_EVENT_SINGLE_CLICK);
                }
                click_count = 0;
                state = BUTTON_STATE_IDLE;
            }
            break;

        default:
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =================================================================
// LCD 显示任务
// =================================================================

/**
 * @brief 显示任务：把帧交给 LVGL 预览（或回退直绘），然后归还帧缓冲
 * @param arg 未使用
 * @note  LVGL 预览限速 ≈22 fps（45 ms）：每帧要拷贝 153 KB（PSRAM→PSRAM），
 *        LCD 任务优先级 5 高于 LVGL 任务（1），不限速会长时间占住 Core0、挤占
 *        绘制与识别时间。限速值必须大于 LVGL 预览服务周期（UI_PREVIEW_SERVICE_MS
 *        = 20 ms），否则上一帧还没被定时器取走、本次推送就会被拒绝，预览白白掉帧。
 *        采集实测约 22 fps，因此 45 ms 限速与采集速率匹配；LVGL 服务在提交图像后
 *        立即刷新，避免再等待默认显示刷新周期。每帧仍需拷贝 153 KB 的 PSRAM 图像。
 *        无论是否更新预览，帧都必须归还。
 */
static void task_process_lcd(void *arg)
{
    (void)arg;
    camera_fb_t *frame = NULL;

    static int64_t s_last_preview_us = 0;

    while (true)
    {
        if (xQueueReceive(xQueueLCDFrame, &frame, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        if (ui_lvgl_is_ready())
        {
            if (s_last_preview_us == 0 || (now_us - s_last_preview_us) >= 45000) // 见函数注释
            {
                const bool pushed = ui_preview_push_frame((const uint16_t *)frame->buf,
                                                          (int)frame->width, (int)frame->height);
                // 只有真正推入新帧才刷新时间戳：若此刻 LVGL 定时器还没把上一帧交给
                // 控件，push 会拒绝本次调用；此时不计时，下一帧立刻再试，
                // 否则会出现“被拒绝 → 再等一个限速周期”的抖动闪屏。
                s_last_preview_us = pushed ? now_us : 0;
            }
        }
        else
        {
            const uint32_t x_start = (LCD_WIDTH - frame->width) / 2;
            const uint32_t y_start = (LCD_HEIGHT - frame->height) / 2;
            lcd_draw_bitmap(x_start, y_start, x_start + frame->width, y_start + frame->height,
                            (uint16_t *)frame->buf);
        }

        esp_camera_fb_return(frame);
        frame = NULL;
    }
}

// =================================================================
// 摄像头采集任务
// =================================================================

/**
 * @brief 摄像头采集任务：取帧后送入 AI 队列
 * @param arg 未使用
 * @note  队列满时阻塞，帧率自动跟随流水线速度。
 */
static void task_process_camera(void *arg)
{
    (void)arg;
    while (true)
    {
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame != NULL)
        {
            sys_monitor_count_capture(); // 仅在帧率统计窗口打开时计数（调试用）
            xQueueSend(xQueueAIFrame, &frame, portMAX_DELAY);
        }
    }
}

// =================================================================
// 自检（M2 联调临时开关，已于硬件验证通过，默认关闭）
// =================================================================

#define FACE_M2_SELFTEST 0

// =================================================================
// M3 联调自检：验证 SPIFFS 日志 / 门禁事件 / 今日计数（已于硬件验证通过，默认关闭）
// =================================================================
#define M3_SELFTEST 0

#if M3_SELFTEST
#include "access_log.h"
#include "door_controller.h"
#include "storage.h"

/**
 * @brief 启动 10 秒后触发一次“模拟开门”，检查日志落盘与计数
 * @note  仅用于 M3 联调；无人机场景下验证 door_task→access_log→主页计数链路。
 */
static void m3_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));

    size_t total = 0;
    size_t used = 0;
    storage_usage(&total, &used);
    ESP_LOGI(TAG, "[M3自检] spiffs=%u/%u bytes, log_seq=%u log_count=%u", (unsigned)used,
             (unsigned)total, access_log_get_seq(), access_log_get_count());

    const door_open_info_t info = {
        .source = DOOR_SOURCE_TEST,
        .name = "SelfTest",
        .similarity = 0.9f,
        .liveness = 88.0f,
        .extra = NULL,
        .track_id = -1,
    };
    ESP_LOGI(TAG, "[M3自检] door_controller_open → %d", (int)door_controller_open(&info));

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "[M3自检] 开门计数=%u 今日=%u log_count=%u seq=%u",
             (unsigned)door_controller_open_count(), (unsigned)access_log_today_open_count(),
             access_log_get_count(), access_log_get_seq());

    access_log_record_t rec[4];
    const size_t n = access_log_read_recent(rec, 4, 3);
    ESP_LOGI(TAG, "[M3自检] 最近记录 %u 条", (unsigned)n);
    for (size_t i = 0; i < n; i++)
    {
        ESP_LOGI(TAG, "[M3自检]   #%u %s/%s/%s name=%s sim=%.2f", rec[i].seq, rec[i].type,
                 rec[i].result, rec[i].reason, rec[i].name, rec[i].similarity);
    }
    vTaskDelete(NULL);
}
#endif

#if FACE_M2_SELFTEST
/**
 * @brief 启动 8 秒后自检 face_service 的命令/响应链路
 * @note  无人脸场景下验证：状态读取、识别（应返回 NO_FACE）、列表、阈值设置、注册失败回执。
 *        仅用于 M2 联调，不影响正常业务路径。
 */
static void face_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));

    face_service_status_t st;
    face_service_get_status(&st);
    ESP_LOGI(TAG, "[自检] face_present=%d enrolled=%d threshold=%.2f live=%d strategy=%d",
             st.face_present, st.enrolled_num, st.threshold, st.liveness_enabled,
             (int)st.strategy);

    face_result_t res;
    const face_err_t rerr = face_service_recognize(&res, 2000);
    ESP_LOGI(TAG, "[自检] recognize → err=%d（无脸时应为 FACE_ERR_NO_FACE=1）", (int)rerr);

    face_record_t list[8];
    size_t n = 0;
    const esp_err_t lerr = face_service_list(list, sizeof(list) / sizeof(list[0]), &n);
    ESP_LOGI(TAG, "[自检] list → err=%d n=%d", (int)lerr, (int)n);

    const esp_err_t terr = face_service_set_threshold(0.60f);
    ESP_LOGI(TAG, "[自检] set_threshold(0.60) → err=%d now=%.2f", (int)terr,
             face_service_get_threshold());
    face_service_set_threshold(0.55f);

    const esp_err_t eerr = face_service_enroll("", NULL, 2000);
    ESP_LOGI(TAG, "[自检] enroll(无脸) → err=%d（应为 ESP_ERR_NOT_FOUND=0x105）", (int)eerr);

    ESP_LOGI(TAG, "[自检] 完成");
    vTaskDelete(NULL);
}
#endif

// =================================================================
// 应用初始化
// =================================================================

/**
 * @brief 初始化并启动人脸门禁应用
 * @note  必须在 camera_init() / lcd_init() / ui_app_start() 之后调用。
 *        摄像头队列创建失败时直接返回，不启动任何任务。
 */
void app_human_face_recognition(void)
{
    xQueueLCDFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    if (xQueueLCDFrame == NULL || xQueueAIFrame == NULL)
    {
        ESP_LOGE(TAG, "创建帧队列失败");
        return;
    }

    // 人脸服务持有全部模型，并启动 face_task（Core0，优先级 5，栈 16 KB）
    const esp_err_t err = face_service_init(xQueueAIFrame, xQueueLCDFrame);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "人脸服务启动失败：%s", esp_err_to_name(err));
        return;
    }

    xTaskCreatePinnedToCore(task_process_camera, "task_process_camera", 3 * 1024, NULL, 5, NULL,
                            1);
    xTaskCreatePinnedToCore(task_process_lcd, "task_process_lcd", 4 * 1024, NULL, 5, NULL, 0);
    // 按键任务栈放 PSRAM：它只调用 face_service 门面并阻塞等回执，自己不碰 Flash/模型，
    // 内部 RAM 让给识别器工作区（MQTT 起来后内部最大块只有 1.8 KB，见 main.c 注释）。
    // 栈 4 KB 不能降：ESP_LOGI 的 vsnprintf 在 2 KB 时溢出过。
    const BaseType_t btn_ok = xTaskCreatePinnedToCoreWithCaps(
        button_task, "button_task", 4 * 1024, NULL, 3, NULL, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (btn_ok != pdPASS)
    {
        // 退回到内部 RAM：按键是可选项，界面/语音仍能操作
        if (xTaskCreatePinnedToCore(button_task, "button_task", 4 * 1024, NULL, 3, NULL, 0) !=
            pdPASS)
        {
            ESP_LOGE(TAG, "按键任务创建失败（按键不可用，界面/语音仍可用）");
        }
    }

#if FACE_M2_SELFTEST
    xTaskCreatePinnedToCore(face_selftest_task, "face_selftest", 4 * 1024, NULL, 2, NULL, 0);
#endif
#if M3_SELFTEST
    xTaskCreatePinnedToCore(m3_selftest_task, "m3_selftest", 4 * 1024, NULL, 2, NULL, 0);
#endif

    ESP_LOGI(TAG, "人脸门禁应用已启动");
}
