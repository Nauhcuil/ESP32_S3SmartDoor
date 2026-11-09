/**
 * @file    access_log.c
 * @brief   门禁事件日志实现：JSONL 追加写、按 256 KB 轮转、按 seq 分页读取。
 *
 * @note    - 写入路径始终在 `g_log_mutex` 内：face_task 在帧送出后才调用，UI/网络任务
 *            也可能读取，串行化避免 SPIFFS 并发写损坏文件。
 *          - `s_seq` 与 `s_count` 由日志锁保护；`s_seq` 在启动时从最后一个非空行的
 *            `"seq":N` 恢复，保证跨重启单调（配合 boot 字段供云端去重）。
 *          - 行格式固定为单行 JSON（不含换行），名字中的 `"`/`\` 做转义，控制字符替换为空格。
 *          - 读取时从“当前文件 + 轮转文件”按文件名倒序拼接，再反转取最近 N 条。
 *            每行约 120~180 字节，2 MB 分区可容纳约 1 万条，读取上限按行解析、不整文件载入。
 */

#include "access_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_events.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "storage.h"

static const char *TAG = "access_log";

/** @brief 日志文件名（索引 0 为当前文件，1~3 为轮转文件） */
static const char *const s_log_files[4] = {
    APP_CFG_LOG_DIR "/events.jsonl",
    APP_CFG_LOG_DIR "/events.1.jsonl",
    APP_CFG_LOG_DIR "/events.2.jsonl",
    APP_CFG_LOG_DIR "/events.3.jsonl",
};

/** @brief 单次读取允许扫描的最大行数（防止异常文件拖死 UI） */
#define ACCESS_LOG_SCAN_LINES 2048

static SemaphoreHandle_t s_lock = NULL;   // 保护文件与计数
static uint32_t s_seq = 0;                // 最近写入的序号
static uint32_t s_boot_id = 0;            // 本次上电标识（低 8 位写入 JSON）
static uint32_t s_count = 0;              // 当前日志文件条数
static uint32_t s_total = 0;              // 全部日志文件条数
static access_log_observer_t s_observer = NULL; // 新日志观察者（云端上报）

/** @brief 今日开门计数缓存（时间窗 + 结果） */
static uint32_t s_today_open = 0;
static int64_t s_today_cache_s = -1;

/** @brief 系统时间是否可信（SNTP 之前返回 false） */
static bool time_is_valid(void)
{
    return (time(NULL) > 1700000000);
}

/** @brief 取当前时间戳：可信时写 epoch 毫秒，否则写开机毫秒 */
static int64_t now_ts_ms(void)
{
    if (time_is_valid())
    {
        return (int64_t)time(NULL) * 1000;
    }
    return esp_timer_get_time() / 1000;
}

/** @brief 本地日期序号（年*10000+月*100+日），用于今日计数 */
static int local_day_key(void)
{
    if (!time_is_valid())
    {
        return -1;
    }
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return (tm_now.tm_year + 1900) * 10000 + (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday;
}

/** @brief 将字符串写入 JSON（转义引号/反斜杠，过滤控制字符） */
static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] != '\0' && o + 2 < cap; i++)
    {
        const unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\')
        {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        }
        else if (c < 0x20)
        {
            dst[o++] = ' ';
        }
        else
        {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/** @brief 文件大小（不存在返回 -1） */
static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose(f);
    return size;
}

/** @brief 统计文件行数（不存在返回 0） */
static uint32_t count_lines(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        return 0;
    }
    uint32_t n = 0;
    int c = 0;
    bool has_data = false;
    while ((c = fgetc(f)) != EOF)
    {
        has_data = true;
        if (c == '\n')
        {
            n++;
        }
    }
    if (has_data)
    {
        n++; // 最后一行可能没有换行符
    }
    fclose(f);
    return n;
}

/**
 * @brief 从文件尾部反向读取最后一行并解析 seq
 * @note  只读最后 512 字节，避免日志很大时启动耗时。
 */
static uint32_t recover_seq_from(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return 0;
    }
    long size = ftell(f);
    long start = size > 512 ? size - 512 : 0;
    char buf[513];
    size_t got = 0;
    if (fseek(f, start, SEEK_SET) == 0)
    {
        got = fread(buf, 1, sizeof(buf) - 1, f);
    }
    fclose(f);
    buf[got] = '\0';

    char *last_nl = strrchr(buf, '\n');
    char *line = last_nl ? last_nl + 1 : buf;
    if (*line == '\0' && last_nl != NULL)
    {
        // 文件以换行结尾：再找上一行
        *last_nl = '\0';
        last_nl = strrchr(buf, '\n');
        line = last_nl ? last_nl + 1 : buf;
    }
    char *p = strstr(line, "\"seq\":");
    if (p == NULL)
    {
        return 0;
    }
    return (uint32_t)strtoul(p + 6, NULL, 10);
}

/** @brief 轮转：当前→.1→.2→.3，超出保留数的丢弃 */
static void rotate_logs(void)
{
    char old_path[64];
    char new_path[64];
    for (int i = APP_CFG_LOG_KEEP_FILES - 1; i > 0; i--)
    {
        snprintf(old_path, sizeof(old_path), "%s", s_log_files[i - 1]);
        snprintf(new_path, sizeof(new_path), "%s", s_log_files[i]);
        remove(new_path);
        if (file_size(old_path) >= 0)
        {
            rename(old_path, new_path);
        }
    }
    ESP_LOGI(TAG, "日志已轮转");
}

/** @brief 按 JSON 行的 key 提取字符串/数值（简易解析，只服务本模块自己写的格式） */
static bool json_get_string(const char *line, const char *key, char *out, size_t cap)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(line, pattern);
    if (p == NULL)
    {
        return false;
    }
    p += strlen(pattern);
    size_t o = 0;
    while (*p != '\0' && *p != '"' && o + 1 < cap)
    {
        out[o++] = *p++;
    }
    out[o] = '\0';
    return true;
}

/** @brief 读取一行并填充记录；失败返回 false */
static bool parse_line(const char *line, access_log_record_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    const char *seq = strstr(line, "\"seq\":");
    if (seq == NULL)
    {
        return false;
    }
    rec->seq = (uint32_t)strtoul(seq + 6, NULL, 10);
    const char *ts = strstr(line, "\"ts\":");
    if (ts != NULL)
    {
        rec->ts_ms = strtoll(ts + 5, NULL, 10);
    }
    (void)json_get_string(line, "type", rec->type, sizeof(rec->type));
    (void)json_get_string(line, "result", rec->result, sizeof(rec->result));
    (void)json_get_string(line, "reason", rec->reason, sizeof(rec->reason));
    (void)json_get_string(line, "name", rec->name, sizeof(rec->name));
    const char *sim = strstr(line, "\"sim\":");
    if (sim != NULL)
    {
        rec->similarity = strtof(sim + 6, NULL);
    }
    const char *live = strstr(line, "\"live\":");
    if (live != NULL)
    {
        rec->liveness = strtof(live + 7, NULL);
    }
    return true;
}

/**
 * @brief 从日志文件按时间倒序解析，收集最新的记录
 * @param before_seq 只收集 seq < before_seq；0 表示不限
 * @param collect_limit 最多收集多少条
 * @param out 输出（按时间倒序，新的在前）
 */
static size_t collect_recent(size_t before_seq, size_t collect_limit, access_log_record_t *out,
                             size_t cap)
{
    if (collect_limit > cap)
    {
        collect_limit = cap;
    }
    if (collect_limit > 32)
    {
        collect_limit = 32; // UI 每页最多 32 条，限制栈占用
    }
    if (collect_limit == 0)
    {
        return 0;
    }

    static access_log_record_t tmp[32]; // 调用方持日志锁，静态缓冲可复用
    size_t got = 0;
    char line[ACCESS_LOG_LINE_MAX];

    // 每个文件完整扫描（≤2048 行），用“保留 seq 最大的 N 条”收集最新记录。
    // 不能只取每个文件的前 N 行——那样会永远读到最旧的记录。
    for (int f = 0; f < APP_CFG_LOG_KEEP_FILES; f++)
    {
        FILE *fp = fopen(s_log_files[f], "rb");
        if (fp == NULL)
        {
            continue;
        }
        size_t scanned = 0;
        while (scanned < ACCESS_LOG_SCAN_LINES && fgets(line, sizeof(line), fp) != NULL)
        {
            scanned++;
            access_log_record_t rec;
            if (!parse_line(line, &rec))
            {
                continue;
            }
            if (before_seq != 0 && rec.seq >= before_seq)
            {
                continue;
            }
            if (got < collect_limit)
            {
                tmp[got++] = rec;
                continue;
            }
            // 已满：替换当前最小 seq（只在新记录更新时）
            size_t min_i = 0;
            for (size_t i = 1; i < got; i++)
            {
                if (tmp[i].seq < tmp[min_i].seq)
                {
                    min_i = i;
                }
            }
            if (rec.seq > tmp[min_i].seq)
            {
                tmp[min_i] = rec;
            }
        }
        fclose(fp);
    }

    // 按 seq 倒序（新的在前）
    for (size_t i = 0; i < got; i++)
    {
        for (size_t j = i + 1; j < got; j++)
        {
            if (tmp[j].seq > tmp[i].seq)
            {
                access_log_record_t t = tmp[i];
                tmp[i] = tmp[j];
                tmp[j] = t;
            }
        }
    }
    memcpy(out, tmp, got * sizeof(tmp[0]));
    return got;
}

/** @brief 是否已初始化，并保证锁存在 */
static bool ready_locked(void)
{
    if (!storage_is_ready())
    {
        return false;
    }
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL)
        {
            return false;
        }
    }
    return true;
}

esp_err_t access_log_init(void)
{
    if (!ready_locked())
    {
        ESP_LOGW(TAG, "存储未就绪，日志功能停用");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    s_boot_id = esp_random() & 0xFF;
    s_seq = 0;
    s_count = 0;
    s_total = 0;
    for (int i = 0; i < APP_CFG_LOG_KEEP_FILES; i++)
    {
        const uint32_t n = count_lines(s_log_files[i]);
        s_total += n;
        if (i == 0)
        {
            s_count = n;
        }
        const uint32_t seq = recover_seq_from(s_log_files[i]);
        if (seq > s_seq)
        {
            s_seq = seq;
        }
    }

    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "日志已就绪：seq=%u，当前文件 %u 条，共 %u 条", s_seq, s_count, s_total);
    return ESP_OK;
}

esp_err_t access_log_append(const access_log_entry_t *entry)
{
    if (entry == NULL || entry->type == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ready_locked() || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;

    // 当前文件超过上限：先轮转
    if (file_size(s_log_files[0]) >= APP_CFG_LOG_MAX_BYTES)
    {
        rotate_logs();
        s_count = 0;
    }

    FILE *f = fopen(s_log_files[0], "a");
    if (f == NULL)
    {
        err = ESP_FAIL;
        xSemaphoreGive(s_lock);
        return err;
    }

    char name[64];
    char extra[96];
    json_escape(entry->name ? entry->name : "", name, sizeof(name));
    json_escape(entry->extra ? entry->extra : "", extra, sizeof(extra));

    char iso[32] = "";
    if (time_is_valid())
    {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M:%S", &tm_now);
    }

    const uint32_t seq = s_seq + 1;
    const int written =
        fprintf(f,
                "{\"seq\":%u,\"boot\":%u,\"ts\":%lld,\"iso\":\"%s\",\"type\":\"%s\","
                "\"result\":\"%s\",\"reason\":\"%s\",\"name\":\"%s\",\"sim\":%.2f,\"live\":%.0f,"
                "\"extra\":\"%s\"}\n",
                (unsigned)seq, (unsigned)s_boot_id, (long long)now_ts_ms(), iso, entry->type,
                entry->result,
                entry->reason ? entry->reason : "", name,
                entry->similarity >= 0.0f ? entry->similarity : -1.0f,
                entry->liveness >= 0.0f ? entry->liveness : -1.0f, extra);

    if (written < 0 || fflush(f) != 0)
    {
        err = ESP_FAIL;
    }
    else
    {
        s_seq = seq;
        s_count++;
        s_total++;
        s_today_cache_s = -1; // 今日计数缓存失效
    }
    fclose(f);
    xSemaphoreGive(s_lock);

    if (err == ESP_OK)
    {
        ESP_LOGD(TAG, "已记录 #%u %s/%s", seq, entry->type, entry->result);
        // 锁已释放再通知观察者：回调里如果要读日志（取原文行）才不会被自己卡住
        const access_log_observer_t cb = s_observer;
        if (cb != NULL)
        {
            cb(seq, entry->type);
        }
    }
    else
    {
        ESP_LOGW(TAG, "日志写入失败");
    }
    return err;
}

void access_log_set_observer(access_log_observer_t cb)
{
    s_observer = cb;
}

uint32_t access_log_get_seq(void)
{
    return s_seq;
}

uint32_t access_log_get_count(void)
{
    return s_total;
}

size_t access_log_read_recent(access_log_record_t *out, size_t cap, size_t limit)
{
    if (out == NULL || !ready_locked() || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        return 0;
    }
    const size_t n = collect_recent(0, limit, out, cap);
    xSemaphoreGive(s_lock);
    return n;
}

size_t access_log_read_page(access_log_record_t *out, size_t cap, size_t limit,
                            uint32_t before_seq)
{
    if (out == NULL || !ready_locked() || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        return 0;
    }
    const size_t n = collect_recent(before_seq, limit, out, cap);
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t access_log_today_open_count(void)
{
    // 缓存 1 秒：append 成功会失效缓存，避免每帧调 SCAN。
    const int64_t up_s = esp_timer_get_time() / 1000000;
    if (s_today_cache_s > 0 && up_s - s_today_cache_s < 1)
    {
        return s_today_open;
    }
    s_today_cache_s = up_s;

    const int day = local_day_key();
    const uint32_t boot_id = s_boot_id;
    uint32_t count = 0;
    if (ready_locked() && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        char line[ACCESS_LOG_LINE_MAX];
        for (int f = 0; f < APP_CFG_LOG_KEEP_FILES; f++)
        {
            FILE *fp = fopen(s_log_files[f], "rb");
            if (fp == NULL)
            {
                continue;
            }
            size_t scanned = 0;
            while (scanned < ACCESS_LOG_SCAN_LINES && fgets(line, sizeof(line), fp) != NULL)
            {
                scanned++;
                if (strstr(line, "\"result\":\"open\"") == NULL)
                {
                    continue;
                }
                if (day < 0)
                {
                    // 时间未同步：只统计本次上电（按 boot 字段）的开门
                    char boot_pat[24];
                    snprintf(boot_pat, sizeof(boot_pat), "\"boot\":%u", (unsigned)boot_id);
                    if (strstr(line, boot_pat) != NULL)
                    {
                        count++;
                    }
                    continue;
                }
                const char *iso = strstr(line, "\"iso\":\"");
                if (iso == NULL)
                {
                    continue;
                }
                iso += 7;
                // iso 形如 2026-11-03 15:30:12；只比较日期部分
                const int y = atoi(iso);
                const int m = isdigit((unsigned char)iso[5]) ? (iso[5] - '0') * 10 + (iso[6] - '0') : 0;
                const int d = isdigit((unsigned char)iso[8]) ? (iso[8] - '0') * 10 + (iso[9] - '0') : 0;
                if (y * 10000 + m * 100 + d == day)
                {
                    count++;
                }
            }
            fclose(fp);
        }
        xSemaphoreGive(s_lock);
    }
    s_today_open = count;
    return count;
}

bool access_log_get_line(uint32_t seq, char *buf, size_t len)
{
    if (buf == NULL || len == 0 || !ready_locked() ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        return false;
    }
    bool found = false;
    char line[ACCESS_LOG_LINE_MAX];
    for (int f = 0; f < APP_CFG_LOG_KEEP_FILES && !found; f++)
    {
        FILE *fp = fopen(s_log_files[f], "rb");
        if (fp == NULL)
        {
            continue;
        }
        size_t scanned = 0;
        while (scanned < ACCESS_LOG_SCAN_LINES && fgets(line, sizeof(line), fp) != NULL)
        {
            scanned++;
            const char *p = strstr(line, "\"seq\":");
            if (p != NULL && (uint32_t)strtoul(p + 6, NULL, 10) == seq)
            {
                snprintf(buf, len, "%s", line);
                found = true;
                break;
            }
        }
        fclose(fp);
    }
    xSemaphoreGive(s_lock);
    return found;
}

esp_err_t access_log_clear(void)
{
    if (!ready_locked() || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    for (int i = 0; i < APP_CFG_LOG_KEEP_FILES; i++)
    {
        remove(s_log_files[i]);
    }
    s_count = 0;
    s_total = 0;
    s_today_open = 0;
    s_today_cache_s = -1;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "日志已清空（seq 保持 %u）", s_seq);
    return ESP_OK;
}
