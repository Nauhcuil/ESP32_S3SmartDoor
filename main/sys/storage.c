/**
 * @file    storage.c
 * @brief   SPIFFS 挂载实现：`spiffs` 分区 2 MB，挂到 `/spiffs`。
 *
 * @note    - 首次挂载失败时尝试格式化（format_if_mount_failed），之后仍失败则
 *            只告警：识别/开门是主业务，不应因文件系统不可用而停机。
 *          - max_files=8：日志 1 ~ 4 个文件 + 抓拍 1 个 + 备用。
 *          - 目录用 mkdir() 创建，重复创建返回 EEXIST，属正常。
 */

#include "storage.h"

#include <stdio.h>

#include "app_config.h"
#include "app_events.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "storage";

static bool s_ready = false; // 文件系统是否已挂载

/**
 * @brief 在 SPIFFS 上创建“目录”占位文件
 * @param path 目录名（如 /spiffs/log）
 * @note  SPIFFS 无目录概念：文件直接位于挂载点下，目录名仅作为路径前缀
 *        出现在文件名中（如 /spiffs/log/events.jsonl）。这里创建一个空标记文件，
 *        便于诊断页判断存储是否已初始化。
 */
static void ensure_dir_marker(const char *path)
{
    FILE *f = fopen(path, "a");
    if (f == NULL)
    {
        ESP_LOGW(TAG, "创建存储标记失败：%s", path);
        return;
    }
    fclose(f);
}

esp_err_t storage_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = APP_CFG_SPIFFS_BASE,
        .partition_label = "spiffs",
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SPIFFS 挂载失败：%s", esp_err_to_name(err));
        return err;
    }

    // SPIFFS 是扁平文件系统，不支持目录：这里直接以空文件创建“占位目录”，
    // 既避免每次 mkdir 报错，也便于 `storage_usage()` 观察分区已初始化。
    ensure_dir_marker(APP_CFG_LOG_DIR);
    ensure_dir_marker(APP_CFG_SNAP_DIR);

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("spiffs", &total, &used) == ESP_OK)
    {
        ESP_LOGI(TAG, "SPIFFS 已挂载：%u / %u 字节已用（%.0f%%）", (unsigned)used,
                 (unsigned)total, total ? used * 100.0f / total : 0.0f);
    }

    s_ready = true;
    xEventGroupSetBits(g_sys_events, SYS_EVENT_STORAGE_READY);
    return ESP_OK;
}

bool storage_is_ready(void)
{
    return s_ready;
}

esp_err_t storage_usage(size_t *total, size_t *used)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_spiffs_info("spiffs", total, used);
}
