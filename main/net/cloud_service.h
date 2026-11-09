/**
 * @file    cloud_service.h
 * @brief   巴法云云端服务（M4）：MQTT 连接、门禁事件上报、心跳与远程命令。
 *
 * @note    - 参数（私钥 / 主题 / 端口）全部来自 NVS，**源码里不出现任何凭据**；
 *            未配置私钥时本模块不创建任务、不占 RAM，诊断页显示“未配置”。
 *          - 设备到云端的通道只有一条主题：上行用日志 JSON 行原样发送（type=access/stranger），
 *            外加 30 s 一次的状态心跳（QoS0）；下行是命令 JSON（cmd/token/value）。
 *          - 网络断开期间事件不丢：门禁日志已经落盘，cloud_service 只记住“待上报的 seq”，
 *            重连后按 200 ms 间隔补发（QoS1，由 esp-mqtt 的 outbox 负责重传）。
 *          - 远程开门要求 `token` 等于管理员 PIN（NVS），并限速 3 次/分钟，防止误触与暴力尝试。
 *          - 所有接口可从任意任务调用；内部只有队列与事件回调，不持 LVGL 锁、不阻塞调用方。
 */

#ifndef __CLOUD_SERVICE_H__
#define __CLOUD_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 云端状态（映射到界面文案） */
typedef enum
{
    CLOUD_STATE_DISABLED = 0, /**< 未启用（未初始化 / 创建失败） */
    CLOUD_STATE_UNSET,        /**< 未配置巴法云私钥 */
    CLOUD_STATE_CONNECTING,   /**< 正在连接 / 等待重连 */
    CLOUD_STATE_CONNECTED,    /**< MQTT 已连接 */
} cloud_state_t;

/**
 * @brief 初始化云端服务（读到私钥时创建 MQTT 客户端与 cloud_task，不阻塞）
 * @return ESP_OK 已启用或未配置（均视为正常）；ESP_ERR_NO_MEM 资源不足
 * @note  必须在 app_events_init / net_config 可用之后调用；WiFi 未连上时 MQTT 客户端
 *        自己按 5 s 间隔重试，不影响启动。
 */
esp_err_t cloud_service_init(void);

/**
 * @brief 是否配置了巴法云（私钥非空）
 */
bool cloud_service_is_configured(void);

/**
 * @brief 当前云端状态
 */
cloud_state_t cloud_service_get_state(void);

/**
 * @brief 暂停/继续云端（进入配网时调用）
 * @param pause true：停掉并释放 MQTT 客户端，之后不再自动创建；false：恢复
 * @note  **配网前必须 pause**：MQTT 任务栈 4 KB + 收发缓冲在配网期与 HTTP 门户争
 *        只剩 ~4 KB 的内部 RAM，门户会以 ESP_ERR_HTTPD_TASK 启动失败（手机连上热点
 *        却打不开 192.168.4.1）。配网保存参数后设备会重启，重启即恢复正常。
 */
void cloud_service_pause(bool pause);

/**
 * @brief 通知“有新的门禁事件待上报”
 * @param seq  日志序号
 * @param type 事件类型（access/stranger/...），仅用于日志，可为 NULL
 * @note  非阻塞：队列满时丢弃本次上报（日志文件里仍有记录，不会造成数据丢失，
 *        只是这一条不会实时推送；重连补发只覆盖队列里记住的序号）。
 */
void cloud_service_notify_log(uint32_t seq, const char *type);

#ifdef __cplusplus
}
#endif

#endif /* __CLOUD_SERVICE_H__ */
