/**
 * @file    bemfa_proto.h
 * @brief   巴法云（bemfa）消息协议：上行状态/回执打包与下行命令解析。
 *
 * @note    - 巴法云是“主题即通道”的简单 MQTT 服务：设备用私钥（UID）作 client id，
 *            订阅并发布同一个主题；上下行都靠 JSON 里的 `type` / `cmd` 字段区分。
 *          - 门禁事件的上行载荷**直接复用门禁日志的 JSON 行**（见 access_log.c），
 *            避免同一份数据有两套序列化逻辑；本模块只负责“状态心跳”和“命令回执”。
 *          - 下行命令兼容两种格式：JSON（可带 token 与 value）与纯文本单词
 *            （巴法云 App 的“推送消息”场景常见），解析失败一律按 NONE 处理，
 *            由调用方决定是否回执错误。
 *          - 本模块不持有任何状态、不分配内存，可在任意任务中调用。
 */

#ifndef __BEMFA_PROTO_H__
#define __BEMFA_PROTO_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 下行命令类型 */
typedef enum
{
    BEMFA_CMD_NONE = 0,    /**< 无法识别 */
    BEMFA_CMD_OPEN,        /**< 远程开门 */
    BEMFA_CMD_QUERY_LOG,   /**< 查询最近门禁记录 */
    BEMFA_CMD_SET_THRESHOLD, /**< 设置识别阈值（value 字段） */
    BEMFA_CMD_REBOOT,      /**< 重启设备 */
    BEMFA_CMD_SET_LIVENESS, /**< 开关活体校验（value 0/1） */
} bemfa_cmd_t;

/** @brief 远程令牌最大长度（含结束符；管理员 PIN 为 4 位数字） */
#define BEMFA_TOKEN_MAX 16

/** @brief 解析后的下行命令 */
typedef struct
{
    bemfa_cmd_t cmd;          /**< 命令类型 */
    char token[BEMFA_TOKEN_MAX]; /**< 令牌（纯文本命令为空串） */
    float value;              /**< 数值参数（set_threshold 用），未给出为 -1 */
} bemfa_request_t;

/**
 * @brief 命令名（日志/回执用），如 "open"
 */
const char *bemfa_cmd_name(bemfa_cmd_t cmd);

/**
 * @brief 主题槽位（巴法云每个主题按名字独立收发，设备用 4 个主题分担）
 * @note  巴法云是“主题即通道”，同一个主题同时承担上下行会让消息类型混在一起，
 *        也更容易被误操作。这里固定 4 个主题：
 *          - ACCESS   门禁事件（开门/拒绝，QoS1 上行）
 *          - STRANGER 陌生人告警（QoS1 上行，手机端可单独做通知）
 *          - STATUS   心跳与命令回执（QoS0 上行）
 *          - CMD      远程命令（下行，qos1 订阅）
 *        4 个都填则完全隔离；只填一部分时按“降级映射”使用：
 *          1 个 → 全部走它；2 个 → 第 2 个专做命令下行；3 个 → 第 3 个只做状态/回执。
 *        映射规则见 cloud_service.c 的 resolve_topics()。
 */
typedef enum
{
    BEMFA_TOPIC_ACCESS = 0,   /**< 门禁事件 */
    BEMFA_TOPIC_STRANGER = 1, /**< 陌生人告警 */
    BEMFA_TOPIC_STATUS = 2,   /**< 心跳/回执 */
    BEMFA_TOPIC_CMD = 3,      /**< 远程命令（下行） */
    BEMFA_TOPIC_NUM = 4,
} bemfa_topic_slot_t;

/**
 * @brief 打包状态心跳 JSON
 * @param buf       输出缓冲
 * @param size      缓冲大小
 * @param seq       最近写入的日志序号（供云端对齐）
 * @param ts_ms     当前时间戳（毫秒）
 * @param fw        固件版本字符串
 * @param rssi      WiFi 信号强度（dBm）
 * @param ip        本机 IP（无则 "0.0.0.0"）
 * @param faces     已录入人脸数（负数表示未知）
 * @param uptime_s  本次上电秒数
 * @return 写入长度（不含结束符）；缓冲不足时返回 0
 */
size_t bemfa_build_status(char *buf, size_t size, uint32_t seq, int64_t ts_ms, const char *fw,
                          int rssi, const char *ip, int faces, uint32_t uptime_s);

/**
 * @brief 打包命令回执 JSON
 * @param buf  输出缓冲
 * @param size 缓冲大小
 * @param cmd  命令类型
 * @param ok   true = 执行成功
 * @return 写入长度；缓冲不足时返回 0
 */
size_t bemfa_build_ack(char *buf, size_t size, bemfa_cmd_t cmd, bool ok);

/**
 * @brief 解析下行载荷
 * @param data 载荷（不要求以 '\0' 结尾）
 * @param len  载荷长度
 * @param out  输出命令
 * @return true 解析出一个可识别的命令
 * @note  支持：
 *        - JSON：{"cmd":"open","token":"1234"} / {"cmd":"set_threshold","value":0.6}
 *        - 纯文本：open / query_log / reboot（无 token，调用方按“无令牌”处理）
 *        字段名与命令名都忽略大小写。
 *        命令集：open / query_log / set_threshold(value) / set_liveness(value) / reboot，
 *        除 token 外都可带 "value"（阈值 0~1，活体 0/1）。纯文本也支持
 *        "set_threshold:0.60" 这种紧凑写法。
 */
bool bemfa_parse(const char *data, size_t len, bemfa_request_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __BEMFA_PROTO_H__ */
