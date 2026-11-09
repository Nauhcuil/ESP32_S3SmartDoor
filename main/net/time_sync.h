/**
 * @file    time_sync.h
 * @brief   SNTP 对时：连上网络后把系统时间校正为真实时间（界面随即显示到秒）。
 *
 * @note    - 必须在 esp_netif_init()（WiFi 初始化）之后调用；SNTP 自己会周期性重试，
 *            网络还没通时请求失败不影响后续成功。
 *          - 对时成功通过 clock_service_mark_synced() 通知时钟模块（界面显示秒），
 *            并置位 g_sys_events 的 SYS_EVENT_TIME_VALID。
 *          - 时区仍是 CST-8（clock_service_init 已设置），SNTP 返回的是 UTC，
 *            localtime_r 按 TZ 换算，无需在回调里手动加 8 小时。
 */

#ifndef __TIME_SYNC_H__
#define __TIME_SYNC_H__

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 SNTP 客户端
 * @return ESP_OK / 其他错误（重复调用返回 ESP_OK）
 * @note   服务器列表见 app_config.h（aliyun → cn.pool → pool），对时周期 15 min。
 */
esp_err_t time_sync_start(void);

/**
 * @brief 本次上电是否已对时成功
 */
bool time_sync_is_synced(void);

#ifdef __cplusplus
}
#endif

#endif /* __TIME_SYNC_H__ */
