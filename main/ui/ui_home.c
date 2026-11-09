/**
 * @file    ui_home.c
 * @brief   主页“开始识别”按钮的业务实现（手动识别模式）。
 *
 * @note    - 识别策略：face_task 默认只在收到 FACE_CMD_RECOGNIZE 时才识别。
 *            主页平时只显示摄像头画面，用户点击按钮后才识别一次，识别完成
 *            （或失败）后状态卡恢复为“待识别”。这样人脸不进入连续识别的
 *            推理负载，预览与界面更流畅，也避免门口有人时状态卡不停跳变。
 *            - 按钮回调在 LVGL 任务里执行，**不能**在这里阻塞等待识别结果：
 *            识别要 ~100 ms 以上，阻塞期间界面与预览全部停住。这里改用
 *            face_service_recognize_async()（只投命令、不等回执），再用一个
 *            200 ms 定时器轮询 face_service_recognize_in_flight() 判断结束。
 *            **不要改成“临时 xTaskCreate 一个识别线程”**：M4 接入网络后内部 RAM
 *            只剩约 6 KB、最大连续块 <4 KB，4 KB 栈的线程会间歇性创建失败，
 *            现场表现为点“开始识别”毫无反应。
 *            - 状态卡的“识别中/结果文字”由 face_task 在识别帧上写入，失败原因
 *            由 face_service 在回执时补写（异步请求没有等待者）；本模块只负责
 *            开始时写“识别中”、结束后停留几秒再切回“待识别”。
 */

#include "ui_home.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "face_service.h"
#include "clock_service.h"
#include "ui_home_api.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui_home";

/**
 * @brief 轮询异步识别是否结束的周期（毫秒）
 * @note  face_task 收到命令后会用“当前那一帧”判定：有人脸且质量合格就立即识别，
 *        否则立刻回 ESP_ERR_NOT_FOUND，正常路径 0.2~1 s 内结束。
 */
#define UI_HOME_POLL_MS 200

/**
 * @brief 识别成功后结果文字的停留时间（毫秒）
 * @note  成功结论由 face_task 在识别帧上写入，这里等几秒再切回“待识别”，
 *        让用户看清“识别成功 xx%”或“未识别”，时长与开门覆盖层接近。
 */
#define UI_HOME_RESULT_HOLD_MS 3000

/** @brief 异步识别是否在途（防止连点重复排队） */
static volatile bool s_busy = false;
/** @brief 识别结束时刻（毫秒，0 表示当前没有待恢复的结果文字） */
static uint32_t s_done_ms = 0;
/** @brief 轮询定时器（首次点击时创建，由 LVGL 任务驱动） */
static lv_timer_t *s_poll_timer = NULL;

/**
 * @brief 把状态卡文字恢复为“待识别”（空闲态）
 * @note  只在识别结束后的停留期结束时调用；face_task 的自动状态更新（待识别提示等）
 *        会随下一帧继续写入状态卡，两者内容一致，不会来回跳变。
 */
static void restore_idle_text(void)
{
    ui_home_set_state(UI_TXT_HOME_WAITING, UI_COLOR_TEXT_DIM);
}

/**
 * @brief 轮询回调：识别结束则进入“结果停留”，停留期满恢复空闲态
 * @param timer 定时器（未使用）
 * @note  只在 LVGL 任务内执行；face_service_recognize_in_flight() 只是读一个标志，
 *        不会阻塞。
 */
static void recognize_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (s_busy)
    {
        if (!face_service_recognize_in_flight())
        {
            s_busy = false;
            s_done_ms = now; // 结果文字由 face_task/face_service 已写入，先停留
        }
        return;
    }

    if (s_done_ms != 0 && (uint32_t)(now - s_done_ms) >= UI_HOME_RESULT_HOLD_MS)
    {
        s_done_ms = 0;
        restore_idle_text();
    }
}

bool ui_home_recognize_busy(void)
{
    return s_busy;
}

/**
 * @brief 主页时钟控件（LVGL 定时器每秒改写一次）
 * @note  未对时显示 "YYYY/MM/DD HH:MM"（16 字符，约 208 px 宽），SNTP 对时后显示到秒
 *        "YYYY/MM/DD HH:MM:SS"（19 字符，24 px 字体约 250 px 宽）。标题栏 480 px 宽、
 *        标题只占左侧 ~100 px，两者都放得下；右对齐所以字符数变化不影响布局。
 */
static lv_obj_t *s_clock_label = NULL;

/**
 * @brief 时钟定时器回调：刷新时间文字
 * @param timer 定时器（未使用）
 * @note  只在 LVGL 任务内执行；内容未变化时不重绘（未对时时同一分钟内只改一次）。
 */
static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_clock_label == NULL)
    {
        return;
    }

    char buf[32]; // "2026/11/05 10:11:33" 共 19 字符 + 结束符，留余量
    clock_service_format_time(buf, sizeof(buf), true);
    if (strcmp(lv_label_get_text(s_clock_label), buf) != 0)
    {
        lv_label_set_text(s_clock_label, buf);
    }
}

void ui_home_attach_clock(lv_obj_t *screen)
{
    if (screen == NULL)
    {
        return;
    }

    // 挂在屏幕对象上、对齐右上角：标题栏高 40 px，y=8 让 24 px 字在标题栏内垂直居中
    // 占位按"已对时"的最长格式写，避免对时后文字变长时右对齐标签向左跳一下
    s_clock_label = ui_label_create(screen, "----/--/-- --:--:--", UI_FONT_TITLE, UI_COLOR_TEXT);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_RIGHT, -12, 8);

    char buf[32];
    clock_service_format_time(buf, sizeof(buf), true);
    lv_label_set_text(s_clock_label, buf);

    lv_timer_create(clock_timer_cb, 1000, NULL);
    ESP_LOGI(TAG, "主页时钟已创建：%s", buf);
}

void ui_home_recognize_async(void)
{
    if (s_busy)
    {
        ESP_LOGI(TAG, "识别已在进行中，忽略本次点击");
        return;
    }

    // 非阻塞投递：不再为每次点击创建线程（内部 RAM 已不够）
    const esp_err_t err = face_service_recognize_async();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "识别请求未受理：%s", esp_err_to_name(err));
        ui_home_set_state(UI_TXT_PLEASE_WAIT, UI_COLOR_WARN);
        return;
    }

    s_busy = true;
    s_done_ms = 0;
    // 先把状态卡文字改成“识别中”，避免识别开始前的几帧仍显示“待识别”
    ui_home_set_state(UI_TXT_HOME_RECOGNIZING, UI_COLOR_ACCENT);

    if (s_poll_timer == NULL)
    {
        s_poll_timer = lv_timer_create(recognize_poll_cb, UI_HOME_POLL_MS, NULL);
    }
}
