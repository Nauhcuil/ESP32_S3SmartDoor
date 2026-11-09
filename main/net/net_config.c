/**
 * @file    net_config.c
 * @brief   网络参数 NVS 读写实现（WiFi 账号、巴法云私钥/主题/端口）。
 *
 * @note    每个接口内部自行 nvs_open/nvs_close，不缓存句柄：调用频率极低
 *          （开机一次、配网保存一次），句柄缓存反而要考虑多任务并发。
 *          NVS 读写不受 g_log_mutex 保护（NVS 自身线程安全）。
 */

#include "net_config.h"

#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "net_config"; // 日志标签

/**
 * @brief 读取一个字符串键（不存在视为未配置）
 * @param key      键名
 * @param out      输出缓冲（可为 NULL，此时只判断是否存在）
 * @param out_size 缓冲大小
 * @return ESP_OK / ESP_ERR_NVS_NOT_FOUND / 其他 NVS 错误
 */
static esp_err_t read_str(const char *key, char *out, size_t out_size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_CFG_NVS_NET_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return err; // 命名空间不存在 = 从未配网
    }

    char tmp[NET_CFG_PASS_MAX]; // 按最长键（WiFi 密码 65）留缓冲，各键共用
    size_t len = sizeof(tmp);
    err = nvs_get_str(h, key, tmp, &len);
    nvs_close(h);

    if (err != ESP_OK)
    {
        return err;
    }
    if (out != NULL && out_size > 0)
    {
        strncpy(out, tmp, out_size - 1);
        out[out_size - 1] = '\0';
    }
    return ESP_OK;
}

/**
 * @brief 写入一个字符串键
 */
static esp_err_t write_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_CFG_NVS_NET_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "打开 NVS 命名空间失败：%s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t net_config_get_wifi(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    esp_err_t err = read_str(APP_CFG_NVS_KEY_WIFI_SSID, ssid, ssid_size);
    if (err != ESP_OK)
    {
        return err;
    }
    if (ssid != NULL && (ssid_size == 0 || ssid[0] == '\0'))
    {
        return ESP_ERR_NVS_NOT_FOUND; // 空名称等同于未配置
    }
    if (pass != NULL && pass_size > 0)
    {
        // 密码允许为空（开放网络），读不到时填空串而不是返回错误
        if (read_str(APP_CFG_NVS_KEY_WIFI_PASS, pass, pass_size) != ESP_OK)
        {
            pass[0] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t net_config_set_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > NET_CFG_SSID_MAX - 1)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass != NULL && strlen(pass) > NET_CFG_PASS_MAX - 1)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = write_str(APP_CFG_NVS_KEY_WIFI_SSID, ssid);
    if (err == ESP_OK)
    {
        err = write_str(APP_CFG_NVS_KEY_WIFI_PASS, pass != NULL ? pass : "");
    }
    ESP_LOGI(TAG, "WiFi 凭据已保存：%s（%s）", ssid, esp_err_to_name(err));
    return err;
}

bool net_config_has_wifi(void)
{
    char ssid[NET_CFG_SSID_MAX];
    return net_config_get_wifi(ssid, sizeof(ssid), NULL, 0) == ESP_OK && ssid[0] != '\0';
}

esp_err_t net_config_clear_wifi(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_CFG_NVS_NET_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    (void)nvs_erase_key(h, APP_CFG_NVS_KEY_WIFI_SSID);
    (void)nvs_erase_key(h, APP_CFG_NVS_KEY_WIFI_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t net_config_get_bemfa(char *uid, size_t uid_size, char *topic, size_t topic_size,
                               uint16_t *port)
{
    esp_err_t err = read_str(APP_CFG_NVS_KEY_BEMFA_UID, uid, uid_size);
    if (err != ESP_OK)
    {
        if (port != NULL)
        {
            *port = APP_CFG_BEMFA_PORT_PLAIN;
        }
        return err;
    }
    if (uid != NULL && (uid_size == 0 || uid[0] == '\0'))
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    if (topic != NULL && topic_size > 0)
    {
        if (read_str(APP_CFG_NVS_KEY_BEMFA_TOPIC, topic, topic_size) != ESP_OK)
        {
            topic[0] = '\0'; // 只填私钥不填主题是允许的
        }
    }

    if (port != NULL)
    {
        *port = APP_CFG_BEMFA_PORT_PLAIN;
        nvs_handle_t h;
        if (nvs_open(APP_CFG_NVS_NET_NAMESPACE, NVS_READONLY, &h) == ESP_OK)
        {
            uint16_t v = 0;
            if (nvs_get_u16(h, APP_CFG_NVS_KEY_BEMFA_PORT, &v) == ESP_OK && v != 0)
            {
                *port = v;
            }
            nvs_close(h);
        }
    }
    return ESP_OK;
}

esp_err_t net_config_set_bemfa(const char *uid, const char *topic, uint16_t port)
{
    if (uid == NULL || uid[0] == '\0' || strlen(uid) > NET_CFG_UID_MAX - 1)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (topic != NULL && strlen(topic) > NET_CFG_TOPIC_CSV_MAX - 1)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = write_str(APP_CFG_NVS_KEY_BEMFA_UID, uid);
    if (err == ESP_OK)
    {
        err = write_str(APP_CFG_NVS_KEY_BEMFA_TOPIC, topic != NULL ? topic : "");
    }
    if (err == ESP_OK)
    {
        nvs_handle_t h;
        err = nvs_open(APP_CFG_NVS_NET_NAMESPACE, NVS_READWRITE, &h);
        if (err == ESP_OK)
        {
            uint16_t p = (port == APP_CFG_BEMFA_PORT_TLS) ? APP_CFG_BEMFA_PORT_TLS
                                                          : APP_CFG_BEMFA_PORT_PLAIN;
            err = nvs_set_u16(h, APP_CFG_NVS_KEY_BEMFA_PORT, p);
            if (err == ESP_OK)
            {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
    }
    // 只打主题与端口：私钥等同于账号凭据，串口日志/抓拍截图都可能外泄，不打印
    ESP_LOGI(TAG, "巴法云参数已保存（主题 %s，端口 %u）：%s", topic != NULL ? topic : "",
             (unsigned)port, esp_err_to_name(err));
    return err;
}

bool net_config_has_bemfa(void)
{
    char uid[NET_CFG_UID_MAX];
    return net_config_get_bemfa(uid, sizeof(uid), NULL, 0, NULL) == ESP_OK && uid[0] != '\0';
}
