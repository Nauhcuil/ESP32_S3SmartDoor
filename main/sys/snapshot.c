/**
 * @file    snapshot.c
 * @brief   陌生人抓拍写盘实现：单任务 + 深度 2 的队列，写完后按数量清理。
 *
 * @note    - 任务优先级 2、Core0：低于识别/显示，空闲时写盘。
 *          - 每次写完后扫描目录：超过 APP_CFG_SNAP_MAX_FILES 时删除文件名最小的
 *            （文件名以时间/开机时长开头，近似最旧）。SPIFFS 目录项上限 32 字符，
 *            文件名控制在 28 字节内。
 *          - 写盘失败只告警；抓拍是旁路功能，不影响识别。
 */

#include "snapshot.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "storage.h"

static const char *TAG = "snapshot";

/** @brief 写盘任务栈
 *  @note  M4 内部 RAM 紧张，从 5 KB 收到 4 KB（实测剩余栈 4.2 KB，即实际只用不到 1 KB）。
 *         不能像大缓冲那样放 PSRAM：本任务要写 SPIFFS，写 Flash 期间 cache 关闭，
 *         若任务栈在 PSRAM 会触发访问异常。 */
#define SNAPSHOT_TASK_STACK (4 * 1024)
/** @brief 写盘任务优先级（低于人脸/显示） */
#define SNAPSHOT_TASK_PRIO 2

/** @brief 队列元素 */
typedef struct
{
    uint8_t *jpg;
    size_t len;
    char name[32];
} snapshot_job_t;

static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_count_lock = NULL;
static volatile size_t s_count = 0;
static bool s_started = false;

/** @brief 文件名是否属于抓拍（以 .jpg 结尾） */
static bool is_jpg(const char *name)
{
    const size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".jpg") == 0;
}

/** @brief 统计目录内抓拍张数 */
static size_t count_files(void)
{
    DIR *dir = opendir(APP_CFG_SNAP_DIR);
    if (dir == NULL)
    {
        return 0;
    }
    size_t n = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL)
    {
        // SPIFFS 扁平：d_name 是带前缀的完整路径，取 basename 判断
        const char *base = strrchr(ent->d_name, '/');
        base = (base != NULL) ? base + 1 : ent->d_name;
        if (is_jpg(base))
        {
            n++;
        }
    }
    closedir(dir);
    return n;
}

/** @brief 删除最旧的抓拍，使总数不超过上限 */
static void trim_files(void)
{
    // 多轮删除：每轮找文件名最小且未删除的一张
    for (int guard = 0; guard < 8; guard++)
    {
        size_t n = count_files();
        if (n <= APP_CFG_SNAP_MAX_FILES)
        {
            return;
        }
        DIR *dir = opendir(APP_CFG_SNAP_DIR);
        if (dir == NULL)
        {
            return;
        }
        char oldest[128] = ""; // SPIFFS d_name 是带前缀的完整路径
        struct dirent *ent = NULL;
        while ((ent = readdir(dir)) != NULL)
        {
            const char *base = strrchr(ent->d_name, '/');
            base = (base != NULL) ? base + 1 : ent->d_name;
            if (!is_jpg(base))
            {
                continue;
            }
            if (oldest[0] == '\0' || strcmp(ent->d_name, oldest) < 0)
            {
                snprintf(oldest, sizeof(oldest), "%.127s", ent->d_name);
            }
        }
        closedir(dir);
        if (oldest[0] == '\0')
        {
            return;
        }
        const char *path = oldest;
        if (remove(path) != 0)
        {
            ESP_LOGW(TAG, "删除旧抓拍失败：%s", oldest);
            return;
        }
        ESP_LOGD(TAG, "已清理旧抓拍：%s", oldest);
    }
}

/** @brief 写盘任务 */
static void snapshot_task(void *arg)
{
    (void)arg;
    snapshot_job_t job;

    while (true)
    {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        char path[96];
        snprintf(path, sizeof(path), APP_CFG_SNAP_DIR "/%.28s", job.name);

        bool ok = false;
        FILE *f = fopen(path, "wb");
        if (f != NULL)
        {
            ok = (fwrite(job.jpg, 1, job.len, f) == job.len);
            if (fclose(f) != 0)
            {
                ok = false;
            }
        }

        free(job.jpg); // frame2jpg 使用 malloc，必须 free

        if (ok)
        {
            xSemaphoreTake(s_count_lock, portMAX_DELAY);
            s_count = count_files();
            xSemaphoreGive(s_count_lock);
            ESP_LOGI(TAG, "抓拍已保存：%s（%u 字节，共 %u 张）", job.name, (unsigned)job.len,
                     (unsigned)s_count);
            trim_files();
        }
        else
        {
            ESP_LOGW(TAG, "抓拍写入失败：%s", job.name);
            remove(path);
        }
    }
}

esp_err_t snapshot_init(void)
{
    if (!storage_is_ready())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started)
    {
        return ESP_OK;
    }

    s_queue = xQueueCreate(2, sizeof(snapshot_job_t));
    s_count_lock = xSemaphoreCreateMutex();
    if (s_queue == NULL || s_count_lock == NULL)
    {
        ESP_LOGE(TAG, "抓拍队列创建失败");
        if (s_queue != NULL)
        {
            vQueueDelete(s_queue);
            s_queue = NULL;
        }
        if (s_count_lock != NULL)
        {
            vSemaphoreDelete(s_count_lock);
            s_count_lock = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(snapshot_task, "snapshot", SNAPSHOT_TASK_STACK, NULL,
                                SNAPSHOT_TASK_PRIO, NULL, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "抓拍任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_count_lock, portMAX_DELAY);
    s_count = count_files();
    xSemaphoreGive(s_count_lock);

    trim_files();
    s_started = true;
    ESP_LOGI(TAG, "抓拍模块已启动：已有 %u 张（上限 %d）", (unsigned)s_count,
             APP_CFG_SNAP_MAX_FILES);
    return ESP_OK;
}

esp_err_t snapshot_make_name(char *out, size_t cap, const char *tag)
{
    if (out == NULL || cap < 16)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const char *safe_tag = (tag != NULL && tag[0] != '\0') ? tag : "S";

    // 时间可信时用本地时间，否则用开机秒数（保证文件名可排序、近似唯一）
    // 注意：time(NULL) 在 SNTP 前返回 0（或 1970 附近），统一判 <1700000000
    if (time(NULL) > 1700000000)
    {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        snprintf(out, cap, "%04d%02d%02d_%02d%02d%02d_%c.jpg", tm_now.tm_year + 1900,
                 tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                 safe_tag[0]);
    }
    else
    {
        const uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
        snprintf(out, cap, "up%010u_%c.jpg", (unsigned)up_s, safe_tag[0]);
    }
    return ESP_OK;
}

esp_err_t snapshot_submit(uint8_t *jpg, size_t len, const char *name)
{
    if (jpg == NULL || len == 0 || name == NULL)
    {
        if (jpg != NULL)
        {
            free(jpg);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || s_queue == NULL)
    {
        free(jpg);
        return ESP_ERR_INVALID_STATE;
    }

    snapshot_job_t job = {0};
    job.jpg = jpg;
    job.len = len;
    snprintf(job.name, sizeof(job.name), "%.28s", name);

    if (xQueueSend(s_queue, &job, 0) != pdTRUE)
    {
        free(jpg);
        ESP_LOGW(TAG, "抓拍队列已满，丢弃本次抓拍");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool snapshot_is_ready(void)
{
    return s_started && storage_is_ready();
}

size_t snapshot_count(void)
{
    return s_count;
}

esp_err_t snapshot_clear(void)
{
    if (!storage_is_ready())
    {
        return ESP_ERR_INVALID_STATE;
    }
    DIR *dir = opendir(APP_CFG_SNAP_DIR);
    if (dir == NULL)
    {
        return ESP_OK;
    }
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL)
    {
        // SPIFFS 扁平：文件名已含 "/spiffs/snap/" 前缀，先截到目录名之后的 basename 再判断
        const char *base = strrchr(ent->d_name, '/');
        base = (base != NULL) ? base + 1 : ent->d_name;
        if (!is_jpg(base))
        {
            continue;
        }
        char path[96];
        snprintf(path, sizeof(path), "%.95s", ent->d_name);
        remove(path);
    }
    closedir(dir);
    if (s_count_lock != NULL)
    {
        xSemaphoreTake(s_count_lock, portMAX_DELAY);
        s_count = 0;
        xSemaphoreGive(s_count_lock);
    }
    return ESP_OK;
}
