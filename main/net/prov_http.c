/**
 * @file    prov_http.c
 * @brief   配网门户实现：内嵌 HTML 表单 + /save 处理（写入 NVS 后重启）。
 *
 * @note    1. 页面与表单不经过 LVGL，HTML 中可以自由使用中文，不需要改字库。
 *          2. 表单解析只做最小实现：application/x-www-form-urlencoded，
 *             逐字段找 "key="，按 '&' 截断，再做 %XX 与 '+' 解码。
 *          3. 请求体上限 512 字节：本表单最长字段 64 + 48 + 48 + 33，足够；
 *             超限直接返回 413，避免为表单分配大缓冲。
 *          4. HTTP 任务优先级 2：低于 face_task(5)，配网时界面预览不至于被抢占。
 *          5. 只处理 "/"、"/save" 与 "/favicon.ico"；其它路径返回 404。
 */

#include "prov_http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "net_config.h"
#include "wifi_manager.h"

static const char *TAG = "prov_http"; // 日志标签

/** @brief HTTP 请求体上限（表单字段总长不会超过这个值） */
#define PROV_BODY_MAX 512
/** @brief 内嵌页面缓冲大小 */
#define PROV_HTML_MAX 4096
/**
 * @brief 服务任务栈（HTML 拼接 + 表单解析）
 * @note  4 KB 是下限：httpd 默认 4 KB，本工程处理器只做内存内的字符串拼接与字段提取
 *        （不写文件、不调 NVS 以外的东西），实测够用；配网期内部 RAM 只剩 ~4~6 KB，
 *        6 KB 栈会以 ESP_ERR_HTTPD_TASK 启动失败。
 */
#define PROV_STACK 4096
/** @brief 服务任务优先级 */
#define PROV_PRIO 2
/** @brief 同时连接的客户端数（每路 socket 占内部 RAM，手机配网 1~2 路足够） */
#define PROV_MAX_SOCKETS 3
/**
 * @brief 门户自检开关（调试用）
 * @note  1 = 门户启动后从设备内部连 127.0.0.1:80 请求首页，串口打印应答首行。
 *        用来区分“HTTP 服务本身没起来”与“手机没把请求发到热点”。
 *        **发布版必须为 0**。
 */
#define PROV_SELFTEST 0
/** @brief 门户访问日志开关（打印来源 IP 与 User-Agent；发布版可置 0，省一条日志） */
#define PROV_LOG_ACCESS 1

/** @brief 服务句柄 */
static httpd_handle_t s_server = NULL;
/**
 * @brief 内嵌页面（启动时按 NVS 现有值渲染一次），放在 PSRAM
 * @note  页面 4 KB；若放 .bss 会永久占掉 4 KB **内部** RAM——配网只在开机未配置
 *        或用户主动进入时用一次，识别/录入的首次推理之后内部 RAM 只剩 ~6 KB，
 *        这 4 KB 是界面工作线程（3 KB 栈）能否创建的关键。
 */
static char *s_html = NULL;

/**
 * @brief HTML 转义（只处理表单回填需要的三个字符）
 */
static void html_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const char *p = in; *p != '\0' && o + 6 < out_size; p++)
    {
        switch (*p)
        {
        case '&':
            memcpy(out + o, "&amp;", 5);
            o += 5;
            break;
        case '<':
            memcpy(out + o, "&lt;", 4);
            o += 4;
            break;
        case '>':
            memcpy(out + o, "&gt;", 4);
            o += 4;
            break;
        case '"':
            memcpy(out + o, "&quot;", 6);
            o += 6;
            break;
        default:
            out[o++] = *p;
            break;
        }
    }
    out[o] = '\0';
}

/**
 * @brief 生成表单页面（启动时调用一次）
 * @note  WiFi 名称回填（不是秘密）；巴法云私钥**不回填**，只提示"已配置"，
 *        因为配网热点是开放的，回填等于把私钥公布给附近任何人。
 */
static void build_page(void)
{
    if (s_html == NULL)
    {
        s_html = (char *)heap_caps_malloc(PROV_HTML_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_html == NULL)
        {
            ESP_LOGE(TAG, "页面缓冲分配失败（%u 字节 PSRAM）", (unsigned)PROV_HTML_MAX);
            return;
        }
    }

    char ssid[NET_CFG_SSID_MAX] = "";
    (void)net_config_get_wifi(ssid, sizeof(ssid), NULL, 0);

    char ssid_esc[NET_CFG_SSID_MAX * 6];
    html_escape(ssid, ssid_esc, sizeof(ssid_esc));

    const char *uid_hint = net_config_has_bemfa() ? "已配置：留空保持不变" : "可留空，稍后再填";

    uint16_t port_now = APP_CFG_BEMFA_PORT_PLAIN;
    char topic_now[NET_CFG_TOPIC_CSV_MAX] = "";
    char topic_esc[NET_CFG_TOPIC_CSV_MAX * 6];
    (void)net_config_get_bemfa(NULL, 0, topic_now, sizeof(topic_now), &port_now);
    // 首次配网（NVS 还没有主题）时把推荐的四主题预填进表单，用户只需核对后保存；
    // NVS 里已有主题则回显实际值，不覆盖用户自己的命名。
    if (topic_now[0] == '\0')
    {
        snprintf(topic_now, sizeof(topic_now), "%s", APP_CFG_BEMFA_TOPICS_DEFAULT);
    }
    html_escape(topic_now, topic_esc, sizeof(topic_esc));

    snprintf(s_html, PROV_HTML_MAX,
             "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>智能门禁 配网</title><style>"
             "body{font-family:sans-serif;margin:0;padding:16px;background:#f5f6f8}"
             ".card{background:#fff;border-radius:10px;padding:18px;max-width:440px;margin:0 auto}"
             "h1{font-size:19px;margin:0 0 6px}.sub{font-size:13px;color:#888;margin-bottom:10px}"
             "label{display:block;font-size:14px;margin:14px 0 5px}"
             "input{width:100%%;box-sizing:border-box;padding:9px;font-size:15px;"
             "border:1px solid #ccd;border-radius:7px}"
             "button{width:100%%;margin-top:22px;padding:12px;font-size:16px;border:0;"
             "border-radius:7px;background:#2f7dff;color:#fff}"
             ".note{font-size:12px;color:#888;margin-top:14px;line-height:1.5}"
             "</style></head><body><div class=\"card\">"
             "<h1>智能门禁 · 网络配置</h1><div class=\"sub\">填写后点保存，设备重启并连接</div>"
             "<form method=\"post\" action=\"/save\">"
             "<label>WiFi 名称（SSID）</label>"
             "<input name=\"ssid\" maxlength=\"32\" required value=\"%s\">"
             "<label>WiFi 密码</label>"
             "<input name=\"pass\" type=\"password\" maxlength=\"64\" placeholder=\"开放网络可留空\">"
             "<label>巴法云私钥（UID）</label>"
             "<input name=\"bemfa_uid\" maxlength=\"40\" placeholder=\"%s\">"
             "<label>巴法云主题（逗号分隔，最多 4 个）</label>"
             "<input name=\"bemfa_topic\" maxlength=\"96\" value=\"%s\" "
             "placeholder=\"事件,命令,状态,陌生人\">"
             "<div class=\"sub\">依次为：门禁事件、远程命令、心跳回执、陌生人告警。"
             "只填前面几个也可以（只填 1 个则全部共用）。</div>"
             "<label>巴法云端口</label>"
             "<input name=\"bemfa_port\" type=\"number\" min=\"9501\" max=\"9503\" step=\"2\" "
             "maxlength=\"5\" value=\"%u\">"
             "<div class=\"sub\">9501 明文 MQTT；9503 TLS（校验证书）。</div>"
             "<button type=\"submit\">保存并重启</button></form>"
             "<p class=\"note\">保存后设备重启；热点消失说明已连上新网络。"
             "若连不上，本热点会在约 1 分钟后自动重新出现。</p>"
             "</div></body></html>",
             ssid_esc, uid_hint, topic_esc, (unsigned)port_now);
}

/**
 * @brief 从 urlencoded 表单中取字段并解码
 * @param body     请求体
 * @param key      字段名
 * @param out      输出缓冲
 * @param out_size 缓冲大小
 * @return true 找到该字段
 */
static bool form_get(const char *body, const char *key, char *out, size_t out_size)
{
    const size_t klen = strlen(key);
    const char *p = body;

    while (p != NULL && *p != '\0')
    {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            p += klen + 1;
            size_t o = 0;
            while (*p != '\0' && *p != '&' && o + 1 < out_size)
            {
                if (*p == '%' && p[1] != '\0' && p[2] != '\0')
                {
                    char hex[3] = {p[1], p[2], '\0'};
                    out[o++] = (char)strtol(hex, NULL, 16);
                    p += 3;
                }
                else if (*p == '+')
                {
                    out[o++] = ' ';
                    p++;
                }
                else
                {
                    out[o++] = *p++;
                }
            }
            out[o] = '\0';
            return true;
        }
        // 跳到下一个字段
        p = strchr(p, '&');
        if (p != NULL)
        {
            p++;
        }
    }
    return false;
}

/**
 * @brief 读取完整请求体（处理超时重试）
 * @return 实际读取字节数；< 0 表示失败
 */
static int read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size)
    {
        return -2; // 超限
    }
    size_t off = 0;
    while (off < (size_t)req->content_len)
    {
        int ret = httpd_req_recv(req, buf + off, (size_t)req->content_len - off);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            continue; // 超时：继续等剩余数据
        }
        if (ret <= 0)
        {
            return -1;
        }
        off += (size_t)ret;
    }
    buf[off] = '\0';
    return (int)off;
}

/**
 * @brief 打一行访问日志：来源 IP + User-Agent + 发送结果
 * @note  配网排查用。手机连上热点却打不开页面时，这一行能区分三种情况：
 *        1. 没有这行 → 请求根本没到设备（手机仍走移动数据 / 没连热点）；
 *        2. 有这行、发送失败 → 设备侧 socket 已断（多见于页面太大或客户端提前关闭）；
 *        3. 有这行、发送成功 → 设备已把完整页面发出，问题在手机浏览器/HTTPS 自动升级。
 */
static void log_access(httpd_req_t *req, const char *result)
{
#if PROV_LOG_ACCESS
    char ua[48] = "";
    if (httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua)) != ESP_OK)
    {
        ua[0] = '\0';
    }
    char peer[16] = "?";
    struct sockaddr_in6 sa;
    socklen_t sl = sizeof(sa);
    const int fd = httpd_req_to_sockfd(req);
    if (fd >= 0 && getpeername(fd, (struct sockaddr *)&sa, &sl) == 0)
    {
        // IPv4-mapped IPv6：打印最后 4 字节即可
        const uint8_t *p = (const uint8_t *)&sa.sin6_addr;
        snprintf(peer, sizeof(peer), "%u.%u.%u.%u", p[12], p[13], p[14], p[15]);
    }
    ESP_LOGI(TAG, "%s %s 来自 %s UA=%s", result, req->uri, peer, ua[0] ? ua : "(无)");
#else
    (void)req;
    (void)result;
#endif
}

/** @brief GET /：返回配置表单 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    log_access(req, "GET");
    if (s_html == NULL)
    {
        // 页面缓冲没分配成功（PSRAM 不足）时不要用 NULL 去 strlen/发送
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "页面未就绪");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // 用带长度的 httpd_resp_send（会带 Content-Length），不用分块接口：
    // 手机浏览器/系统门户浏览器对 chunked 短页面兼容性差（设备日志能看到 GET 到达、
    // 响应也发完，手机上却一直空白）。发送失败一定要打日志——设备端看起来“已处理”，
    // 实际 socket 已中断的情况就靠这行定位。
    const esp_err_t err = httpd_resp_send(req, s_html, (ssize_t)strlen(s_html));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "回送首页失败：%s", esp_err_to_name(err));
    }
    return err;
}

/** @brief POST /save：保存 WiFi（必填）与巴法云（可选），然后重启 */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    static char body[PROV_BODY_MAX]; // 单客户端场景，静态缓冲即可（HTTP 任务串行处理）
    const int len = read_body(req, body, sizeof(body));
    if (len == -2)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "表单过长");
        return ESP_FAIL;
    }
    if (len < 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "读取表单失败");
        return ESP_FAIL;
    }

    char ssid[NET_CFG_SSID_MAX] = "";
    char pass[NET_CFG_PASS_MAX] = "";
    char uid[NET_CFG_UID_MAX] = "";
    char topic[NET_CFG_TOPIC_CSV_MAX] = "";
    char port_s[8] = "";

    (void)form_get(body, "ssid", ssid, sizeof(ssid));
    (void)form_get(body, "pass", pass, sizeof(pass));
    (void)form_get(body, "bemfa_uid", uid, sizeof(uid));
    (void)form_get(body, "bemfa_topic", topic, sizeof(topic));
    (void)form_get(body, "bemfa_port", port_s, sizeof(port_s));

    if (ssid[0] == '\0')
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WiFi 名称不能为空");
        return ESP_FAIL;
    }

    // 只改巴法云参数时用户不会重输 WiFi 密码（表单不回填密码）。若 SSID 没变而密码
    // 留空，沿用旧密码；否则会把已有凭据写成空串，设备再也连不上。
    {
        char old_ssid[NET_CFG_SSID_MAX] = "";
        char old_pass[NET_CFG_PASS_MAX] = "";
        if (pass[0] == '\0' &&
            net_config_get_wifi(old_ssid, sizeof(old_ssid), old_pass, sizeof(old_pass)) == ESP_OK &&
            strcmp(old_ssid, ssid) == 0)
        {
            snprintf(pass, sizeof(pass), "%s", old_pass);
        }
    }

    // 巴法云：页面提示"留空保持不变"，所以逐个字段合并 NVS 里的旧值——
    // 只填主题（不重输私钥）或只填私钥（保留旧主题）都能正常工作。
    char old_uid[NET_CFG_UID_MAX] = "";
    char old_topic[NET_CFG_TOPIC_CSV_MAX] = "";
    uint16_t old_port = APP_CFG_BEMFA_PORT_PLAIN;
    const bool has_old = (net_config_get_bemfa(old_uid, sizeof(old_uid), old_topic,
                                               sizeof(old_topic), &old_port) == ESP_OK);
    if (uid[0] == '\0')
    {
        snprintf(uid, sizeof(uid), "%s", old_uid);
    }
    if (topic[0] == '\0')
    {
        snprintf(topic, sizeof(topic), "%s", old_topic);
    }
    if (uid[0] != '\0')
    {
        uint16_t port = has_old ? old_port : APP_CFG_BEMFA_PORT_PLAIN;
        if (port_s[0] != '\0')
        {
            const int v = atoi(port_s);
            if (v > 0 && v < 65536)
            {
                port = (uint16_t)v;
            }
        }
        esp_err_t berr = net_config_set_bemfa(uid, topic, port);
        ESP_LOGI(TAG, "巴法云参数写入：%s", esp_err_to_name(berr));
    }

    esp_err_t err = wifi_manager_apply_credentials(ssid, pass);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "保存 WiFi 凭据失败：%s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "配网参数已保存：%s（2 s 后重启）", ssid);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    static const char ok_page[] =
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<title>已保存</title></head><body style=\"font-family:sans-serif;"
        "padding:24px;text-align:center\"><h2>配置已保存</h2>"
        "<p>设备正在重启，请稍候。热点消失后请确认设备已连上新网络。</p>"
        "</body></html>";
    return httpd_resp_send(req, ok_page, (ssize_t)strlen(ok_page));
}

/** @brief GET /favicon.ico：返回 204，避免浏览器反复请求 */
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/**
 * @brief 系统“连上热点后自动检查网络”的探测请求：302 跳回门户首页
 * @note  Android 访问 /generate_204、iOS 访问 /hotspot-detect.html、Windows 访问
 *        /connecttest.txt。这些请求若返回 404，手机只会提示“无法访问互联网”，
 *        用户必须手动输入 IP；返回跳转后系统会弹出“登录网络”提示，直接进门户
 *        （**不需要真正的强制门户认证**，只是把自动探测变成跳转）。
 */
static esp_err_t portal_probe_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "门户探测：%s → 跳转首页", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, NULL, 0);
}

#if PROV_SELFTEST
/**
 * @brief 门户自检：从设备内部连 127.0.0.1:80 请求首页
 * @note  只用于区分“HTTP 服务本身没起来”和“手机没把请求发到热点”。
 *        结果写在串口日志里；排查完把 PROV_SELFTEST 置 0（发布版默认关闭）。
 */
static void prov_http_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500)); // 等 httpd 任务真正进入 accept 循环

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
    {
        ESP_LOGE(TAG, "自检：创建 socket 失败（errno=%d）", errno);
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ESP_LOGE(TAG, "自检：连接 127.0.0.1:80 失败（errno=%d）", errno);
        (void)close(s);
        vTaskDelete(NULL);
        return;
    }

    // 模拟手机浏览器：带 Accept/User-Agent 头，Host 为热点 IP
    static const char req[] = "GET / HTTP/1.1\r\n"
                              "Host: 192.168.4.1\r\n"
                              "User-Agent: Mozilla/5.0 (Linux; Android 14) Mobile Safari\r\n"
                              "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                              "Accept-Encoding: gzip, deflate\r\n"
                              "Connection: close\r\n\r\n";
    (void)send(s, req, sizeof(req) - 1, 0);

    // 把整个响应收下来，打印状态行 + 长度/编码头，便于判断是否为 chunked/被截断
    static char resp[2048]; // 自检任务独占，发布版（PROV_SELFTEST=0）不会编入
    int total = 0;
    for (;;)
    {
        const int n = recv(s, resp + total, sizeof(resp) - 1 - (size_t)total, 0);
        if (n <= 0)
        {
            break;
        }
        total += n;
        if (total >= (int)sizeof(resp) - 1)
        {
            break;
        }
    }
    (void)close(s);

    if (total <= 0)
    {
        ESP_LOGE(TAG, "自检：未收到应答（errno=%d）", errno);
        vTaskDelete(NULL);
        return;
    }
    resp[total] = '\0';
    const char *eol = strstr(resp, "\r\n");
    char status[64] = "";
    if (eol != NULL && (size_t)(eol - resp) < sizeof(status))
    {
        memcpy(status, resp, (size_t)(eol - resp));
        status[eol - resp] = '\0';
    }
    const bool chunked = (strstr(resp, "Transfer-Encoding: chunked") != NULL);
    const bool has_len = (strstr(resp, "Content-Length") != NULL);
    ESP_LOGI(TAG, "自检：%s | %d 字节 | Content-Length=%d chunked=%d | 含 </html>=%d", status,
             total, has_len ? 1 : 0, chunked ? 1 : 0, strstr(resp, "</html>") != NULL ? 1 : 0);
    vTaskDelete(NULL);
}
#endif /* PROV_SELFTEST */

esp_err_t prov_http_start(void)
{
    if (s_server != NULL)
    {
        return ESP_OK;
    }

    build_page();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8; // 首页 / 保存 / 图标 + 三个系统探测地址
    config.stack_size = PROV_STACK;
    config.task_priority = PROV_PRIO;
    config.max_open_sockets = PROV_MAX_SOCKETS;
    config.lru_purge_enable = true;
    // 让 httpd 自己的 URI 缓冲（scratch）小一些：默认按 max_uri_len 512 分配
    config.max_uri_len = 192;

    ESP_LOGI(TAG, "启动配网门户：栈 %u B，内部空闲=%u 最大块=%u", (unsigned)PROV_STACK,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start 失败：%s（内部空闲=%u 最大块=%u）", esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_get_handler,
        .user_ctx = NULL,
    };
    (void)httpd_register_uri_handler(s_server, &root);
    (void)httpd_register_uri_handler(s_server, &save);
    (void)httpd_register_uri_handler(s_server, &favicon);

    // 系统自动弹出门户用到的探测地址（见 portal_probe_handler 的说明）
    static const char *const probes[] = {
        "/generate_204",        // Android
        "/gen_204",             // Android 旧版本
        "/hotspot-detect.html", // iOS / macOS
        "/connecttest.txt",     // Windows
        "/ncsi.txt",            // Windows 旧版本
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
    {
        const httpd_uri_t probe = {
            .uri = probes[i],
            .method = HTTP_GET,
            .handler = portal_probe_handler,
            .user_ctx = NULL,
        };
        (void)httpd_register_uri_handler(s_server, &probe);
    }

    ESP_LOGI(TAG, "配网门户已启动：http://192.168.4.1");

#if PROV_SELFTEST
    if (xTaskCreate(prov_http_selftest_task, "prov_selftest", 3072, NULL, 2, NULL) != pdPASS)
    {
        ESP_LOGW(TAG, "自检任务创建失败（不影响门户）");
    }
#endif
    return ESP_OK;
}

void prov_http_stop(void)
{
    if (s_server != NULL)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

bool prov_http_is_running(void)
{
    return s_server != NULL;
}
