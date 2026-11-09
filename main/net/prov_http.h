/**
 * @file    prov_http.h
 * @brief   SoftAP 配网门户：设备开热点后，手机浏览器访问 192.168.4.1 填写 WiFi 与巴法云参数。
 *
 * @note    - 不用 BLE/ESP SmartConfig：浏览器表单对普通用户最直观，也不依赖 App。
 *          - 页面是内嵌的 UTF-8 HTML（中文），**不经过 LVGL 字库**，所以这里可以直接写
 *            任意汉字，不受 font_symbols.txt 限制。
 *          - 表单字段：ssid / pass（必填名称）、bemfa_uid / bemfa_topic / bemfa_port（可空）。
 *            巴法云参数留空则不改动原值。
 *          - 保存成功后调用 wifi_manager_apply_credentials()——写 NVS，2 s 后重启。
 */

#ifndef __PROV_HTTP_H__
#define __PROV_HTTP_H__

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动配网 HTTP 服务（默认端口 80）
 * @return ESP_OK / 其他 esp_http_server 错误
 * @note  幂等；服务名 "prov_http"。
 */
esp_err_t prov_http_start(void);

/**
 * @brief 停止配网 HTTP 服务
 */
void prov_http_stop(void);

/**
 * @brief 服务是否在运行
 */
bool prov_http_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* __PROV_HTTP_H__ */
