/**
 * @file    door_controller.c
 * @brief   门禁控制实现：事件队列 → door_task 串行执行（模拟开门 / 日志 / 拒绝）。
 *
 * @note    - 开门动作顺序：置 SYS_EVENT_UNLOCK_ACTIVE → 继电器脉冲（若配置）→ 门禁日志
 *            （JSONL）→ 屏幕覆盖层 → 启动释放定时器。
 *          - 日志与覆盖层都在 door_task 中执行：写 Flash 时不会与其他写 Flash 的任务
 *            （face_task 的注册）并发（由 g_log_mutex / face 命令队列天然串行）。
 *          - 覆盖层时长 APP_CFG_DOOR_OPEN_SIM_MS（3 s）；届时清 SYS_EVENT_UNLOCK_ACTIVE。
 *          - 人脸的覆盖层由 face_task 立即弹出（保证画面同步），这里只在“开门事件”路径
 *            弹覆盖层，并做重复抑制，避免 3 秒内连续两次识别弹两次。
 */

#include "door_controller.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_events.h"
#include "access_log.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "snapshot.h"
#include "ui_app.h"
#include "ui_home_api.h"
#include "ui_strings.h"
#include "ui_theme.h"

static const char *TAG = "door";

/** @brief 门禁任务栈
 *  @note  M4 内部 RAM 紧张，从 5 KB 收到 4 KB（实测剩余栈 4.2 KB）。不放 PSRAM：
 *         本任务写门禁日志（SPIFFS），写 Flash 期间 cache 关闭，PSRAM 栈会异常。 */
#define DOOR_TASK_STACK (4 * 1024)
/** @brief 门禁任务优先级（与按键同级，低于人脸/显示） */
#define DOOR_TASK_PRIO 3
/** @brief 事件队列深度 */
#define DOOR_QUEUE_LEN 8

/** @brief 内部事件类型 */
typedef enum
{
    DOOR_EVT_OPEN = 1,
    DOOR_EVT_DENY,
    DOOR_EVT_STRANGER,
} door_evt_type_t;

/** @brief 队列元素：固定字段 + 复制后的字符串（避免指针越界） */
typedef struct
{
    door_evt_type_t type;
    door_source_t source;
    char name[32];
    float similarity;
    float liveness;
    bool liveness_fail;
    char extra[32];
    int track_id;
} door_event_t;

static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_state_lock = NULL;
static volatile bool s_unlocked = false;
static volatile door_source_t s_last_source = DOOR_SOURCE_FACE;
static uint32_t s_open_count = 0;
static int64_t s_last_open_ms = 0;
static esp_timer_handle_t s_release_timer = NULL;

/** @brief 继电器脉冲定时器 */
static esp_timer_handle_t s_relay_timer = NULL;

/** @brief 开门释放定时器回调：清除“开门中”状态 */
static void release_cb(void *arg)
{
    (void)arg;
    s_unlocked = false;
    xEventGroupClearBits(g_sys_events, SYS_EVENT_UNLOCK_ACTIVE);
}

/** @brief 继电器脉冲结束回调：拉低释放 */
static void relay_release_cb(void *arg)
{
    (void)arg;
    gpio_set_level((gpio_num_t)APP_CFG_DOOR_RELAY_GPIO, 0);
}

/**
 * @brief 继电器脉冲：拉高 DOOR_RELAY_GPIO，800 ms 后拉低
 * @note  未配置（-1）时直接返回；开漏/推挽按低电平有效继电器接线，默认推挽输出高电平吸合。
 */
static void relay_pulse(void)
{
    if (APP_CFG_DOOR_RELAY_GPIO < 0)
    {
        return;
    }
    gpio_num_t pin = (gpio_num_t)APP_CFG_DOOR_RELAY_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(pin, 1);

    if (s_relay_timer != NULL)
    {
        esp_timer_restart(s_relay_timer, 800 * 1000);
    }
}

/** @brief 来源 → 日志 reason 字段 */
static const char *source_reason(door_source_t src)
{
    switch (src)
    {
    case DOOR_SOURCE_FACE:
        return ACCESS_LOG_REASON_FACE;
    case DOOR_SOURCE_REMOTE:
        return ACCESS_LOG_REASON_REMOTE;
    case DOOR_SOURCE_VOICE:
        return ACCESS_LOG_REASON_VOICE;
    default:
        return ACCESS_LOG_REASON_UNKNOWN;
    }
}

/** @brief 来源 → 覆盖层标题 */
static const char *source_title(door_source_t src)
{
    switch (src)
    {
    case DOOR_SOURCE_REMOTE:
        return UI_TXT_DOOR_REMOTE_OK;
    case DOOR_SOURCE_VOICE:
        return UI_TXT_DOOR_VOICE_OK;
    default:
        return UI_TXT_DOOR_OPEN;
    }
}

/** @brief 执行开门：日志 + 覆盖层 + 状态 */
static void do_open(const door_event_t *ev)
{
    char sub[48];
    if (ev->name[0] != '\0')
    {
        snprintf(sub, sizeof(sub), UI_FMT_RECOGNIZE, ev->name,
                 ev->similarity > 0.0f ? ev->similarity * 100.0f : 0.0f);
    }
    else
    {
        snprintf(sub, sizeof(sub), "%s", source_title(ev->source));
    }

    // 每次开门都写日志（即使在开门窗口内重复触发，也要让云端看到这次识别）。
    // 重复触发时不再刷新”今日通行”计数，避免同一扇门被连续识别刷高计数。
    const bool repeat = s_unlocked;
    {
        const access_log_entry_t entry = {
            .type = ACCESS_LOG_TYPE_ACCESS,
            .result = ACCESS_LOG_RESULT_OPEN,
            .reason = source_reason(ev->source),
            .name = ev->name[0] ? ev->name : NULL,
            .similarity = ev->similarity,
            .liveness = ev->liveness,
            .extra = ev->extra[0] ? ev->extra : NULL,
        };
        (void)access_log_append(&entry);

        if (!repeat)
        {
            // 今日通行计数以日志为准：写完立即刷新主页
            ui_home_set_counts(-1, (int)access_log_today_open_count());
        }
    }

    // 覆盖层每次都弹（重复触发时重新计时），用户能明确看到“这次远程开门生效了”
    ui_app_show_door_overlay(source_title(ev->source), sub, true, APP_CFG_DOOR_OPEN_SIM_MS);
    ui_app_set_status(UI_TXT_STATUS_DOOR_DONE, UI_COLOR_OK);

    s_unlocked = true;
    s_last_source = ev->source;
    xEventGroupSetBits(g_sys_events, SYS_EVENT_UNLOCK_ACTIVE);
    if (s_release_timer != NULL)
    {
        esp_timer_restart(s_release_timer, (uint64_t)APP_CFG_DOOR_OPEN_SIM_MS * 1000);
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_open_count++;
    s_last_open_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_state_lock);

    ESP_LOGI(TAG, "模拟开门：来源=%s 姓名=%s 相似度=%.2f 活体=%.0f%s%s", source_reason(ev->source),
             ev->name[0] ? ev->name : "-", ev->similarity, ev->liveness,
             ev->extra[0] ? " 抓拍=" : "", ev->extra);
}

/** @brief 执行拒绝：日志 + 状态栏（覆盖层由 face_task 立即弹出，避免画面滞后） */
static void do_deny(const door_event_t *ev)
{
    const access_log_entry_t entry = {
        .type = ACCESS_LOG_TYPE_ACCESS,
        .result = ev->liveness_fail ? ACCESS_LOG_RESULT_DENY_LIVE : ACCESS_LOG_RESULT_DENY,
        .reason = ev->liveness_fail ? ACCESS_LOG_REASON_LIVENESS_FAIL : ACCESS_LOG_REASON_UNKNOWN,
        .name = ev->name[0] ? ev->name : NULL,
        .similarity = ev->similarity,
        .liveness = ev->liveness,
        .extra = ev->extra[0] ? ev->extra : NULL,
    };
    (void)access_log_append(&entry);

    ui_app_set_status(ev->liveness_fail ? UI_TXT_DOOR_LIVENESS_FAIL : UI_TXT_DOOR_NOT_ENROLLED,
                      UI_COLOR_DANGER);
    ESP_LOGI(TAG, "拒绝记录：%s 姓名=%s 相似度=%.2f", ev->liveness_fail ? "活体失败" : "未注册",
             ev->name[0] ? ev->name : "-", ev->similarity);
}

/** @brief 执行陌生人记录 */
static void do_stranger(const door_event_t *ev)
{
    const access_log_entry_t entry = {
        .type = ACCESS_LOG_TYPE_STRANGER,
        .result = ACCESS_LOG_RESULT_INFO,
        .reason = ACCESS_LOG_REASON_UNKNOWN,
        .name = NULL,
        .similarity = ev->similarity,
        .liveness = -1.0f,
        .extra = ev->extra[0] ? ev->extra : NULL,
    };
    (void)access_log_append(&entry);

    ui_app_set_status(UI_TXT_DOOR_NOT_ENROLLED, UI_COLOR_WARN);
    ESP_LOGW(TAG, "发现陌生人：轨迹 %d%s%s", ev->track_id, ev->extra[0] ? " 抓拍=" : "",
             ev->extra);
}

/** @brief 门禁任务：串行执行事件 */
static void door_task(void *arg)
{
    (void)arg;
    door_event_t ev;
    while (true)
    {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        switch (ev.type)
        {
        case DOOR_EVT_OPEN:
            do_open(&ev);
            break;
        case DOOR_EVT_DENY:
            do_deny(&ev);
            break;
        case DOOR_EVT_STRANGER:
            do_stranger(&ev);
            break;
        default:
            break;
        }
    }
}

esp_err_t door_controller_init(void)
{
    if (s_queue != NULL)
    {
        return ESP_OK;
    }

    s_queue = xQueueCreate(DOOR_QUEUE_LEN, sizeof(door_event_t));
    s_state_lock = xSemaphoreCreateMutex();
    if (s_queue == NULL || s_state_lock == NULL)
    {
        ESP_LOGE(TAG, "门禁队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t args = {
        .callback = release_cb,
        .name = "door_release",
    };
    if (esp_timer_create(&args, &s_release_timer) != ESP_OK)
    {
        s_release_timer = NULL;
        ESP_LOGW(TAG, "开门释放定时器创建失败");
    }

    if (xTaskCreatePinnedToCore(door_task, "door_task", DOOR_TASK_STACK, NULL, DOOR_TASK_PRIO, NULL,
                                0) != pdPASS)
    {
        ESP_LOGE(TAG, "门禁任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    if (APP_CFG_DOOR_RELAY_GPIO < 0)
    {
        ESP_LOGI(TAG, "门禁控制已启动（模拟开门，未配置继电器）");
    }
    else
    {
        const esp_timer_create_args_t relay_args = {
            .callback = relay_release_cb,
            .name = "door_relay",
        };
        if (esp_timer_create(&relay_args, &s_relay_timer) != ESP_OK)
        {
            s_relay_timer = NULL;
            ESP_LOGW(TAG, "继电器释放定时器创建失败，继电器将保持吸合");
        }
        ESP_LOGI(TAG, "门禁控制已启动（继电器 GPIO%d）", APP_CFG_DOOR_RELAY_GPIO);
    }
    return ESP_OK;
}

/** @brief 通用入队 */
static esp_err_t push_event(door_event_t *ev)
{
    if (s_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_queue, ev, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "门禁事件队列已满，丢弃事件");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t door_controller_open(const door_open_info_t *info)
{
    if (info == NULL || s_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // 不再在入口静默丢弃人脸/语音开门请求：始终入队，由 do_open 里的 repeat 判断
    // 决定是否重复触发继电器/覆盖层。日志每次都写（否则识别成功但云端看不到记录）。
    door_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DOOR_EVT_OPEN;
    ev.source = info->source;
    if (info->name != NULL)
    {
        snprintf(ev.name, sizeof(ev.name), "%s", info->name);
    }
    ev.similarity = info->similarity >= 0.0f ? info->similarity : -1.0f;
    ev.liveness = info->liveness >= 0.0f ? info->liveness : -1.0f;
    if (info->extra != NULL)
    {
        snprintf(ev.extra, sizeof(ev.extra), "%s", info->extra);
    }
    ev.track_id = info->track_id;

    relay_pulse();
    return push_event(&ev);
}

esp_err_t door_controller_deny(const door_deny_info_t *info)
{
    if (info == NULL || s_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    door_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DOOR_EVT_DENY;
    if (info->name != NULL)
    {
        snprintf(ev.name, sizeof(ev.name), "%s", info->name);
    }
    ev.similarity = info->similarity >= 0.0f ? info->similarity : -1.0f;
    ev.liveness = info->liveness >= 0.0f ? info->liveness : -1.0f;
    ev.liveness_fail = info->liveness_fail;
    if (info->snap != NULL)
    {
        snprintf(ev.extra, sizeof(ev.extra), "%s", info->snap);
    }
    ev.track_id = info->track_id;
    return push_event(&ev);
}

esp_err_t door_controller_stranger(const char *snap, int track_id)
{
    if (s_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    door_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DOOR_EVT_STRANGER;
    if (snap != NULL)
    {
        snprintf(ev.extra, sizeof(ev.extra), "%s", snap);
    }
    ev.similarity = -1.0f;
    ev.liveness = -1.0f;
    ev.track_id = track_id;
    return push_event(&ev);
}

bool door_controller_is_open(void)
{
    return s_unlocked;
}

door_source_t door_controller_last_source(void)
{
    return s_last_source;
}

uint32_t door_controller_open_count(void)
{
    if (s_state_lock == NULL)
    {
        return 0;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    const uint32_t n = s_open_count;
    xSemaphoreGive(s_state_lock);
    return n;
}
