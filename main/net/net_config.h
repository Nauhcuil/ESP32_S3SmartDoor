/**
 * @file    net_config.h
 * @brief   网络参数持久化：WiFi 账号与巴法云私钥/主题/端口（全部存 NVS）。
 *
 * @note    - 命名空间 APP_CFG_NVS_NET_NAMESPACE（"net_cfg"），键名见 app_config.h。
 *          - 与 sys/app_nvs.h 的做法一致：读失败或键不存在时返回"未配置"，
 *            不在启动阶段擦除 NVS（只烧 app 时参数得以保留）。
 *          - **任何凭据都不允许硬编码在源码里**：这里只负责读写，值来自配网表单。
 *          - 巴法云参数允许"只有私钥、主题留空"（部分用户只用单主题），
 *            是否可用由调用方用 net_config_has_bemfa() 判断。
 */

#ifndef __NET_CONFIG_H__
#define __NET_CONFIG_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief WiFi 名称最大长度（含结束符：32 字符 + '\0'） */
#define NET_CFG_SSID_MAX 33
/** @brief WiFi 密码最大长度（含结束符：64 字符 + '\0'） */
#define NET_CFG_PASS_MAX 65
/** @brief 巴法云私钥最大长度（私钥为 32 位十六进制串，留余量） */
#define NET_CFG_UID_MAX 48
/** @brief 单个巴法云主题最大长度（主题名通常 ≤16 字符） */
#define NET_CFG_TOPIC_MAX 24
/** @brief 巴法云主题串最大长度：**逗号分隔的多个主题**（最多 4 个，见 bemfa_topic_slot_t）
 *         例："doorcam,doorcam_cmd,doorcam_status,doorcam_stranger" */
#define NET_CFG_TOPIC_CSV_MAX 100

/**
 * @brief 读取 WiFi 凭据
 * @param ssid     输出 WiFi 名称（可为 NULL）
 * @param ssid_size ssid 缓冲大小
 * @param pass     输出密码（可为 NULL）
 * @param pass_size pass 缓冲大小
 * @return ESP_OK 读到有效 ssid；ESP_ERR_NVS_NOT_FOUND 未配置；其余为 NVS 错误
 */
esp_err_t net_config_get_wifi(char *ssid, size_t ssid_size, char *pass, size_t pass_size);

/**
 * @brief 写入 WiFi 凭据
 * @param ssid 名称（非空，≤32 字符）
 * @param pass 密码（可为空串，表示开放网络）
 * @return ESP_OK / ESP_ERR_INVALID_ARG / NVS 错误
 */
esp_err_t net_config_set_wifi(const char *ssid, const char *pass);

/**
 * @brief 是否已配置 WiFi
 */
bool net_config_has_wifi(void);

/**
 * @brief 清除 WiFi 凭据（恢复出厂/重新配网前可选调用）
 */
esp_err_t net_config_clear_wifi(void);

/**
 * @brief 读取巴法云参数
 * @param uid      输出私钥（可为 NULL）
 * @param uid_size 缓冲大小
 * @param topic    输出主题串（逗号分隔，可为 NULL），缓冲至少 NET_CFG_TOPIC_CSV_MAX
 * @param topic_size 缓冲大小
 * @param port     输出端口（可为 NULL）；未配置时给 APP_CFG_BEMFA_PORT_PLAIN
 * @return ESP_OK 读到有效私钥；ESP_ERR_NVS_NOT_FOUND 未配置
 */
esp_err_t net_config_get_bemfa(char *uid, size_t uid_size, char *topic, size_t topic_size,
                               uint16_t *port);

/**
 * @brief 写入巴法云参数
 * @param uid   私钥（非空）
 * @param topic 主题串（逗号分隔，可为空串；长度 ≤ NET_CFG_TOPIC_CSV_MAX-1）
 * @param port  端口（9501 明文 / 9503 TLS，其它值按 9501 处理）
 */
esp_err_t net_config_set_bemfa(const char *uid, const char *topic, uint16_t port);

/**
 * @brief 是否已配置巴法云私钥
 */
bool net_config_has_bemfa(void);

#ifdef __cplusplus
}
#endif

#endif /* __NET_CONFIG_H__ */
