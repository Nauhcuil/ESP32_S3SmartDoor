/**
 * @file    access_log.h
 * @brief   门禁事件日志（M3）：JSONL 追加写 `/spiffs/log/events.jsonl`，按大小轮转。
 *
 * @note    - 每行一条 JSON：`{"seq":N,"boot":0x..,"ts":毫秒,"iso":"...","type":"access|stranger|...",
 *            "result":"open|deny|...","reason":"face|remote|voice|...","name":"..","sim":0.87,"live":82}`。
 *          - `seq` 进程内单调递增，`boot` 为本次上电的随机标识；两者组合供云端（M4）去重。
 *          - 写文件会短暂关闭 Flash cache，因此**不得在持 LVGL 锁或尚未归还相机帧时调用**；
 *            日志模块内部用 g_log_mutex 串行化，调用方阻塞到写完为止（通常 <10 ms）。
 *          - 轮转策略：当前文件超过 APP_CFG_LOG_MAX_BYTES 后，删除最旧的 `.3`，依次
 *            `.2→.3`、`.1→.2`、当前→`.1`，再新建当前文件；最多保留 APP_CFG_LOG_KEEP_FILES 个。
 */

#ifndef __ACCESS_LOG_H__
#define __ACCESS_LOG_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 单行日志最大长度（含结束符）。读取接口用它确定缓冲下限，
 *         云端上报（cloud_service）也直接复用这个上限取原文行。 */
#define ACCESS_LOG_LINE_MAX 256

/** @brief 事件类型 / 结果 / 原因（以字符串常量写入 JSON，便于云端与 UI 解析） */
#define ACCESS_LOG_TYPE_ACCESS   "access"   /**< 一次人脸/远程/语音开门尝试 */
#define ACCESS_LOG_TYPE_STRANGER "stranger" /**< 确认的陌生人 */
#define ACCESS_LOG_TYPE_OCCUPY   "occupy"   /**< 视频占用/遮挡 */
#define ACCESS_LOG_TYPE_STATUS   "status"   /**< 系统状态（M4 上报） */

#define ACCESS_LOG_RESULT_OPEN      "open"         /**< 开门 */
#define ACCESS_LOG_RESULT_DENY      "deny"         /**< 未注册 / 识别失败 */
#define ACCESS_LOG_RESULT_DENY_LIVE "deny_liveness" /**< 活体失败 */
#define ACCESS_LOG_RESULT_INFO      "info"         /**< 仅记录 */

#define ACCESS_LOG_REASON_FACE          "face"
#define ACCESS_LOG_REASON_REMOTE        "remote"
#define ACCESS_LOG_REASON_VOICE         "voice"
#define ACCESS_LOG_REASON_UNKNOWN       "unknown"
#define ACCESS_LOG_REASON_LIVENESS_FAIL "liveness"

/** @brief 一条日志记录（由业务填充，access_log_append 负责序列化） */
typedef struct
{
    const char *type;   /**< ACCESS_LOG_TYPE_* */
    const char *result; /**< ACCESS_LOG_RESULT_* */
    const char *reason; /**< ACCESS_LOG_REASON_* */
    const char *name;   /**< 姓名或 NULL */
    float similarity;   /**< 相似度 [0,1]；<0 表示无 */
    float liveness;     /**< 活体分 [0,100]；<0 表示无 */
    const char *extra;  /**< 附加说明（可为 NULL），如抓拍路径 */
} access_log_entry_t;

/** @brief 读取用的记录（字符串已复制，供 UI 列表展示） */
typedef struct
{
    uint32_t seq;
    int64_t ts_ms;
    char type[16];
    char result[20];
    char reason[16];
    char name[32];
    float similarity;
    float liveness;
} access_log_record_t;

/** @brief 初始化：打开/轮转日志文件并用最后一条恢复 seq */
esp_err_t access_log_init(void);

/**
 * @brief 追加一条记录
 * @return ESP_OK 成功；序号（>0）表示已写入；ESP_ERR_INVALID_STATE 存储未就绪
 * @note  成功后 seq 递增；返回的序号可通过 access_log_get_seq() 读取
 */
esp_err_t access_log_append(const access_log_entry_t *entry);

/** @brief 最近一次写入的序号（未写入过为 0） */
uint32_t access_log_get_seq(void);

/** @brief 当前日志文件累计条数（从文件恢复，用于 UI “共 N 条”） */
uint32_t access_log_get_count(void);

/**
 * @brief 读取最新的 limit 条记录（新的在前）
 * @param out 输出数组
 * @param cap 数组容量
 * @param limit 想要的最大条数（≤ cap）
 * @return 实际条数
 */
size_t access_log_read_recent(access_log_record_t *out, size_t cap, size_t limit);

/**
 * @brief 读取序号小于 before_seq 的 limit 条记录（分页）
 * @param before_seq 只返回 seq < before_seq 的记录；0 表示不限制
 * @return 实际条数
 */
size_t access_log_read_page(access_log_record_t *out, size_t cap, size_t limit,
                            uint32_t before_seq);

/**
 * @brief 今日开门次数
 * @note  系统时间未同步时 ts_ms 是开机毫秒数，此时直接返回本次上电的开门次数；
 *        时间同步后按本地自然日过滤。
 */
uint32_t access_log_today_open_count(void);

/**
 * @brief 取出序号为 seq 的原样 JSON 行（M4 断网补发用）
 * @return true 找到并复制；false 未找到
 */
bool access_log_get_line(uint32_t seq, char *buf, size_t len);

/** @brief 清空全部日志文件 */
esp_err_t access_log_clear(void);

/**
 * @brief 新日志观察者（M4 云端上报用）
 * @param seq  刚写入的日志序号
 * @param type 事件类型（ACCESS_LOG_TYPE_*）
 * @note  在 access_log_append 成功、锁已释放之后调用，运行在写日志的任务上下文里，
 *        **回调必须立即返回**（只做入队之类的轻量动作）；未注册时为空操作。
 *        任何任务都可以注册，但只保留一个观察者。
 */
typedef void (*access_log_observer_t)(uint32_t seq, const char *type);

/** @brief 注册（传 NULL 取消）新日志观察者 */
void access_log_set_observer(access_log_observer_t cb);

#ifdef __cplusplus
}
#endif

#endif /* __ACCESS_LOG_H__ */
