/**
 * @file    sys_monitor.h
 * @brief   系统运行监控：周期打印流水线 fps、AI 耗时、堆内存与关键任务栈水位。
 *
 * @note    - 业务任务通过 sys_monitor_report_frame() / sys_monitor_report_recognize()
 *            上报耗时，监控任务本身不做任何推理或硬件访问，开销可忽略。
 *          - 统计窗口由 APP_CFG_SYS_MONITOR_PERIOD_MS 决定（默认 5 秒）。
 *          - 在 app_main 中于启动业务任务前调用 sys_monitor_start()。
 */

#ifndef __SYS_MONITOR_H__
#define __SYS_MONITOR_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动系统监控任务（Core0，优先级 1，栈 3KB），幂等
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t sys_monitor_start(void);

/**
 * @brief 打印一次帧率统计（采集 fps、送显示 fps、检测 fps），幂等
 * @param timeout_ms 从现在起统计多久后打印（毫秒），到时自动关闭
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 窗口过长；ESP_ERR_NO_MEM 创建定时器失败
 * @note  用一次性 esp_timer 定时回调统计：回调运行在 esp_timer 任务里（不在 Core0
 *        业务线程中），只做计数，开销可忽略。比在各业务任务里自己数帧更准确：
 *        摄像头取帧（esp_camera_fb_get 在 Core1 上阻塞）与 face_task 推理可以并行，
 *        单看其中任何一个都不能代表用户看到的流畅度（实测采集 12 fps 时送显示仍可能
 *        只有 4 fps）。调试/观测用，正式固件可不调用。
 */
esp_err_t sys_monitor_start_fps_window(uint32_t timeout_ms);

/**
 * @brief 提前关闭帧率统计窗口（不打印），幂等
 */
void sys_monitor_stop_fps_window(void);

/**
 * @brief 帧率统计窗口是否还在运行
 * @return true 已运行且未超时
 */
bool sys_monitor_fps_window_active(void);

/**
 * @brief 在窗口内记录一次“摄像头取帧”（由采集任务每帧调用）
 */
void sys_monitor_count_capture(void);

/**
 * @brief 在窗口内记录一次“帧送显示队列”（由 face_task 每帧调用）
 */
void sys_monitor_count_display(void);

/**
 * @brief 上报一帧的检测耗时（MSR01 + MNP01 合计）
 * @param detect_us 检测耗时（微秒）
 */
void sys_monitor_report_frame(uint32_t detect_us);

/**
 * @brief 上报一次人脸识别的耗时（特征提取 + 比对）
 * @param recognize_us 识别耗时（微秒）
 */
void sys_monitor_report_recognize(uint32_t recognize_us);

/** @brief 一次性读取的运行状态快照（诊断页用，内容与 5 s 日志的口径一致） */
typedef struct
{
    float fps_detect;      /**< 检测帧率（受 200 ms 节流，正常 4~5） */
    float fps_preview;     /**< 上屏预览帧率（ui_preview 计数，正常 ~13） */
    uint32_t detect_avg_us; /**< 自上次周期以来的平均检测耗时（微秒） */
    uint32_t detect_max_us; /**< 单帧最大检测耗时（微秒），开机以来 */
    uint32_t recog_avg_us;  /**< 自上次周期以来的平均识别耗时（微秒） */
    uint32_t heap_internal_free;  /**< 内部 RAM 可用（字节） */
    uint32_t heap_internal_largest; /**< 内部 RAM 最大连续块（字节） */
    uint32_t heap_psram_free;     /**< PSRAM 可用（字节） */
    uint32_t uptime_s;            /**< 开机时长（秒） */
} sys_monitor_status_t;

/**
 * @brief 读取运行状态快照
 * @param out 输出（不能为 NULL）
 * @note  帧率与平均耗时按“上次周期结束到现在的增量”换算，与日志里 fps= 的含义相同；
 *        监控任务未启动或自上次周期不足 1 ms 时帧率为 0。
 */
void sys_monitor_get_status(sys_monitor_status_t *out);

/**
 * @brief 读取指定任务的剩余栈空间
 * @param name      任务名（按前缀匹配，名称超长会被 FreeRTOS 截断）
 * @param out_bytes 剩余栈字节数（找不到任务或 out 为 NULL 时写 0）
 * @note  需要 CONFIG_FREERTOS_USE_TRACE_FACILITY；调用开销为一次全任务枚举，
 *        诊断页每秒调用几次可以接受，不要在业务热路径里使用。
 */
void sys_monitor_get_task_stack(const char *name, uint32_t *out_bytes);

#ifdef __cplusplus
}
#endif

#endif /* __SYS_MONITOR_H__ */
