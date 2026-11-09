/**
 * @file    net_service.c
 * @brief   网络门面实现：启动 WiFi + SNTP，并把内部状态翻译成界面文案。
 *
 * @note    1. 文案统一取自 ui_strings.h（字库按该文件生成），不要在这里写死中文字面量，
 *             否则新字符不会进字库，屏幕上会出现空白。
 *          2. 本模块不持有任何锁：状态查询都是读一个 volatile 枚举或 WiFi 驱动 API。
 *          3. 云端（巴法云）MQTT 由 cloud_service 自己建任务；本模块只在 access_log 上挂
 *             观察者，把新日志的 seq 交给云端队列，并暴露云端状态给诊断页。
 */

#include "net_service.h"

#include <stdio.h>

#include "access_log.h"
#include "cloud_service.h"
#include "esp_log.h"
#include "net_config.h"
#include "time_sync.h"
#include "ui_strings.h"
#include "wifi_manager.h"

static const char *TAG = "net_service"; // 日志标签

/** @brief WiFi 栈是否初始化成功 */
static volatile bool s_net_ready = false;

esp_err_t net_service_init(void)
{
    if (s_net_ready)
    {
        return ESP_OK;
    }

    // 内部 RAM 不足时 esp_wifi_init 只回一个 ESP_ERR_NO_MEM，看不出是哪块分配失败；
    // 排查期把 WiFi/网络栈日志提到 DEBUG，正式固件置 0
#ifndef NET_WIFI_DEBUG_LOG
#define NET_WIFI_DEBUG_LOG 0
#endif
#if NET_WIFI_DEBUG_LOG
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("wifi_init", ESP_LOG_DEBUG);
    esp_log_level_set("esp_netif", ESP_LOG_DEBUG);
#endif

    ESP_LOGI(TAG, "网络初始化开始（WiFi + SNTP）"); // 先打一行：内部 RAM 不足时卡在 WiFi 初始化也有迹可循
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi 初始化失败：%s（网络功能不可用，其余功能照常）", esp_err_to_name(err));
        return err;
    }
    s_net_ready = true;

    // SNTP 立即启动：网络未通时请求会失败并按周期重试，不需要等 WiFi 连接事件
    if (time_sync_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "SNTP 启动失败（时间将用编译时刻兜底）");
    }

    // 云端（巴法云 MQTT）：未配置私钥时只占一个状态枚举，不建任务、不占 RAM。
    // 门禁日志写入后由 access_log 的观察者回调把 seq 交给云端队列（非阻塞）。
    access_log_set_observer(cloud_service_notify_log);
    (void)cloud_service_init();

    char ap[24];
    wifi_manager_get_ap_ssid(ap, sizeof(ap));
    ESP_LOGI(TAG, "网络已启动：状态=%d 配网热点=%s", (int)net_service_get_status(), ap);
    return ESP_OK;
}

net_status_t net_service_get_status(void)
{
    if (!s_net_ready)
    {
        return NET_STATUS_DISABLED;
    }

    switch (wifi_manager_get_state())
    {
    case WIFI_MGR_CONNECTED:
        return NET_STATUS_CONNECTED;
    case WIFI_MGR_CONNECTING:
        return NET_STATUS_CONNECTING;
    case WIFI_MGR_PROVISIONING:
        return NET_STATUS_PROVISIONING;
    case WIFI_MGR_NO_CONFIG:
        return NET_STATUS_UNSET;
    case WIFI_MGR_FAILED:
    default:
        return NET_STATUS_CONNECTING;
    }
}

const char *net_service_status_text(void)
{
    switch (net_service_get_status())
    {
    case NET_STATUS_CONNECTED:
        return UI_TXT_CONNECTED;
    case NET_STATUS_PROVISIONING:
        return UI_TXT_PROVISIONING;
    case NET_STATUS_CONNECTING:
        return UI_TXT_STATUS_NET_CONNECT;
    case NET_STATUS_UNSET:
        return UI_TXT_STATUS_NET_UNSET;
    case NET_STATUS_DISABLED:
    default:
        return UI_TXT_NOT_ENABLED;
    }
}

net_cloud_status_t net_service_get_cloud_status(void)
{
    if (!s_net_ready)
    {
        return NET_CLOUD_DISABLED;
    }
    switch (cloud_service_get_state())
    {
    case CLOUD_STATE_CONNECTED:
        return NET_CLOUD_CONNECTED;
    case CLOUD_STATE_CONNECTING:
        return NET_CLOUD_CONNECTING;
    case CLOUD_STATE_UNSET:
        return NET_CLOUD_UNSET;
    case CLOUD_STATE_DISABLED:
    default:
        return NET_CLOUD_DISABLED;
    }
}

const char *net_service_cloud_text(void)
{
    switch (net_service_get_cloud_status())
    {
    case NET_CLOUD_CONNECTED:
        return UI_TXT_ONLINE;
    case NET_CLOUD_CONNECTING:
        return UI_TXT_DISCONNECTED;
    case NET_CLOUD_UNSET:
        return UI_TXT_STATUS_NET_UNSET;
    case NET_CLOUD_DISABLED:
    default:
        return UI_TXT_NOT_ENABLED;
    }
}

void net_service_summary(char *buf, size_t size)
{
    if (buf == NULL || size == 0)
    {
        return;
    }

    switch (net_service_get_status())
    {
    case NET_STATUS_CONNECTED:
    {
        const int rssi = (int)net_service_get_rssi();
        if (rssi != 0)
        {
            snprintf(buf, size, "%s %d dBm", UI_TXT_CONNECTED, rssi);
        }
        else
        {
            snprintf(buf, size, "%s", UI_TXT_CONNECTED);
        }
        break;
    }
    case NET_STATUS_PROVISIONING:
    {
        char ap[24];
        net_service_get_ap_ssid(ap, sizeof(ap));
        snprintf(buf, size, "%s %s", UI_TXT_PROVISIONING, ap);
        break;
    }
    default:
        snprintf(buf, size, "%s", net_service_status_text());
        break;
    }
}

esp_err_t net_service_start_provisioning(void)
{
    if (!s_net_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return wifi_manager_start_provisioning();
}

void net_service_get_ap_ssid(char *buf, size_t size)
{
    wifi_manager_get_ap_ssid(buf, size);
}

int8_t net_service_get_rssi(void)
{
    return wifi_manager_get_rssi();
}
