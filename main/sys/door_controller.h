/**
 * @file    door_controller.h
 * @brief   门禁控制（M3）：模拟开门、开门事件分发给日志/UI/网络（M4）/音频（M5）。
 *
 * @note    - 本项目不做真实继电器：`DOOR_RELAY_GPIO` 默认为 -1，开门动作 = 屏幕覆盖层 +
 *            门禁日志 + 状态栏提示；后续接继电器只需配置 GPIO，本模块负责脉冲输出。
 *          - 覆盖层与视觉识别结果解耦：人脸的“识别成功”只是触发来源之一，
 *            远程（M4）/语音（M5）走同一个入口 `door_controller_open()`。
 *          - 内部维护“开门进行中”状态（SYS_EVENT_UNLOCK_ACTIVE），用于：
 *              * 抑制重复开门事件（同一来源在 DOOR_REPEAT_SUPPRESS_MS 内不重复记录）；
 *              * face_task 在开门期间暂停自动识别，避免同一人反复触发。
 *          - 事件分发用独立的深度 8 队列由 door_task 串行执行：写日志、弹覆盖层都不阻塞调用方。
 */

#ifndef __DOOR_CONTROLLER_H__
#define __DOOR_CONTROLLER_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 开门来源 */
typedef enum
{
    DOOR_SOURCE_FACE = 0, /**< 人脸识别 */
    DOOR_SOURCE_REMOTE,   /**< 远程（巴法云 MQTT，M4） */
    DOOR_SOURCE_VOICE,    /**< 语音命令（M5） */
    DOOR_SOURCE_TEST,     /**< 本地调试/自检 */
} door_source_t;

/** @brief 一次开门事件（日志/云端上报所需字段） */
typedef struct
{
    door_source_t source;
    const char *name;   /**< 姓名（远程/语音可为 NULL） */
    float similarity;   /**< 相似度；<0 表示无 */
    float liveness;     /**< 活体分；<0 表示无 */
    const char *extra;  /**< 附加信息（抓拍文件名等），可为 NULL */
    int track_id;       /**< 人脸轨迹号；非人脸来源为 -1 */
} door_open_info_t;

/** @brief 一次拒绝事件（日志用） */
typedef struct
{
    const char *name;   /**< 识别到的姓名（未匹配为 NULL） */
    float similarity;
    float liveness;
    bool liveness_fail; /**< true = 活体失败；false = 未注册 */
    const char *snap;   /**< 抓拍文件名（可为 NULL） */
    int track_id;
} door_deny_info_t;

/**
 * @brief 初始化门禁控制（创建队列与 door_task），并配置继电器 GPIO（若已定义）
 * @return ESP_OK 成功
 */
esp_err_t door_controller_init(void);

/**
 * @brief 触发一次开门
 * @param info 事件信息（内部复制字符串，调用返回后原字符串即可释放）
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 未初始化；ESP_ERR_NO_MEM 队列满
 */
esp_err_t door_controller_open(const door_open_info_t *info);

/**
 * @brief 记录一次拒绝事件（不开门）
 */
esp_err_t door_controller_deny(const door_deny_info_t *info);

/**
 * @brief 记录一条陌生人来访（日志 type=stranger + 可选抓拍文件名）
 */
esp_err_t door_controller_stranger(const char *snap, int track_id);

/** @brief 当前是否处于开门状态（覆盖层显示期间） */
bool door_controller_is_open(void);

/** @brief 最近一次开门来源（UI 状态栏/播报读取） */
door_source_t door_controller_last_source(void);

/** @brief 开门次数统计（本次上电，供诊断页） */
uint32_t door_controller_open_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __DOOR_CONTROLLER_H__ */
