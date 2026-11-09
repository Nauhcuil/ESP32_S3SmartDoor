/**
 * @file    bemfa_proto.c
 * @brief   巴法云消息协议实现：纯字符串扫描，不依赖 cJSON（省内部 RAM 与代码体积）。
 *
 * @note    - 下行载荷来自公网，长度不可信：所有扫描都以 (data, len) 为界，绝不依赖 '\0'。
 *          - 只实现“取字符串字段 / 取数值字段”两个最小解析器，足以覆盖本项目的命令集。
 *            没有完整 JSON 校验——非法内容最终会解析出 NONE 并被丢弃。
 *          - 上行 JSON 手写 snprintf：字段固定、值都来自内部，不需要转义。
 */

#include "bemfa_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "bemfa_proto"; // 日志标签

/** @brief 忽略大小写的单字符比较 */
static bool ci_equal(char a, char b)
{
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

/** @brief 忽略大小写比较（前者是待检查文本，后者是期望值） */
static bool ci_starts_with(const char *text, size_t len, const char *expect)
{
    const size_t n = strlen(expect);
    if (len < n)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (!ci_equal(text[i], expect[i]))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 在 JSON 文本中定位某个字段的值起点
 * @return 指向值（去掉了引号与冒号）的指针；未找到返回 NULL
 * @note  只接受 "key" 形式（键必须带引号），符合巴法云实际下发格式。
 */
static const char *json_find_value(const char *data, size_t len, const char *key)
{
    const size_t klen = strlen(key);

    for (size_t i = 0; i + klen + 2 <= len; i++)
    {
        if (data[i] != '"')
        {
            continue;
        }
        // 键名必须恰好匹配（后面紧跟引号）
        bool match = true;
        for (size_t k = 0; k < klen; k++)
        {
            if (i + 1 + k >= len || !ci_equal(data[i + 1 + k], key[k]))
            {
                match = false;
                break;
            }
        }
        if (!match || i + 1 + klen >= len || data[i + 1 + klen] != '"')
        {
            continue;
        }

        // 跳过 "key" 后面的空白与冒号
        size_t p = i + 2 + klen;
        while (p < len && (data[p] == ' ' || data[p] == '\t' || data[p] == '\r' || data[p] == '\n'))
        {
            p++;
        }
        if (p >= len || data[p] != ':')
        {
            continue;
        }
        p++;
        while (p < len &&
               (data[p] == ' ' || data[p] == '\t' || data[p] == '\r' || data[p] == '\n'))
        {
            p++;
        }
        return (p < len) ? (data + p) : NULL;
    }
    return NULL;
}

/**
 * @brief 取字符串字段
 * @param p 值起点（json_find_value 的结果）
 * @return 复制长度；未找到/为空返回 0
 */
static size_t json_get_string(const char *p, size_t avail, char *out, size_t out_size)
{
    if (p == NULL || avail == 0 || out_size == 0)
    {
        return 0;
    }

    size_t o = 0;
    if (p[0] == '"')
    {
        for (size_t i = 1; i < avail && p[i] != '"' && o + 1 < out_size; i++)
        {
            if (p[i] == '\\' && i + 1 < avail)
            {
                i++; // 简单的转义：直接取下一个字符
            }
            out[o++] = p[i];
        }
    }
    else
    {
        // 无引号：取到分隔符为止
        for (size_t i = 0; i < avail && o + 1 < out_size; i++)
        {
            const char c = p[i];
            if (c == ',' || c == '}' || c == ' ' || c == '\r' || c == '\n' || c == '\t')
            {
                break;
            }
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o;
}

/**
 * @brief 取数值字段
 * @return 解析出的值；未找到返回 -1
 */
static float json_get_number(const char *p, size_t avail)
{
    if (p == NULL || avail == 0)
    {
        return -1.0f;
    }
    // 复制到小缓冲再转（strtof 需要结束符；载荷不保证以 '\0' 结尾）
    char tmp[16];
    size_t n = 0;
    for (size_t i = 0; i < avail && n + 1 < sizeof(tmp); i++)
    {
        const char c = p[i];
        if (c == ',' || c == '}' || c == ' ' || c == '\r' || c == '\n' || c == '\t')
        {
            break;
        }
        tmp[n++] = c;
    }
    tmp[n] = '\0';
    if (n == 0)
    {
        return -1.0f;
    }
    char *end = NULL;
    const float v = strtof(tmp, &end);
    if (end == tmp)
    {
        return -1.0f; // 不是数字
    }
    return v;
}

const char *bemfa_cmd_name(bemfa_cmd_t cmd)
{
    switch (cmd)
    {
    case BEMFA_CMD_OPEN:
        return "open";
    case BEMFA_CMD_QUERY_LOG:
        return "query_log";
    case BEMFA_CMD_SET_THRESHOLD:
        return "set_threshold";
    case BEMFA_CMD_REBOOT:
        return "reboot";
    case BEMFA_CMD_SET_LIVENESS:
        return "set_liveness";
    case BEMFA_CMD_NONE:
    default:
        return "none";
    }
}

/** @brief 命令名/输入 → 命令类型（忽略大小写） */
static bemfa_cmd_t cmd_from_text(const char *text, size_t len)
{
    if (ci_starts_with(text, len, "open") && len == 4)
    {
        return BEMFA_CMD_OPEN;
    }
    if (ci_starts_with(text, len, "query_log") && len == 9)
    {
        return BEMFA_CMD_QUERY_LOG;
    }
    if (ci_starts_with(text, len, "set_threshold") && len == 13)
    {
        return BEMFA_CMD_SET_THRESHOLD;
    }
    if (ci_starts_with(text, len, "reboot") && len == 6)
    {
        return BEMFA_CMD_REBOOT;
    }
    if (ci_starts_with(text, len, "set_liveness") && len == 12)
    {
        return BEMFA_CMD_SET_LIVENESS;
    }
    return BEMFA_CMD_NONE;
}

bool bemfa_parse(const char *data, size_t len, bemfa_request_t *out)
{
    if (data == NULL || len == 0 || out == NULL)
    {
        return false;
    }
    out->cmd = BEMFA_CMD_NONE;
    out->token[0] = '\0';
    out->value = -1.0f;

    // ---- JSON 形式 ----
    const char *v = json_find_value(data, len, "cmd");
    if (v != NULL)
    {
        char name[20] = {0};
        (void)json_get_string(v, len - (size_t)(v - data), name, sizeof(name));
        if (name[0] != '\0')
        {
            out->cmd = cmd_from_text(name, strlen(name));
            if (out->cmd != BEMFA_CMD_NONE)
            {
                const char *t = json_find_value(data, len, "token");
                (void)json_get_string(t, t != NULL ? len - (size_t)(t - data) : 0, out->token,
                                      sizeof(out->token));
                const char *n = json_find_value(data, len, "value");
                if (n != NULL)
                {
                    out->value = json_get_number(n, len - (size_t)(n - data));
                }
                return true;
            }
        }
    }

    // ---- 纯文本形式：去掉首尾空白/引号/换行后整体比对 ----
    size_t start = 0;
    size_t end = len;
    while (start < end && (isspace((unsigned char)data[start]) || data[start] == '"'))
    {
        start++;
    }
    while (end > start && (isspace((unsigned char)data[end - 1]) || data[end - 1] == '"'))
    {
        end--;
    }
    if (end - start >= 32)
    {
        return false; // 纯文本命令不会有这么长
    }
    char text[32];
    const size_t n = end - start; // 已保证 < 32（上面长度检查）
    memcpy(text, data + start, n);
    text[n] = '\0';

    // 纯文本支持 "set_threshold:0.60" 这种紧凑写法（巴法云 App 不便发 JSON 时使用）
    char *colon = strchr(text, ':');
    float value = -1.0f;
    if (colon != NULL)
    {
        *colon = '\0';
        value = strtof(colon + 1, NULL);
    }

    const bemfa_cmd_t cmd = cmd_from_text(text, strlen(text));
    if (cmd == BEMFA_CMD_NONE)
    {
        ESP_LOGD(TAG, "无法识别的下行载荷（长度 %u）", (unsigned)len);
        return false;
    }
    out->cmd = cmd;
    out->value = value;
    return true;
}

size_t bemfa_build_status(char *buf, size_t size, uint32_t seq, int64_t ts_ms, const char *fw,
                          int rssi, const char *ip, int faces, uint32_t uptime_s)
{
    if (buf == NULL || size == 0)
    {
        return 0;
    }
    const int ret = snprintf(buf, size,
                             "{\"type\":\"status\",\"seq\":%u,\"ts\":%lld,\"fw\":\"%s\","
                             "\"rssi\":%d,\"ip\":\"%s\",\"faces\":%d,\"uptime\":%u}",
                             (unsigned)seq, (long long)ts_ms, fw != NULL ? fw : "",
                             rssi, ip != NULL ? ip : "0.0.0.0", faces, (unsigned)uptime_s);
    if (ret < 0 || (size_t)ret >= size)
    {
        return 0;
    }
    return (size_t)ret;
}

size_t bemfa_build_ack(char *buf, size_t size, bemfa_cmd_t cmd, bool ok)
{
    if (buf == NULL || size == 0)
    {
        return 0;
    }
    const int ret = snprintf(buf, size, "{\"type\":\"ack\",\"cmd\":\"%s\",\"ok\":%d}",
                             bemfa_cmd_name(cmd), ok ? 1 : 0);
    if (ret < 0 || (size_t)ret >= size)
    {
        return 0;
    }
    return (size_t)ret;
}
