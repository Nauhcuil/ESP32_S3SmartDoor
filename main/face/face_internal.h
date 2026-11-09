/**
 * @file    face_internal.h
 * @brief   face_service 与 face_task 之间的内部协议（不对外暴露）。
 *
 * @note    - 命令通过 FreeRTOS 队列传递（只传 POD），响应通过一组受 s_api_mutex
 *            保护的全局槽位回传，保证同一时刻只有一个未完成的请求。
 *            - face_task 完成命令后调用 face_reply_* 填入响应，最后 face_reply_done()
 *            释放信号量；调用方在 face_service.c 内等待该信号量。
 *            - 本头文件只允许 face/ 目录内部包含。
 */

#ifndef __FACE_INTERNAL_H__
#define __FACE_INTERNAL_H__

#include "face_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 人脸识别模型的量化位宽（M6 起用 8-bit 模型）
 *
 * @note  同时作为 NVS `model_ver` 的值：S16 = 16、S8 = 8。换回 16-bit 模型时只需要
 *        改 face_task.cpp 的 include/类型并把这里改回 16，启动逻辑会自动清库。
 *        S8 与 S16 的特征向量不兼容，且 S8 相似度整体偏低，阈值默认值也不同。
 */
#define FACE_MODEL_VERSION 8

/** @brief 内部命令类型 */
typedef enum
{
    FACE_CMD_RECOGNIZE = 1, /**< 识别一帧 */
    FACE_CMD_ENROLL,        /**< 注册当前人脸 */
    FACE_CMD_DELETE,        /**< 删除指定 ID */
    FACE_CMD_CLEAR,         /**< 清空所有人脸 */
    FACE_CMD_LIST,          /**< 读取人脸库 */
    FACE_CMD_SET_THRESHOLD, /**< 设置识别阈值 */
} face_cmd_type_t;

/** @brief 命令负载（POD） */
typedef struct
{
    face_cmd_type_t type;
    char name[32]; /**< ENROLL 名称 */
    int id;        /**< DELETE 目标 ID */
    float value;   /**< SET_THRESHOLD 阈值 */
} face_cmd_t;

/** @brief 人脸库列表响应容量（storage 分区最多约 125 个 ID） */
#define FACE_REPLY_LIST_MAX 128

/** @brief face_task 启动（由 face_service_init 调用） */
esp_err_t face_task_start(QueueHandle_t camera_queue, QueueHandle_t display_queue,
                          QueueHandle_t cmd_queue);

/** @brief face_task 读取命令队列（阻塞等待由 face_task 自己控制） */
QueueHandle_t face_task_cmd_queue(void);

/* ---- 以下由 face_service.c 实现，供 face_task 回填响应 ---- */

/** @brief 回填识别/注册结果 */
void face_reply_result(esp_err_t err, const face_result_t *result, int out_id);

/** @brief 回填人脸库列表 */
void face_reply_list(esp_err_t err, const face_record_t *list, size_t n);

/** @brief 回填通用错误码（删除/清空/阈值） */
void face_reply_err(esp_err_t err);

/** @brief 通知调用方命令已完成 */
void face_reply_done(void);

/** @brief face_task 用：读取当前策略（无需命令往返） */
face_strategy_t face_task_strategy(void);

/** @brief face_task 用：读取当前阈值（face_service 内缓存） */
float face_task_threshold(void);

/** @brief face_task 用：读取活体开关（face_service 内缓存） */
bool face_task_liveness_enabled(void);

/**
 * @brief face_task 用：启动时是否需要清空人脸库（模型版本变化）
 * @return true 表示 NVS 里记录的模型版本与当前固件不一致
 *
 * @note  esp-dl 的人脸库记录带模型校验码，S16 ↔ S8 互换后 `set_ids_from_flash()`
 *        一条也读不出来。face_service_init() 发现版本不一致时置位本标志，
 *        face_task 在 **写 Flash 安全期**（首帧前后均可，此处放在模型初始化后）
 *        调用 `clear_id(true)` 把旧库清掉，避免出现“分区有数据但库里是空的”。
 */
bool face_task_clear_db_on_start(void);

/* ---- 以下由 face_service.c 实现，供 face_task 写回快照 ---- */

/** @brief 更新当前人脸状态快照（每帧调用） */
void face_service_update_face_state(const face_face_state_t *state);

/** @brief 发布最近一次识别结果（每次识别调用） */
void face_service_publish_result(const face_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* __FACE_INTERNAL_H__ */
