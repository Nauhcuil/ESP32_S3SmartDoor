/**
 * @file    time_sync.c
 * @brief   SNTP 对时实现（立即模式 + 3 个服务器 + 15 min 周期）。
 *
 * @note    1. 立即模式（SNTP_SYNC_MODE_IMMED）而不是平滑模式：门禁日志的时间戳要么准确，
 *             要么明显不对；平滑模式在首次可差几分钟，日志会错乱。
 *          2. 对时回调运行在 lwIP 的 tcpip 线程，只做置位与日志，不做任何阻塞操作。
 *          3. 服务器顺序是"国内可达优先"：阿里云 NTP 在国内公网最稳，
 *             pool.ntp.org 作为出国/换环境时的兜底。
 *          4. 只接受合理时间（> CLOCK_VALID_EPOCH）：防止解析异常把时钟拨回 1970。
 */

#include "time_sync.h"

#include "app_config.h"
#include "app_events.h"
#include "clock_service.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// 服务器索引不能超过 lwIP 配置的上限（sdkconfig: CONFIG_LWIP_SNTP_MAX_SERVERS=3）
#if CONFIG_LWIP_SNTP_MAX_SERVERS < APP_CFG_SNTP_MAX_SERVERS
#error "CONFIG_LWIP_SNTP_MAX_SERVERS 必须 >= APP_CFG_SNTP_MAX_SERVERS（3）"
#endif

static const char *TAG = "time_sync"; // 日志标签

/** @brief 是否已启动（幂等） */
static bool s_started = false;
/** @brief 本次上电是否对时成功 */
static volatile bool s_synced = false;

/**
 * @brief 对时成功回调（lwIP tcpip 线程上下文）
 * @param tv 服务器返回的时间（UTC）
 */
static void sync_cb(struct timeval *tv)
{
    if (tv == NULL || tv->tv_sec < CLOCK_VALID_EPOCH)
    {
        ESP_LOGW(TAG, "丢弃异常时间：%lld", (long long)(tv != NULL ? tv->tv_sec : 0));
        return;
    }

    clock_service_mark_synced(tv); // 置位"已对时"→ 主页时钟显示到秒
    s_synced = true;
    xEventGroupSetBits(g_sys_events, SYS_EVENT_TIME_VALID);

    char buf[32];
    ESP_LOGI(TAG, "对时成功：%s（%lld）", clock_service_format_time(buf, sizeof(buf), true),
             (long long)tv->tv_sec);
}

esp_err_t time_sync_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, APP_CFG_SNTP_SERVER_0);
    esp_sntp_setservername(1, APP_CFG_SNTP_SERVER_1);
    esp_sntp_setservername(2, APP_CFG_SNTP_SERVER_2);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_sync_interval(APP_CFG_SNTP_SYNC_INTERVAL_MS);
    esp_sntp_set_time_sync_notification_cb(sync_cb);
    esp_sntp_init();

    s_started = true;
    ESP_LOGI(TAG, "SNTP 已启动：%s / %s / %s，周期 %u 分钟", APP_CFG_SNTP_SERVER_0,
             APP_CFG_SNTP_SERVER_1, APP_CFG_SNTP_SERVER_2,
             (unsigned)(APP_CFG_SNTP_SYNC_INTERVAL_MS / 60000));
    return ESP_OK;
}

bool time_sync_is_synced(void)
{
    return s_synced;
}
