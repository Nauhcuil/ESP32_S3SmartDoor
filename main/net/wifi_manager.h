/**
 * @file    wifi_manager.h
 * @brief   WiFi 连接管理：STA 自动重连、SoftAP 配网（HTTP 表单写 NVS 后重启）。
 *
 * @note    - 凭据只来自 NVS（见 net_config.h），源码不含任何 SSID/密码。
 *          - 状态通过 g_sys_events 的 SYS_EVENT_WIFI_CONNECTED / SYS_EVENT_PROVISIONING
 *            广播，界面与云端模块读事件位或 wifi_manager_get_state() 均可。
 *          - 连续 APP_CFG_WIFI_RETRY_MAX 次连接失败后自动进入配网模式（APSTA），
 *            这样路由器换了密码/搬家后设备不会"失联"，用户用手机即可重新配网。
 *          - 配网热点名 = APP_CFG_NAME + MAC 后两字节（例如 "DoorCam-A1B2"），无密码，
 *            门户地址固定 http://192.168.4.1 。
 *          - 配网保存后统一重启：WiFi 参数在 esp_wifi_init 之前生效最简单，
 *            也避免"运行时切换 STA 配置"与 MQTT 重连逻辑互相干扰。
 */

#ifndef __WIFI_MANAGER_H__
#define __WIFI_MANAGER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief WiFi 管理状态（界面按此映射中文文案） */
typedef enum
{
    WIFI_MGR_NO_CONFIG = 0, /**< 从未配网（启动即进配网模式） */
    WIFI_MGR_CONNECTING,    /**< 已配置，正在连接/重连 */
    WIFI_MGR_CONNECTED,     /**< 已获取 IP */
    WIFI_MGR_PROVISIONING,  /**< 处于 SoftAP 配网模式 */
    WIFI_MGR_FAILED,        /**< 连接失败且未进入配网（保留状态，正常不会出现） */
} wifi_manager_state_t;

/**
 * @brief 初始化 WiFi（必须是 nvs_flash_init 之后调用）
 * @return ESP_OK 成功；其余表示 WiFi/网络栈初始化失败（人脸识别不受影响）
 * @note  已配置凭据则直接连接；未配置则立刻启动配网热点。整个调用不阻塞等待连接结果。
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 当前状态（任意任务可调用）
 */
wifi_manager_state_t wifi_manager_get_state(void);

/**
 * @brief 是否已连接并拿到 IP
 */
bool wifi_manager_is_connected(void);

/**
 * @brief 是否处于配网模式
 */
bool wifi_manager_is_provisioning(void);

/**
 * @brief 进入配网模式：开热点 + 启动 HTTP 配置门户
 * @return ESP_OK / ESP_ERR_INVALID_STATE（WiFi 未初始化）/ 其他
 * @note  幂等；设置页"配网模式"按钮（验密码后）与自动失败重连都会调用。
 */
esp_err_t wifi_manager_start_provisioning(void);

/**
 * @brief 取配网热点名称（"DoorCam-XXXX"），未进入配网也返回设备固定名
 * @param buf  输出缓冲（至少 24 字节）
 * @param size 缓冲大小
 */
void wifi_manager_get_ap_ssid(char *buf, size_t size);

/**
 * @brief 当前连接的 AP 信号强度
 * @return dBm（负数）；未连接返回 0
 */
int8_t wifi_manager_get_rssi(void);

/**
 * @brief 取当前连接的 SSID
 * @param buf  输出缓冲
 * @param size 缓冲大小
 */
void wifi_manager_get_ssid(char *buf, size_t size);

/**
 * @brief 配网门户保存凭据后调用：写入 NVS 并延时重启
 * @param ssid WiFi 名称
 * @param pass 密码（空串 = 开放网络）
 * @return ESP_OK 已保存并安排重启
 * @note  由 prov_http 的 POST 处理函数调用，重启延时 2 s 让 HTTP 响应先发出去。
 */
esp_err_t wifi_manager_apply_credentials(const char *ssid, const char *pass);

/**
 * @brief 暂停/恢复 WiFi 主动连接（配网热点不受影响）
 * @param paused true = 暂停（不发起 esp_wifi_connect，重连定时器也跳过）
 *               false = 恢复：若已配置凭据且尚未连接，立即发起一次连接
 *
 * @note  为什么需要它：esp-sr 的 MultiNet `create()`（构建 FST）会用掉**全部**
 *        剩余内部 RAM（实测那 4 s 窗口里内部空闲只剩几十字节）。若 WiFi 恰好
 *        在这个窗口里连接成功，esp_coex 需要新建合法定时器（ets_timer_setfn →
 *        esp_timer_create），申请不到就 `ESP_ERROR_CHECK` abort 复位——现场是
 *        “有时开机几秒就重启”。
 *        正确用法：`net_service_init()` 之前置 true，`voice_assistant_init()`
 *        与 `voice_prompt_init()` 都完成后再置 false。WiFi 的驱动缓冲在
 *        esp_wifi_init 阶段就已分配好，暂停只影响“发起连接”这个动作。
 */
void wifi_manager_set_connect_paused(bool paused);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_MANAGER_H__ */
