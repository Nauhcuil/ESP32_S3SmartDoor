/**
 * @file    sys_monitor.c
 * @brief   系统运行监控实现：按固定窗口统计 fps 与 AI 耗时，并打印堆内存/栈水位。
 *
 * @note    - 统计计数由 portMUX 临界区保护，业务任务在双核上上报不会丢计数。
 *          - 栈水位通过 xTaskGetHandle() + uxTaskGetStackHighWaterMark() 读取（依赖
 *            CONFIG_FREERTOS_USE_TRACE_FACILITY=y，已在 sdkconfig.defaults 中开启）。
 *          - 本任务只读取统计与内存信息，不调用摄像头/Flash，不会干扰人脸流水线。
 */

#include "sys_monitor.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdbool.h>
#include "ui_preview.h"   // 预览帧计数（LVGL 未启用时返回 0）
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreatePinnedToCoreWithCaps（PSRAM 栈） */
#include "freertos/task.h"

static const char *TAG = "sys_monitor"; // 日志标签

/** @brief 保护统计计数器的自旋锁 */
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_frame_count = 0;           // 累计处理帧数
static uint64_t s_detect_us_total = 0;       // 累计检测耗时（微秒）
static uint32_t s_detect_us_max = 0;         // 单帧最大检测耗时（微秒）
static uint32_t s_recognize_count = 0;       // 累计识别次数
static uint64_t s_recognize_us_total = 0;    // 累计识别耗时（微秒）
static uint32_t s_recognize_us_max = 0;      // 单次最大识别耗时（微秒）

static TaskHandle_t s_task_handle = NULL;    // 监控任务句柄

// ---- 帧率统计窗口（调试用，见 sys_monitor_start_fps_window）----
/** @brief 窗口内累计采集帧数（采集任务写） */
static volatile uint32_t s_cap_count = 0;
/** @brief 窗口内累计送显示帧数（face_task 写） */
static volatile uint32_t s_disp_count = 0;
/** @brief 窗口起始时刻（微秒） */
static volatile int64_t s_window_start_us = 0;
/** @brief 窗口长度（毫秒，0 = 未运行） */
static volatile uint32_t s_window_ms = 0;
/** @brief 一次性定时器（首次使用时创建，之后复用） */
static esp_timer_handle_t s_window_timer = NULL;

/** @brief 需要打印栈水位的关键任务名（与 xTaskCreate 时一致） */
static const char *const s_watch_tasks[] = {
    "task_process_camera",
    "task_process_ai",
    "face_task",
    "task_process_lcd",
    "button_task",
    "door_task",
    "snapshot",
    "sys_monitor",
    // esp_lvgl_port 内部创建的任务名固定为 "LVGL task"（含空格），栈 8 KB
    "LVGL task",
};

/** @brief 最近一个周期算出的速率快照（诊断页读取，s_stats_lock 保护） */
static float s_last_fps_detect = 0.0f;
static float s_last_fps_preview = 0.0f;
static uint32_t s_last_detect_avg_us = 0;
static uint32_t s_last_recog_avg_us = 0;

/**
 * @brief 把任务名截断成 FreeRTOS 内部保存的形式
 * @param name 完整任务名
 * @param out  输出缓冲，至少 configMAX_TASK_NAME_LEN 字节
 * @note  任务名受 configMAX_TASK_NAME_LEN（默认 16）限制会被截断，例如
 *        "task_process_camera" 实际保存为 "task_process_ca"。xTaskGetHandle() 是**精确匹配**
 *        且断言传入名短于 configMAX_TASK_NAME_LEN，所以必须先截断。
 */
static void make_task_name(const char *name, char out[configMAX_TASK_NAME_LEN])
{
    memset(out, 0, configMAX_TASK_NAME_LEN);
    strncpy(out, name, configMAX_TASK_NAME_LEN - 1);
}

/**
 * @brief 打印关键任务的剩余栈空间（单位：字节）
 * @note  栈高水位线单位是 StackType_t（ESP32-S3 上为 4 字节），乘以 sizeof 得到字节数。
 *
 *        **不要用 uxTaskGetSystemState() 做这件事**：它在 xKernelLock 内核自旋锁内遍历
 *        所有任务并逐个扫描栈（face_task 16 KB、voice_task 8 KB 栈还在 PSRAM，扫描更慢），
 *        实测持锁 2.5~3 ms。Core1 上的 cam_task（摄像头 DMA 任务）被中断唤醒后也要拿这把
 *        锁才能上 CPU，于是这段时间 GDMA 继续往只够 1.7 ms 的环形中转区里写，整圈被覆盖，
 *        现场表现为每 5 s 一次的 "cam_hal: FB-SIZE: 145920 != 153600" 掉帧 + 下一帧再丢
 *        8 段。xTaskGetHandle()（仅遍历链表、不扫栈）+ uxTaskGetStackHighWaterMark()
 *        （不持锁，扫描在调用者上下文进行）可以拿到同样的信息而没有长时间持锁。
 */
static void log_stack_watermarks(void)
{
    char line[192]; // 单行输出，减少日志行数
    int offset = snprintf(line, sizeof(line), "stack_free(B):");
    if (offset < 0)
    {
        return;
    }

    for (size_t i = 0; i < sizeof(s_watch_tasks) / sizeof(s_watch_tasks[0]); i++)
    {
        char name[configMAX_TASK_NAME_LEN];
        make_task_name(s_watch_tasks[i], name);

        const TaskHandle_t handle = xTaskGetHandle(name);
        if (handle == NULL)
        {
            continue; // 该任务尚未创建（或已删除），跳过
        }

        // 剩余栈字节数；空间不足时 snprintf 会截断，仍保证缓冲区以 '\0' 结尾
        offset += snprintf(line + offset, sizeof(line) - offset, " %s=%lu", s_watch_tasks[i],
                           (unsigned long)(uxTaskGetStackHighWaterMark(handle) *
                                           sizeof(StackType_t)));
        if (offset < 0 || (size_t)offset >= sizeof(line))
        {
            break;
        }
    }

    ESP_LOGI(TAG, "%s", line);
}

/**
 * @brief 帧率窗口到期回调：打印采集/送显示/检测三个帧率并关闭窗口
 * @param arg 未使用
 * @note  在 esp_timer 任务里执行（不属于任何业务任务），只读计数并打印一行日志。
 *        检测帧率用 s_frame_count 在窗口内的增量——它只在真正跑完检测的帧上递增。
 */
static void fps_window_timer_cb(void *arg)
{
    (void)arg;

    const int64_t now_us = esp_timer_get_time();
    uint32_t dt_ms = (uint32_t)((now_us - s_window_start_us) / 1000);
    if (dt_ms == 0)
    {
        dt_ms = 1;
    }

    taskENTER_CRITICAL(&s_stats_lock);
    const uint32_t detect_frames = s_frame_count;
    taskEXIT_CRITICAL(&s_stats_lock);
    const uint32_t cap = s_cap_count;
    const uint32_t disp = s_disp_count;

    ESP_LOGI(TAG, "[帧率] 窗口 %lu ms：采集 %.1f fps，送显示 %.1f fps，检测 %.1f fps",
             (unsigned long)dt_ms, (float)cap * 1000.0f / (float)dt_ms,
             (float)disp * 1000.0f / (float)dt_ms,
             (float)detect_frames * 1000.0f / (float)dt_ms);

    s_window_ms = 0;
}

esp_err_t sys_monitor_start_fps_window(uint32_t timeout_ms)
{
    if (timeout_ms == 0 || timeout_ms > 60000)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_window_timer == NULL)
    {
        const esp_timer_create_args_t args = {
            .callback = fps_window_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "fps_window",
            .skip_unhandled_events = true,
        };
        const esp_err_t err = esp_timer_create(&args, &s_window_timer);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "创建帧率统计定时器失败：%s", esp_err_to_name(err));
            return ESP_ERR_NO_MEM;
        }
    }

    // 先停掉可能还在运行的窗口（一次性定时器在停止后才能再次启动）
    if (s_window_ms != 0)
    {
        sys_monitor_stop_fps_window();
    }

    taskENTER_CRITICAL(&s_stats_lock);
    s_cap_count = 0;
    s_disp_count = 0;
    taskEXIT_CRITICAL(&s_stats_lock);
    s_window_start_us = esp_timer_get_time();
    s_window_ms = timeout_ms;

    const esp_err_t err = esp_timer_start_once(s_window_timer, (uint64_t)timeout_ms * 1000);
    if (err != ESP_OK)
    {
        s_window_ms = 0;
        ESP_LOGE(TAG, "启动帧率统计定时器失败：%s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "帧率统计窗口已打开（%lu ms）", (unsigned long)timeout_ms);
    return ESP_OK;
}

void sys_monitor_stop_fps_window(void)
{
    if (s_window_ms == 0)
    {
        return;
    }
    s_window_ms = 0;
    if (s_window_timer != NULL)
    {
        esp_timer_stop(s_window_timer); // 返回值忽略：可能已经到期自行停止
    }
}

bool sys_monitor_fps_window_active(void)
{
    return s_window_ms != 0;
}

void sys_monitor_count_capture(void)
{
    if (s_window_ms == 0)
    {
        return;
    }
    taskENTER_CRITICAL(&s_stats_lock);
    s_cap_count++;
    taskEXIT_CRITICAL(&s_stats_lock);
}

void sys_monitor_count_display(void)
{
    if (s_window_ms == 0)
    {
        return;
    }
    taskENTER_CRITICAL(&s_stats_lock);
    s_disp_count++;
    taskEXIT_CRITICAL(&s_stats_lock);
}

/**
 * @brief 监控任务主循环：每 APP_CFG_SYS_MONITOR_PERIOD_MS 输出一次统计
 * @param arg 未使用
 */
static void sys_monitor_task(void *arg)
{
    (void)arg;

    uint32_t last_frames = 0;
    uint64_t last_detect_total = 0;
    uint32_t last_recogs = 0;
    uint64_t last_recog_total = 0;
    uint32_t last_previews = 0;
    int64_t last_us = esp_timer_get_time();

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(APP_CFG_SYS_MONITOR_PERIOD_MS));

        int64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - last_us) / 1000);
        if (dt_ms == 0)
        {
            dt_ms = 1; // 防御除零（正常不会发生）
        }

        // 在临界区内取出快照，避免双核并发导致计数撕裂
        uint32_t frames;
        uint64_t detect_total;
        uint32_t detect_max;
        uint32_t recogs;
        uint64_t recog_total;
        uint32_t recog_max;
        taskENTER_CRITICAL(&s_stats_lock);
        frames = s_frame_count;
        detect_total = s_detect_us_total;
        detect_max = s_detect_us_max;
        recogs = s_recognize_count;
        recog_total = s_recognize_us_total;
        recog_max = s_recognize_us_max;
        taskEXIT_CRITICAL(&s_stats_lock);

        uint32_t frame_delta = frames - last_frames;
        uint32_t recog_delta = recogs - last_recogs;
        float fps = (float)frame_delta * 1000.0f / (float)dt_ms;
        uint32_t detect_avg =
            frame_delta ? (uint32_t)((detect_total - last_detect_total) / frame_delta) : 0;
        uint32_t recog_avg =
            recog_delta ? (uint32_t)((recog_total - last_recog_total) / recog_delta) : 0;

        // 预览帧率：ui_preview 只在主页且加锁成功时计数，直接反映上屏刷新速率
        uint32_t previews = ui_preview_get_push_count();
        float preview_fps = (float)(previews - last_previews) * 1000.0f / (float)dt_ms;

        ESP_LOGI(TAG,
                 "fps=%.1f preview=%.1f detect_avg/max=%lu/%lu us recog_avg/max=%lu/%lu us",
                 fps, preview_fps, (unsigned long)detect_avg, (unsigned long)detect_max,
                 (unsigned long)recog_avg, (unsigned long)recog_max);

        // 供诊断页读取：与上面这行日志同一口径
        taskENTER_CRITICAL(&s_stats_lock);
        s_last_fps_detect = fps;
        s_last_fps_preview = preview_fps;
        s_last_detect_avg_us = detect_avg;
        s_last_recog_avg_us = recog_avg;
        taskEXIT_CRITICAL(&s_stats_lock);

        // 内部 RAM 单独一行：largest 决定能否再创建任务/申请大块，face_task 的静态栈
        // 不占堆，所以 largest 变小往往来自 LVGL/云端的一次性分配。
        ESP_LOGI(TAG, "heap 内部=%u 最大块=%u PSRAM=%u (bytes)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        log_stack_watermarks();

        last_frames = frames;
        last_detect_total = detect_total;
        last_recogs = recogs;
        last_recog_total = recog_total;
        last_previews = previews;
        last_us = now_us;
    }
}

esp_err_t sys_monitor_start(void)
{
    if (s_task_handle != NULL)
    {
        return ESP_OK; // 已启动，幂等返回
    }

    // 栈放 PSRAM：监控任务只读统计/堆信息并打日志，不碰 Flash 与模型，属于“可以牺牲
    // 一点延迟”的旁路任务；内部 RAM 要留给识别器首次推理的工作区（WiFi/MQTT 之后
    // 内部只剩几 KB，见 main.c 的内存说明）。PSRAM 栈的任务在 Flash 操作期间会被内核
    // 挂起，本任务不做 Flash 访问，安全。
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        sys_monitor_task, "sys_monitor", 3 * 1024, NULL, 1, &s_task_handle, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        // 回退到内部 RAM：监控只是可观测性功能，绝不能因此让启动失败
        ret = xTaskCreatePinnedToCore(sys_monitor_task, "sys_monitor", 3 * 1024, NULL, 1,
                                      &s_task_handle, 0);
    }
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "创建监控任务失败");
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "系统监控任务已启动（Core0，周期 %d ms）",
             APP_CFG_SYS_MONITOR_PERIOD_MS);
    return ESP_OK;
}

void sys_monitor_get_task_stack(const char *name, uint32_t *out_bytes)
{
    if (out_bytes == NULL)
    {
        return;
    }
    *out_bytes = 0;
    if (name == NULL)
    {
        return;
    }

    // 与 log_stack_watermarks() 同样的理由：不能为了取一个任务的栈水位去
    // uxTaskGetSystemState() 长时间持 xKernelLock（会掐住摄像头 DMA 任务）。这里由诊断页
    // 每秒调用一次，所以更必须用“按名取句柄 + 单任务水位”的方式。
    char truncated[configMAX_TASK_NAME_LEN];
    make_task_name(name, truncated);
    const TaskHandle_t handle = xTaskGetHandle(truncated);
    if (handle != NULL)
    {
        *out_bytes =
            (uint32_t)(uxTaskGetStackHighWaterMark(handle) * sizeof(StackType_t));
    }
}

void sys_monitor_get_status(sys_monitor_status_t *out)
{
    if (out == NULL)
    {
        return;
    }
    memset(out, 0, sizeof(*out));

    taskENTER_CRITICAL(&s_stats_lock);
    out->fps_detect = s_last_fps_detect;
    out->fps_preview = s_last_fps_preview;
    out->detect_avg_us = s_last_detect_avg_us;
    out->recog_avg_us = s_last_recog_avg_us;
    out->detect_max_us = s_detect_us_max;
    taskEXIT_CRITICAL(&s_stats_lock);

    out->heap_internal_free =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->heap_internal_largest =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->heap_psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
}

void sys_monitor_report_frame(uint32_t detect_us)
{
    taskENTER_CRITICAL(&s_stats_lock);
    s_frame_count++;
    s_detect_us_total += detect_us;
    if (detect_us > s_detect_us_max)
    {
        s_detect_us_max = detect_us;
    }
    taskEXIT_CRITICAL(&s_stats_lock);
}

void sys_monitor_report_recognize(uint32_t recognize_us)
{
    taskENTER_CRITICAL(&s_stats_lock);
    s_recognize_count++;
    s_recognize_us_total += recognize_us;
    if (recognize_us > s_recognize_us_max)
    {
        s_recognize_us_max = recognize_us;
    }
    taskEXIT_CRITICAL(&s_stats_lock);
}
