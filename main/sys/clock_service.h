/**
 * @file    clock_service.h
 * @brief   系统时间来源与主页时钟显示用的格式化接口。
 *
 * @note    - M4 之前没有 WiFi/SNTP：ESP32 的 RTC 掉电归零，time(NULL) 会返回 1970，
 *            界面显示 1970 年没有意义。因此 clock_service_init() 在“时间尚未有效”时
 *            用固件编译时刻（__DATE__/__TIME__）作为起点，让时钟从烧录那一刻开始走。
 *            它只是临时起点，不是真实时间；M4 接入 SNTP 后 time() 跳到真实时间，
 *            时钟显示自动跟随，不需要改 UI。
 *          - 时区固定 CST-8（中国），在 init 里 setenv/tzset，之后 localtime_r 即本地时间。
 *          - 提供 clock_service_format_time() 给主页标题栏用（"YYYY/MM/DD HH:MM"）；
 *            SNTP 对时成功后（M4）自动变成 "YYYY/MM/DD HH:MM:SS"。
 */

#ifndef __CLOCK_SERVICE_H__
#define __CLOCK_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 判定“时间有效”的起点（2023-11-14 之后），早于此视为 RTC 未设置 */
#define CLOCK_VALID_EPOCH 1700000000

/**
 * @brief 初始化系统时间
 * @note  幂等；若 RTC 已有有效时间（例如 M4 的 SNTP 已同步）则什么都不做。
 *        必须在第一次显示时钟前调用（main.c 的启动流程里）。
 */
void clock_service_init(void);

/**
 * @brief 时间是否有效（已同步或已用编译时间兜底）
 * @return true 表示 time() 可用
 */
bool clock_service_is_valid(void);

/**
 * @brief 本次上电是否已由 SNTP 对时（决定界面是否显示到秒）
 * @return true 表示系统时间是真实时间
 */
bool clock_service_is_synced(void);

/**
 * @brief SNTP 对时成功后由 time_sync 回调（可重复调用，每次都会校正系统时间）
 * @param tv 服务器返回的时间（UTC），NULL 或无效时间会被忽略
 * @note  由 lwIP tcpip 线程调用：只做 settimeofday + 置标志，不阻塞。
 */
void clock_service_mark_synced(const struct timeval *tv);

/**
 * @brief 按本地时区格式化当前时间
 * @param buf  输出缓冲区（只显示时分至少 12 字节；带日期至少 24 字节）
 * @param size 缓冲区大小
 * @param with_date true 输出全日期，false 只输出时间
 * @return buf
 * @note  已对时：带日期 "YYYY/MM/DD HH:MM:SS"，不带日期 "HH:MM:SS"；
 *        未对时（只有编译时刻兜底）：去掉秒，显示 "YYYY/MM/DD HH:MM" / "HH:MM"，
 *        避免让人误以为秒是准确的；时间无效时输出 "--:--:--"。
 */
const char *clock_service_format_time(char *buf, size_t size, bool with_date);

#ifdef __cplusplus
}
#endif

#endif /* __CLOCK_SERVICE_H__ */
