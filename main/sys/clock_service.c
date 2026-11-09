/**
 * @file    clock_service.c
 * @brief   系统时间来源实现：无 SNTP 时用固件编译时刻兜底，并统一格式化给 UI。
 *
 * @note    为什么用编译时刻：M4 之前没有网络，ESP32 内部 RTC 掉电归零，time(NULL)
 *          返回 1970 附近。主页时钟若显示 1970 年毫无意义，不如从“烧录这一刻”开始走，
 *          用户看到的时分与真实时间只差一次编译到上电的间隔（通常几分钟）。
 *          M4 接入 SNTP 后 time() 会被校正为真实时间，时钟自动跳到正确的值。
 *
 *          编译时刻格式：__DATE__ = "Nov  5 2026"（月份用英文缩写，日可能是 "  5"
 *          两位空格对齐），__TIME__ = "11:23:45"。解析时月份查表，日/时分秒用 sscanf。
 */

#include "clock_service.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "clock_service";

/** @brief 时区字符串（中国标准时间，UTC+8） */
#define CLOCK_TZ "CST-8"

/**
 * @brief 是否已由 SNTP 对时
 * @note  只表示"本次上电拿到过真实时间"：编译时刻兜底不算。
 *        读写在 lwIP 线程（mark_synced）与 UI 线程（格式化）之间，bool 读写是原子的。
 */
static volatile bool s_synced = false;

/** @brief 英文月份缩写查表（__DATE__ 的月份字段） */
static const char *const s_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/**
 * @brief 把 __DATE__ 的月份缩写转成 1~12
 * @return 月份；无法识别时返回 1
 */
static int month_from_name(const char *name)
{
    for (int i = 0; i < 12; i++)
    {
        if (strncmp(name, s_months[i], 3) == 0)
        {
            return i + 1;
        }
    }
    return 1;
}

/**
 * @brief 把编译时刻写入系统时钟
 * @return true 写入成功
 * @note  用 mktime() 按本地时区解释结构体字段，因此要先设置 TZ。
 */
static bool set_time_from_build(void)
{
    char mon[4] = {0};
    int day = 1;
    int year = 2024;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3)
    {
        ESP_LOGW(TAG, "编译日期解析失败：%s", __DATE__);
        return false;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3)
    {
        ESP_LOGW(TAG, "编译时间解析失败：%s", __TIME__);
        return false;
    }

    struct tm tm_build;
    memset(&tm_build, 0, sizeof(tm_build));
    tm_build.tm_year = year - 1900;
    tm_build.tm_mon = month_from_name(mon) - 1;
    tm_build.tm_mday = day;
    tm_build.tm_hour = hour;
    tm_build.tm_min = minute;
    tm_build.tm_sec = second;
    tm_build.tm_isdst = 0;

    const time_t build_epoch = mktime(&tm_build); // 按本地时区解释
    if (build_epoch <= 0)
    {
        ESP_LOGW(TAG, "编译时刻换算失败");
        return false;
    }

    struct timeval tv;
    tv.tv_sec = build_epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0)
    {
        ESP_LOGW(TAG, "设置系统时间失败");
        return false;
    }
    return true;
}

void clock_service_init(void)
{
    // 时区：access_log / snapshot 的 localtime_r 也用这个，统一 CST-8
    setenv("TZ", CLOCK_TZ, 1);
    tzset();

    if (time(NULL) >= CLOCK_VALID_EPOCH)
    {
        ESP_LOGI(TAG, "系统时间已有效，无需兜底");
        return;
    }

    if (set_time_from_build())
    {
        char buf[32];
        ESP_LOGI(TAG, "系统时间未同步，暂用编译时刻起步：%s",
                 clock_service_format_time(buf, sizeof(buf), true));
    }
}

bool clock_service_is_valid(void)
{
    return time(NULL) >= CLOCK_VALID_EPOCH;
}

bool clock_service_is_synced(void)
{
    return s_synced;
}

void clock_service_mark_synced(const struct timeval *tv)
{
    if (tv == NULL || tv->tv_sec < CLOCK_VALID_EPOCH)
    {
        return;
    }

    // SNTP 默认实现（sntp_sync_time）已经 settimeofday 过一次；这里再写一次是为了
    // 让"标记已对时"与"时间生效"在同一个地方发生，不依赖 lwIP 内部实现细节。
    struct timeval now = *tv;
    if (settimeofday(&now, NULL) != 0)
    {
        ESP_LOGW(TAG, "对时写入系统时钟失败");
        return;
    }
    s_synced = true;
}

const char *clock_service_format_time(char *buf, size_t size, bool with_date)
{
    if (buf == NULL || size == 0)
    {
        return "";
    }

    const time_t now = time(NULL);
    if (now < CLOCK_VALID_EPOCH)
    {
        snprintf(buf, size, "--:--:--");
        return buf;
    }

    struct tm tm_now;
    localtime_r(&now, &tm_now);
    const bool synced = s_synced; // 只读一次，避免格式化过程中标志变化造成格式不一致
    if (with_date)
    {
        // 年/月/日 + 时:分[:秒]，全部用数字与 "/" 表示（用户要求的简洁格式）。
        // 秒只在 SNTP 对时后显示：编译时刻兜底本身就有分钟级误差，显示秒是误导。
        if (synced)
        {
            snprintf(buf, size, "%04d/%02d/%02d %02d:%02d:%02d", tm_now.tm_year + 1900,
                     tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min,
                     tm_now.tm_sec);
        }
        else
        {
            snprintf(buf, size, "%04d/%02d/%02d %02d:%02d", tm_now.tm_year + 1900,
                     tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min);
        }
    }
    else
    {
        if (synced)
        {
            snprintf(buf, size, "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        }
        else
        {
            snprintf(buf, size, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        }
    }
    return buf;
}
