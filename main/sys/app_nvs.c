/**
 * @file    app_nvs.c
 * @brief   NVS 业务参数读写实现。
 *
 * @note    - 每次读写都自行打开/关闭句柄，调用频率很低（设置页、启动一次），
 *            不长期持有 NVS 句柄，避免与 WiFi 配置模块争用。
 *            - 写入后调用 nvs_commit()，保证掉电前落盘。
 */

#include "app_nvs.h"

#include <stddef.h>

#include "app_config.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_nvs"; // 日志标签

/**
 * @brief 打开业务命名空间
 */
static nvs_handle_t open_ns(nvs_open_mode_t mode)
{
    nvs_handle_t h = 0;
    const esp_err_t err = nvs_open(APP_CFG_NVS_NAMESPACE, mode, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "打开 NVS 命名空间失败: %s", esp_err_to_name(err));
        return 0;
    }
    return h;
}

float app_nvs_get_threshold(void)
{
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (h == 0)
    {
        return APP_CFG_FACE_THRESHOLD;
    }
    float t = APP_CFG_FACE_THRESHOLD;
    const size_t sz = sizeof(t);
    if (nvs_get_blob(h, APP_CFG_NVS_KEY_THRESHOLD, &t, &sz) != ESP_OK || sz != sizeof(t) ||
        t < -1.0f || t > 1.0f)
    {
        t = APP_CFG_FACE_THRESHOLD;
    }
    nvs_close(h);
    return t;
}

esp_err_t app_nvs_set_threshold(float t)
{
    if (t < -1.0f || t > 1.0f)
    {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h == 0)
    {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    esp_err_t err = nvs_set_blob(h, APP_CFG_NVS_KEY_THRESHOLD, &t, sizeof(t));
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool app_nvs_get_liveness(void)
{
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (h == 0)
    {
        return APP_CFG_LIVENESS_ENABLE != 0;
    }
    uint8_t v = APP_CFG_LIVENESS_ENABLE ? 1 : 0;
    if (nvs_get_u8(h, APP_CFG_NVS_KEY_LIVENESS, &v) != ESP_OK || v > 1)
    {
        v = APP_CFG_LIVENESS_ENABLE ? 1 : 0;
    }
    nvs_close(h);
    return v != 0;
}

esp_err_t app_nvs_set_liveness(bool enable)
{
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h == 0)
    {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    esp_err_t err = nvs_set_u8(h, APP_CFG_NVS_KEY_LIVENESS, enable ? 1 : 0);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool app_nvs_get_snapshot_enabled(void)
{
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (h == 0)
    {
        return true;
    }
    uint8_t v = 1;
    if (nvs_get_u8(h, APP_CFG_NVS_KEY_SNAPSHOT, &v) != ESP_OK || v > 1)
    {
        v = 1;
    }
    nvs_close(h);
    return v != 0;
}

esp_err_t app_nvs_set_snapshot_enabled(bool enable)
{
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h == 0)
    {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    esp_err_t err = nvs_set_u8(h, APP_CFG_NVS_KEY_SNAPSHOT, enable ? 1 : 0);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t app_nvs_get_model_ver(void)
{
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (h == 0)
    {
        return 0; // 0 = 未记录（首次跑新固件或从未注册）
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, APP_CFG_NVS_KEY_MODEL_VER, &v) != ESP_OK)
    {
        v = 0;
    }
    nvs_close(h);
    return v;
}

esp_err_t app_nvs_set_model_ver(uint8_t ver)
{
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h == 0)
    {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    esp_err_t err = nvs_set_u8(h, APP_CFG_NVS_KEY_MODEL_VER, ver);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t app_nvs_get_threshold_rev(void)
{
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (h == 0)
    {
        return 0; // 0 = 从未记录（旧固件），调用方会重置为新默认阈值
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, APP_CFG_NVS_KEY_THR_REV, &v) != ESP_OK)
    {
        v = 0;
    }
    nvs_close(h);
    return v;
}

esp_err_t app_nvs_set_threshold_rev(uint8_t rev)
{
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h == 0)
    {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    esp_err_t err = nvs_set_u8(h, APP_CFG_NVS_KEY_THR_REV, rev);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint32_t app_nvs_get_admin_pin(void)
{
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (h == 0)
    {
        return APP_CFG_ADMIN_PIN_DEFAULT;
    }
    uint32_t pin = APP_CFG_ADMIN_PIN_DEFAULT;
    if (nvs_get_u32(h, APP_CFG_NVS_KEY_ADMIN_PIN, &pin) != ESP_OK || pin > 99999999U)
    {
        pin = APP_CFG_ADMIN_PIN_DEFAULT;
    }
    nvs_close(h);
    return pin;
}

esp_err_t app_nvs_set_admin_pin(uint32_t pin)
{
    if (pin > 99999999U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h == 0)
    {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    esp_err_t err = nvs_set_u32(h, APP_CFG_NVS_KEY_ADMIN_PIN, pin);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
