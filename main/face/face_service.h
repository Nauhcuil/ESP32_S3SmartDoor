/**
 * @file    face_service.h
 * @brief   人脸业务门面（C 接口）：由 UI / 按键 / 网络 / 语音等模块调用，
 *          真正的模型推理全部在 face_task（C++）内部执行。
 *
 * @note    - 设计意图：模型对象（MSR01/MNP01/Recognizer）只能被单一任务持有，
 *            其他模块一律通过命令队列与结果快照访问，避免跨任务重入与栈溢出。
 *            - 命令处理在 face_task 内部按帧循环执行，超时返回 ESP_ERR_TIMEOUT，
 *            不会阻塞调用方超过 timeout_ms。
 *            - 所有接口可在任意任务调用（内部只用队列与互斥量）。
 */

#ifndef __FACE_SERVICE_H__
#define __FACE_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 识别/活体综合动作 */
typedef enum
{
    FACE_ACTION_NONE = 0,        /**< 未识别 */
    FACE_ACTION_OPEN,            /**< 识别成功且活体通过：允许开门 */
    FACE_ACTION_DENY_LIVENESS,   /**< 已注册但活体不通过：拒绝 */
    FACE_ACTION_UNKNOWN,         /**< 未注册人脸（陌生人） */
} face_action_t;

/** @brief 一次识别结果（快照，可由任意任务读取） */
typedef struct
{
    int id;                   /**< 人脸 ID；<0 表示未匹配 */
    char name[32];            /**< 人脸名称（UTF-8） */
    float similarity;         /**< 相似度（-1~1） */
    float liveness;           /**< 活体综合分（0-100）；未启用时为 100 */
    face_action_t action;     /**< 综合动作 */
    int32_t x, y, w, h;       /**< 人脸框（整帧坐标） */
    int track_id;             /**< 轨迹号（每张新人脸轨迹递增） */
    uint64_t ts_ms;           /**< 结果生成时间（毫秒，esp_timer） */
    bool quality_pass;        /**< 本帧人脸质量是否达标 */
} face_result_t;

/** @brief 最近一次被识别人脸的摘要（无人脸时清零），供主页状态卡展示 */
typedef struct
{
    bool face_present;        /**< 当前帧是否检测到人脸 */
    bool quality_pass;        /**< 当前人脸质量是否达标 */
    int track_id;             /**< 当前轨迹号（无人脸为 -1） */
    face_action_t action;     /**< 当前轨迹最近一次识别动作（未识别为 NONE） */
    char name[32];            /**< 当前轨迹识别到的名称（未识别为空串） */
    float similarity;         /**< 相似度 */
    int enrolled_num;         /**< 已注册人数（首页状态卡展示） */
} face_face_state_t;

/** @brief 人脸库条目 */
typedef struct
{
    int id;        /**< 人脸 ID */
    char name[32]; /**< 名称 */
} face_record_t;

/** @brief 识别策略 */
typedef enum
{
    FACE_STRATEGY_MANUAL = 0, /**< 仅在 face_service_recognize() 请求时识别（按键/语音/触摸） */
    FACE_STRATEGY_AUTO,       /**< 自动识别：稳定人脸每 N 帧识别一次（M3 起用于门禁常驻） */
} face_strategy_t;

/** @brief 运行状态（诊断页/状态栏使用） */
typedef struct
{
    bool face_present;      /**< 当前帧是否有人脸 */
    int enrolled_num;       /**< 已注册人数 */
    int track_id;           /**< 当前轨迹号 */
    float fps_ai;           /**< 预留：AI 任务自测帧率（当前为 0） */
    float threshold;        /**< 当前识别阈值 */
    bool liveness_enabled;  /**< 活体是否开启 */
    face_strategy_t strategy; /**< 当前识别策略 */
} face_service_status_t;

/** @brief 识别请求的结果码（通过 face_service_recognize 的 err 输出） */
typedef enum
{
    FACE_OK = 0,             /**< 识别完成（无论是否匹配） */
    FACE_ERR_NO_FACE,        /**< 超时时间内未检测到人脸 */
    FACE_ERR_QUALITY,        /**< 检测到人脸但质量不达标 */
    FACE_ERR_BUSY,           /**< 上一次请求尚未完成 */
    FACE_ERR_TIMEOUT,        /**< 等待超时 */
    FACE_ERR_STATE,          /**< 模块未初始化 */
} face_err_t;

/**
 * @brief 初始化人脸服务：创建模型、加载人脸库、启动 face_task
 * @param camera_queue 摄像头帧队列（元素为 camera_fb_t*），face_task 从中取帧
 * @param display_queue 显示帧队列（元素为 camera_fb_t*），可为 NULL
 * @return ESP_OK 成功；其他值为创建任务/队列失败
 * @note 必须在 camera_init() 之后、sys_monitor_start() 之后调用（face_task 会调用监控上报）。
 */
esp_err_t face_service_init(void *camera_queue, void *display_queue);

/**
 * @brief 读取当前人脸状态（每帧更新，不要求识别发生过）
 * @param out 输出状态
 */
void face_service_get_face_state(face_face_state_t *out);

/**
 * @brief 读取最近一次识别结果
 * @param out 输出快照
 * @param fresh_ms 结果新鲜度（毫秒）；0 表示不限。超过新鲜度时 *out 清零并返回 ESP_ERR_NOT_FOUND
 * @return ESP_OK 有有效结果；ESP_ERR_NOT_FOUND 无结果或结果过期
 */
esp_err_t face_service_get_result(face_result_t *out, uint32_t fresh_ms);

/**
 * @brief 请求一次识别（阻塞直到完成或超时）
 * @param out       识别结果输出（可为 NULL）
 * @param timeout_ms 最长等待时间
 * @return FACE_OK 或具体错误码（FACE_ERR_*）
 * @note 内部等待 face_task 处理；face_task 需要收到有效帧才会执行识别。
 */
face_err_t face_service_recognize(face_result_t *out, uint32_t timeout_ms);

/**
 * @brief 非阻塞地请求一次识别（界面按钮/语音用）
 * @return ESP_OK 请求已入队；ESP_ERR_INVALID_STATE 已有请求在途或服务未初始化
 * @note  **不要在 LVGL 回调里调用阻塞版**（会冻结界面与预览），也不要为此另建任务：
 *        M4 接入网络后内部 RAM 只剩约 6 KB、最大连续块 <4 KB，`xTaskCreate` 一个 4 KB
 *        栈的识别线程会间歇性失败（表现为点“开始识别”毫无反应）。
 *        本接口把命令投给 face_task 后立即返回；结果仍由 face_task 写状态卡，
 *        失败原因由 face_service 在回执时补写。界面用
 *        face_service_recognize_in_flight() 轮询是否结束。
 */
esp_err_t face_service_recognize_async(void);

/**
 * @brief 是否有异步识别在途（供界面轮询）
 */
bool face_service_recognize_in_flight(void);

/**
 * @brief 注册当前人脸
 * @param name       名称（可为 NULL：自动命名 Person_N）
 * @param out_id     输出人脸 ID（可为 NULL）
 * @param timeout_ms 最长等待时间（覆盖采集到质量合格人脸的时间）
 * @return 具体错误码：
 *         - ESP_OK                 录入成功
 *         - ESP_ERR_NOT_FOUND      画面中没有人脸
 *         - ESP_ERR_INVALID_ARG    人脸质量不达标（尺寸/亮度/清晰度/正脸）
 *         - ESP_ERR_INVALID_STATE  该人脸已注册 / 写入人脸库失败
 *         - ESP_FAIL               特征提取失败
 *         - ESP_ERR_TIMEOUT        等待超时
 */
esp_err_t face_service_enroll(const char *name, int *out_id, uint32_t timeout_ms);

/**
 * @brief 删除指定人脸
 * @param id 人脸 ID
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 不存在；ESP_ERR_TIMEOUT 超时
 */
esp_err_t face_service_delete(int id);

/**
 * @brief 清空所有人脸（写 Flash）
 * @return ESP_OK 成功；ESP_ERR_TIMEOUT 超时
 */
esp_err_t face_service_clear_all(void);

/**
 * @brief 读取人脸库列表
 * @param out 输出数组
 * @param cap 数组容量
 * @param n   输出实际条数
 * @return ESP_OK 成功
 */
esp_err_t face_service_list(face_record_t *out, size_t cap, size_t *n);

/**
 * @brief 设置识别阈值（立即生效并写入 NVS）
 * @param t 阈值，有效范围 [-1, 1]，实际使用建议 0.4~0.75
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 越界
 */
esp_err_t face_service_set_threshold(float t);

/**
 * @brief 读取当前识别阈值
 */
float face_service_get_threshold(void);

/**
 * @brief 开关启发式活体检测（写入 NVS）
 * @param enable true 开启
 */
esp_err_t face_service_set_liveness(bool enable);

/**
 * @brief 查询活体是否开启
 */
bool face_service_get_liveness(void);

/**
 * @brief 设置识别策略（只影响 face_task 内部行为，不写 NVS）
 */
void face_service_set_strategy(face_strategy_t strategy);

/**
 * @brief 读取运行状态
 */
void face_service_get_status(face_service_status_t *out);

/**
 * @brief 人脸动作转中文说明（状态栏/日志用）
 */
const char *face_action_text(face_action_t a);

#ifdef __cplusplus
}
#endif

#endif /* __FACE_SERVICE_H__ */
