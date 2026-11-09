/**
 * @file    net_service.h
 * @brief   网络门面：网络状态查询（设置页/诊断页用），并统一持有 WiFi、SNTP 与巴法云云端。
 *
 * @note    - 界面层只依赖本头文件：状态文字由 wifi_manager 的状态映射得到，
 *            诊断页的网络/云端两行直接取这里的结果，不直接碰 esp_wifi。
 *          - 云端（巴法云 MQTT）在 M4 后续步骤接入：net_service_init() 里启动云端任务，
 *            未配置私钥时任务直接退出，诊断页显示"未配置"。
 *          - 全部接口可从任意任务调用，内部不持 LVGL 锁。
 */

#ifndef __NET_SERVICE_H__
#define __NET_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 网络状态（映射到界面文案） */
typedef enum
{
    NET_STATUS_DISABLED = 0, /**< WiFi 栈未启动（初始化失败） */
    NET_STATUS_UNSET,        /**< 未配置 WiFi */
    NET_STATUS_CONNECTING,   /**< 正在连接 / 重连 */
    NET_STATUS_CONNECTED,    /**< 已连接并拿到 IP */
    NET_STATUS_PROVISIONING, /**< 配网模式（热点已开） */
} net_status_t;

/** @brief 云端状态（映射到界面文案） */
typedef enum
{
    NET_CLOUD_DISABLED = 0, /**< 未启用（功能未接入或初始化失败） */
    NET_CLOUD_UNSET,        /**< 未配置巴法云私钥 */
    NET_CLOUD_CONNECTING,   /**< 正在连接 / 等待重连 */
    NET_CLOUD_CONNECTED,    /**< MQTT 已连接 */
} net_cloud_status_t;

/**
 * @brief 初始化网络：WiFi 连接/配网 → 启动 SNTP → 启动云端任务（不阻塞）
 * @return ESP_OK 成功；失败时人脸识别与界面照常工作
 * @note  必须在 nvs_flash_init 与 app_events_init 之后调用；WiFi 需 IDF 网络栈（默认依赖）。
 */
esp_err_t net_service_init(void);

/**
 * @brief 当前网络状态
 */
net_status_t net_service_get_status(void);

/**
 * @brief 网络状态的中文文字（返回 ui_strings.h 中的常量字符串）
 */
const char *net_service_status_text(void);

/**
 * @brief 当前云端状态
 */
net_cloud_status_t net_service_get_cloud_status(void);

/**
 * @brief 云端状态的中文文字
 */
const char *net_service_cloud_text(void);

/**
 * @brief 取当前连接状态的一行摘要（诊断页用），如 "已连接 -48dBm" / "配网模式"
 * @param buf  输出缓冲
 * @param size 缓冲大小
 */
void net_service_summary(char *buf, size_t size);

/**
 * @brief 进入配网模式（设置页"配网模式"按钮，已验密码后由工作线程调用）
 * @return ESP_OK / ESP_ERR_INVALID_STATE（网络未初始化）/ 其他
 */
esp_err_t net_service_start_provisioning(void);

/**
 * @brief 配网热点名（"DoorCam-XXXX"）
 */
void net_service_get_ap_ssid(char *buf, size_t size);

/**
 * @brief 当前 WiFi RSSI（dBm，未连接返回 0）
 */
int8_t net_service_get_rssi(void);

#ifdef __cplusplus
}
#endif

#endif /* __NET_SERVICE_H__ */
