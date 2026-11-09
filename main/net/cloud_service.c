/**
 * @file    cloud_service.c
 * @brief   巴法云云端服务实现：MQTT 客户端 + 上报队列 + 下行命令执行。
 *
 * @note    1. 一个 cloud_task（Core0、优先级 3、栈 4 KB）串行处理三件事：
 *              上报队列（按 200 ms 间隔逐条发 QoS1）、心跳（30 s 一次 QoS0）、
 *              下行命令（开门 / 查询 / 改阈值 / 活体开关 / 重启）。串行是为了让阈值等
 *              会阻塞 face_task 的操作不与心跳、上报互相等待。
 *          2. 上报载荷直接用 access_log_get_line() 取原文：日志行本身就是完整 JSON，
 *              云端与本地看到的是同一份数据，也省掉第二套序列化。
 *          3. esp-mqtt 自己的任务栈按 4 KB 配置（明文 9501，无 TLS）——M4 之后内部 RAM
 *              只剩几 KB，栈必须省着用；MQTT 缓冲上下行各 1 KB，命令与日志行都够。
 *          4. 远程命令令牌 = 管理员 PIN（NVS），开门另限速 3 次/分钟；未带令牌或不符
 *              一律拒绝并回执 ok=0。
 *          5. 断网期间不丢数据：日志已落盘，队列只记住序号；重连后补发。队列满时只丢
 *              “实时推送”，本地记录仍然完整。
 */

#include "cloud_service.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "access_log.h"
#include "app_config.h"
#include "app_nvs.h"
#include "bemfa_proto.h"
#include "door_controller.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "face_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "net_config.h"
#include "wifi_manager.h"
#include "voice_prompt.h"

static const char *TAG = "cloud"; // 日志标签

/** @brief 上报队列深度（断网时最多记住的事件条数，超出的只在本地日志） */
#define CLOUD_EVENT_QUEUE_LEN 8
/** @brief 云端任务栈（不含 esp-mqtt 自己的任务栈） */
#define CLOUD_TASK_STACK (4 * 1024)
/** @brief 云端任务优先级（低于门禁/人脸，高于监控） */
#define CLOUD_TASK_PRIO 3
/** @brief 心跳周期 */
#define CLOUD_HEARTBEAT_MS 30000
/** @brief 相邻两条 QoS1 上报的最小间隔（巴法云限速较宽松，200 ms 足够稳妥） */
#define CLOUD_PUBLISH_GAP_MS 200
/**
 * @brief MQTT 接收缓冲大小
 * @note  只需容纳“主题 + 下行载荷”：命令主题 ≤32 字节、载荷上限 CLOUD_DOWNLINK_MAX
 *        (256)，512 足够。原来上下行都用 1024，MQTT 客户端启动时一次吃掉 7.3 KB
 *        内部 RAM（含任务栈 4 KB），识别首帧后再少 6 KB，界面工作线程就创建不出来。
 */
#define CLOUD_MQTT_IN_BUFFER 512
/** @brief MQTT 发送缓冲（esp-mqtt 的 outbox，QoS1 待确认消息排队；日志行约 200 字节） */
#define CLOUD_MQTT_OUT_BUFFER 1024
/** @brief 查询回执拼接缓冲（最多 3 条日志行 + 包头，放静态区省栈） */
#define CLOUD_QUERY_BUF 1024
/**
 * @brief 创建 MQTT 客户端所需的最小内部 RAM（空闲 / 最大连续块，字节）
 * @note  实测客户端创建 + 任务栈 4 KB + 收发缓冲共约 6.8 KB 内部 RAM；
 *        门限取 12 KB 空闲 / 8 KB 连续块，留出余量又不至于把正常启动挡在门外
 *        （实测语音初始化完成后：内部空闲约 24 KB、最大块 8 KB）。
 */
#define MQTT_START_MIN_INTERNAL (12 * 1024)
/** @brief 见 MQTT_START_MIN_INTERNAL */
#define MQTT_START_MIN_BLOCK (8 * 1024)
/** @brief 远程开门限速：窗口内最多 3 次 */
#define CLOUD_OPEN_WINDOW_MS 60000
#define CLOUD_OPEN_MAX       3
/** @brief 下行载荷上限（超过直接丢弃，避免截断后误解析） */
#define CLOUD_DOWNLINK_MAX 256

/** @brief 队列事件类型 */
typedef enum
{
    CLOUD_EVT_LOG = 1, /**< 有日志待上报 */
    CLOUD_EVT_CMD,     /**< 收到下行命令 */
} cloud_evt_kind_t;

/** @brief 队列元素（日志事件只用 seq，命令事件用 req） */
typedef struct
{
    cloud_evt_kind_t kind;
    uint32_t seq;
    uint8_t slot; /**< 日志事件的上行主题槽位（ACCESS/STRANGER） */
    bemfa_request_t req;
} cloud_event_t;

static esp_mqtt_client_handle_t s_client = NULL;
static QueueHandle_t s_queue = NULL;
static volatile cloud_state_t s_state = CLOUD_STATE_UNSET;
static char s_uid[NET_CFG_UID_MAX];
/** @brief 实际使用的主题（按槽位解析后），空串表示该槽位未配置 */
static char s_topics[BEMFA_TOPIC_NUM][NET_CFG_TOPIC_MAX];
static char s_topic_csv[NET_CFG_TOPIC_CSV_MAX]; /**< NVS 里的逗号分隔原文 */
static uint16_t s_port = APP_CFG_BEMFA_PORT_PLAIN;
static bool s_cfg_valid = false;
/** @brief 是否已配置 WiFi（未配置时连不上 BSSID，不必创建 MQTT 客户端） */
static bool s_has_wifi_cfg = false;
/** @brief 配网期间暂停：不再创建 MQTT 客户端（内部 RAM 让给配网门户） */
static volatile bool s_paused = false;
static int64_t s_last_heartbeat_ms = 0;
/** @brief MQTT 资源不足或初始化失败后的重试时间 */
static int64_t s_next_mqtt_retry_ms = 0;
/** @brief 资源门限未满足时的最后一次诊断日志时间 */
static int64_t s_last_mqtt_resource_log_ms = 0;
static int64_t s_open_marks[CLOUD_OPEN_MAX] = {0};

/** @brief 查询回执拼接缓冲（单任务独享，放静态区省栈） */
static char s_query_buf[CLOUD_QUERY_BUF];

/** @brief 时间戳：与日志口径一致，对时前用开机毫秒 */
static int64_t now_ts_ms(void)
{
    const time_t now = time(NULL);
    if (now > (time_t)1700000000)
    {
        return (int64_t)now * 1000;
    }
    return esp_timer_get_time() / 1000;
}

/** @brief 取 STA 的 IP 字符串（未连接返回 "0.0.0.0"） */
static void get_ip(char *buf, size_t size)
{
    if (buf == NULL || size == 0)
    {
        return;
    }
    snprintf(buf, size, "0.0.0.0");
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL)
    {
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0)
    {
        snprintf(buf, size, IPSTR, IP2STR(&ip.ip));
    }
}

/**
 * @brief 把 NVS 里的逗号分隔主题解析到槽位
 * @param csv 形如 "doorcam,doorcam_cmd,doorcam_status,doorcam_stranger"（允许只填前几项）
 * @return 有效槽位数（至少 1）
 * @note  降级映射（用户只登记了较少的主题时）：
 *          1 个：全部槽位都用它（旧的单主题行为）；
 *          2 个：第 2 个专做命令下行（CMD），其余走上行；
 *          3 个：第 2 个 = CMD、第 3 个 = STATUS，其余（ACCESS/STRANGER）走第 1 个；
 *          4 个：一一对应。
 *        填了空项（如 "a,,c"）按“跳过该项”处理。
 */
static int resolve_topics(const char *csv)
{
    char fields[BEMFA_TOPIC_NUM][NET_CFG_TOPIC_MAX];
    int count = 0;

    const char *p = csv;
    while (p != NULL && *p != '\0' && count < BEMFA_TOPIC_NUM)
    {
        const char *comma = strchr(p, ',');
        const size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);
        if (len > 0 && len < NET_CFG_TOPIC_MAX)
        {
            memcpy(fields[count], p, len);
            fields[count][len] = '\0';
            // 去掉首尾空白（表单输入很容易带空格）
            char *s = fields[count];
            while (*s == ' ')
            {
                s++;
            }
            char *e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n'))
            {
                *--e = '\0';
            }
            if (s[0] != '\0')
            {
                memmove(fields[count], s, strlen(s) + 1);
                count++;
            }
        }
        p = comma != NULL ? comma + 1 : NULL;
    }

    for (int i = 0; i < BEMFA_TOPIC_NUM; i++)
    {
        s_topics[i][0] = '\0';
    }
    if (count == 0)
    {
        return 0;
    }
    if (count == 1)
    {
        for (int i = 0; i < BEMFA_TOPIC_NUM; i++)
        {
            snprintf(s_topics[i], NET_CFG_TOPIC_MAX, "%s", fields[0]);
        }
    }
    else if (count == 2)
    {
        snprintf(s_topics[BEMFA_TOPIC_CMD], NET_CFG_TOPIC_MAX, "%s", fields[1]);
        snprintf(s_topics[BEMFA_TOPIC_ACCESS], NET_CFG_TOPIC_MAX, "%s", fields[0]);
        snprintf(s_topics[BEMFA_TOPIC_STRANGER], NET_CFG_TOPIC_MAX, "%s", fields[0]);
        snprintf(s_topics[BEMFA_TOPIC_STATUS], NET_CFG_TOPIC_MAX, "%s", fields[0]);
    }
    else
    {
        snprintf(s_topics[BEMFA_TOPIC_ACCESS], NET_CFG_TOPIC_MAX, "%s", fields[0]);
        snprintf(s_topics[BEMFA_TOPIC_STRANGER], NET_CFG_TOPIC_MAX, "%s",
                 count >= 4 ? fields[3] : fields[0]);
        snprintf(s_topics[BEMFA_TOPIC_CMD], NET_CFG_TOPIC_MAX, "%s", fields[1]);
        snprintf(s_topics[BEMFA_TOPIC_STATUS], NET_CFG_TOPIC_MAX, "%s", fields[2]);
    }
    return count;
}

/** @brief 发布一条消息到指定槽位；未连接/槽位为空时返回 -1 */
static int publish_to(bemfa_topic_slot_t slot, const char *data, int len, int qos)
{
    if (s_client == NULL || s_state != CLOUD_STATE_CONNECTED || data == NULL)
    {
        return -1;
    }
    const char *topic = s_topics[slot];
    if (topic[0] == '\0')
    {
        return -1;
    }
    return esp_mqtt_client_publish(s_client, topic, data, len, qos, 0);
}

/** @brief 立即发布一次状态心跳 */
static void publish_status(void)
{
    char ip[16];
    get_ip(ip, sizeof(ip));

    char buf[256];
    face_face_state_t fs;
    memset(&fs, 0, sizeof(fs));
    face_service_get_face_state(&fs); // 无返回值；未初始化时字段为 0
    const int enrolled = fs.enrolled_num > 0 ? fs.enrolled_num : -1;
    const size_t n = bemfa_build_status(buf, sizeof(buf), access_log_get_seq(), now_ts_ms(),
                                        APP_CFG_VERSION, (int)wifi_manager_get_rssi(), ip, enrolled,
                                        (uint32_t)(esp_timer_get_time() / 1000000));
    if (n > 0)
    {
        (void)publish_to(BEMFA_TOPIC_STATUS, buf, (int)n, 0);
    }
}

/** @brief 上报一条门禁日志（原文 JSON）到指定主题槽位 */
static void publish_log_seq(uint32_t seq, uint8_t slot)
{
    if (seq == 0 || s_state != CLOUD_STATE_CONNECTED)
    {
        return;
    }
    char line[ACCESS_LOG_LINE_MAX + 1]; // 原样 JSON 行；fgets 需要多 1 字节
    if (!access_log_get_line(seq, line, sizeof(line)))
    {
        ESP_LOGW(TAG, "待上报日志 #%u 已不在文件里（轮转/清空），跳过", (unsigned)seq);
        return;
    }
    const int len = (int)strlen(line);
    if (publish_to((bemfa_topic_slot_t)slot, line, len, 1) < 0)
    {
        ESP_LOGW(TAG, "上报 #%u 失败（未连接/缓冲不足）", (unsigned)seq);
    }
    else
    {
        ESP_LOGI(TAG, "已上报 #%u（%d 字节，qos1）", (unsigned)seq, len);
    }
}

/**
 * @brief 远程开门限速检查：窗口内未超过 CLOUD_OPEN_MAX 时记录并放行
 * @note  只让“仍在窗口内”的记录参与判断：旧实现按时间戳找“最旧”的记录
 *        （0 也算），已滑出窗口的记录时间戳更早、会被反复选中并覆盖，
 *        窗口内的真实记录因此永远清不掉——表现为远程开门只成功一次，
 *        之后所有请求都被判“超过 3 次/分钟”。0 表示空槽，直接跳过。
 */
static bool open_rate_ok(void)
{
    const int64_t now = esp_timer_get_time() / 1000;
    int in_window = 0;
    for (int i = 0; i < CLOUD_OPEN_MAX; i++)
    {
        if (s_open_marks[i] != 0 && now - s_open_marks[i] < CLOUD_OPEN_WINDOW_MS)
        {
            in_window++;
        }
    }
    if (in_window >= CLOUD_OPEN_MAX)
    {
        return false;
    }
    // 只在“空槽 / 已滑出窗口”的槽位上记录；不要覆盖窗口内的记录
    // （覆盖会让窗口计数变少，限速形同虚设）
    for (int i = 0; i < CLOUD_OPEN_MAX; i++)
    {
        if (s_open_marks[i] == 0 || now - s_open_marks[i] >= CLOUD_OPEN_WINDOW_MS)
        {
            s_open_marks[i] = now;
            return true;
        }
    }
    return false; // 不会走到（in_window < MAX 时必有可用槽）
}

/** @brief 令牌校验（管理员 PIN） */
static bool token_ok(const char *token)
{
    if (token == NULL || token[0] == '\0')
    {
        return false;
    }
    char expect[16];
    snprintf(expect, sizeof(expect), "%u", (unsigned)app_nvs_get_admin_pin());
    return strcmp(token, expect) == 0;
}

/**
 * @brief 查询最近 3 条门禁记录并发布
 * @note  先把 JSON 行取到缓冲末尾，再 memmove 前移补逗号：避免再开一份行缓冲
 *        （cloud_task 栈只有 4 KB）。
 */
static void publish_query_result(void)
{
    char *buf = s_query_buf;
    const size_t cap = sizeof(s_query_buf);
    int n = snprintf(buf, cap, "{\"type\":\"query\",\"seq\":%u,\"items\":[",
                     (unsigned)access_log_get_seq());
    if (n < 0 || (size_t)n >= cap)
    {
        return;
    }

    const uint32_t last = access_log_get_seq();
    int added = 0;
    for (int i = 0; i < 3 && last > (uint32_t)i; i++)
    {
        char *line = buf + n;
        const size_t room = cap - (size_t)n;
        if (room <= 2 || !access_log_get_line(last - (uint32_t)i, line, room))
        {
            continue;
        }
        const size_t line_len = strlen(line);
        const char *sep = added > 0 ? "," : "";
        const size_t sep_len = strlen(sep);
        memmove(line + sep_len, line, line_len + 1);
        memcpy(line, sep, sep_len);
        n += (int)(sep_len + line_len);
        added++;
    }
    const int w = snprintf(buf + n, cap - (size_t)n, "]}");
    if (w < 0 || (size_t)(n + w) >= cap)
    {
        return; // 拼不下就整条不发（宁可无回执，也不发截断的 JSON）
    }
    n += w;
    (void)publish_to(BEMFA_TOPIC_STATUS, buf, n, 0);
    ESP_LOGI(TAG, "查询回执：%d 条记录（%d 字节）", added, n);
}

/** @brief 回执一条命令 */
static void send_ack(bemfa_cmd_t cmd, bool ok)
{
    char ack[64];
    const size_t n = bemfa_build_ack(ack, sizeof(ack), cmd, ok);
    if (n > 0)
    {
        (void)publish_to(BEMFA_TOPIC_STATUS, ack, (int)n, 0);
    }
}

/** @brief 执行一条下行命令 */
static void handle_cmd(const bemfa_request_t *req)
{
    if (req == NULL || req->cmd == BEMFA_CMD_NONE)
    {
        return;
    }

    // 所有命令都要求令牌：巴法云主题名可能被猜到，不能只靠私钥隔离
    if (!token_ok(req->token))
    {
        ESP_LOGW(TAG, "远程命令 %s 被拒绝：令牌错误", bemfa_cmd_name(req->cmd));
        send_ack(req->cmd, false);
        return;
    }

    switch (req->cmd)
    {
    case BEMFA_CMD_OPEN:
        if (open_rate_ok())
        {
            const door_open_info_t info = {
                .source = DOOR_SOURCE_REMOTE,
                .name = NULL,
                .similarity = -1.0f,
                .liveness = -1.0f,
                .extra = "bemfa",
                .track_id = -1,
            };
            const bool ok = (door_controller_open(&info) == ESP_OK);
            ESP_LOGW(TAG, "远程开门：%s", ok ? "已触发" : "队列满");
            if (ok)
            {
                (void)voice_prompt_say(VOICE_PROMPT_REMOTE);
            }
            send_ack(req->cmd, ok);
        }
        else
        {
            ESP_LOGW(TAG, "远程开门被限速（>%d 次/分钟）", CLOUD_OPEN_MAX);
            send_ack(req->cmd, false);
        }
        break;

    case BEMFA_CMD_QUERY_LOG:
        publish_query_result();
        send_ack(req->cmd, true);
        break;

    case BEMFA_CMD_SET_THRESHOLD:
    {
        const bool ok =
            (req->value > 0.0f && req->value < 1.0f) &&
            (face_service_set_threshold(req->value) == ESP_OK);
        ESP_LOGI(TAG, "远程设置阈值 %.2f：%s", req->value, ok ? "成功" : "失败");
        send_ack(req->cmd, ok);
        break;
    }

    case BEMFA_CMD_SET_LIVENESS:
    {
        const bool enable = (req->value > 0.5f);
        const bool ok = (req->value >= 0.0f) && (face_service_set_liveness(enable) == ESP_OK);
        ESP_LOGI(TAG, "远程设置活体 %d：%s", enable ? 1 : 0, ok ? "成功" : "失败");
        send_ack(req->cmd, ok);
        break;
    }

    case BEMFA_CMD_REBOOT:
        ESP_LOGW(TAG, "远程重启：300 ms 后执行");
        send_ack(req->cmd, true);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        break;

    case BEMFA_CMD_NONE:
    default:
        break;
    }
}

/**
 * @brief 订阅所有已配置的主题（巴法云控制台只把“被订阅”的主题显示为在线）
 * @note  1. 设备只在命令主题上接收下行命令，其余主题订阅只是为了在巴法云控制台
 *          显示“在线”：巴法云按“是否有客户端订阅”判定主题在线，只发布不订阅的
 *          主题会一直显示离线，让人误以为设备没连上（实测只订阅命令主题时，
 *          控制台仅门禁命令主题在线）。
 *        2. 订阅后自己 QoS1 上报的日志会回送一份，MQTT_EVENT_DATA 只认命令主题，
 *          其它主题的消息直接丢弃，不会重复执行。
 *        3. QoS：命令主题用 1（命令不能丢），其余用 0（只做在线指示）。
 *        4. 降级映射（只登记 1~2 个主题）会让多个槽位指向同一主题，按字符串去重。
 */
static void subscribe_topics(void)
{
    if (s_client == NULL)
    {
        return;
    }

    const char *done[BEMFA_TOPIC_NUM];
    int done_n = 0;
    for (int i = 0; i < BEMFA_TOPIC_NUM; i++)
    {
        const char *topic = s_topics[i];
        if (topic[0] == '\0')
        {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < done_n; j++)
        {
            if (strcmp(done[j], topic) == 0)
            {
                dup = true;
                break;
            }
        }
        if (dup)
        {
            continue;
        }
        done[done_n++] = topic;

        // 同一主题可能被多个槽位共用：只要其中有命令槽位就按 QoS1 订阅
        int qos = 0;
        for (int k = 0; k < BEMFA_TOPIC_NUM; k++)
        {
            if (k == BEMFA_TOPIC_CMD && strcmp(s_topics[k], topic) == 0)
            {
                qos = 1;
                break;
            }
        }
        (void)esp_mqtt_client_subscribe(s_client, topic, qos);
    }
    ESP_LOGI(TAG, "已订阅 %d 个主题（仅命令主题接收下行）", done_n);
}

/**
 * @brief MQTT 事件回调（esp-mqtt 任务上下文）
 * @note  只做轻量动作：改状态、订阅、把下行载荷入队。
 *        开门/改阈值等会阻塞的操作一律交给 cloud_task。
 */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        s_state = CLOUD_STATE_CONNECTED;
        // 带内部 RAM：MQTT 连接后内部 RAM 会再掉一截（outbox / 收发缓冲），
        // 配网门户与 UI 工作线程都要在这之后创建，余额必须看得见。
        ESP_LOGI(TAG, "MQTT 已连接（内部剩余=%u 最大块=%u，%s:%u）：事件=%s 陌生人=%s 状态=%s "
                      "命令=%s",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 APP_CFG_BEMFA_HOST, (unsigned)s_port, s_topics[BEMFA_TOPIC_ACCESS],
                 s_topics[BEMFA_TOPIC_STRANGER], s_topics[BEMFA_TOPIC_STATUS],
                 s_topics[BEMFA_TOPIC_CMD]);
        subscribe_topics();
        publish_status();
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_state = CLOUD_STATE_CONNECTING;
        // MQTT_EVENT_ERROR 紧接着必然到来，这里不再重复打日志（每 5 s 两次太吵，
        // 真正的原因看 esp-tls/mqtt_client 打出的错误行）
        ESP_LOGD(TAG, "MQTT 已断开");
        break;

    case MQTT_EVENT_DATA:
        if (ev == NULL || ev->data == NULL || ev->data_len <= 0)
        {
            break;
        }
        if (ev->total_data_len != ev->data_len)
        {
            ESP_LOGW(TAG, "下行载荷被分包（%d/%d），丢弃", ev->data_len, ev->total_data_len);
            break;
        }
        // 只接受命令主题上的下行消息。ev->topic 是“主题 + NUL”的临时缓冲（不是
        // 可续接的字符串），所以按长度比较；若命令主题与上报主题是同一个（用户只登记
        // 了一个主题），设备自己 QoS1 上报回来的消息也会出现在这里——它们没有 cmd 字段，
        // 被 bemfa_parse() 拒绝，不会误执行。
        if (ev->topic_len > 0 &&
            (ev->topic_len != (int)strlen(s_topics[BEMFA_TOPIC_CMD]) ||
             strncmp(ev->topic, s_topics[BEMFA_TOPIC_CMD], (size_t)ev->topic_len) != 0))
        {
            break;
        }
        if (ev->data_len > CLOUD_DOWNLINK_MAX)
        {
            ESP_LOGW(TAG, "下行载荷过长（%d 字节），丢弃", ev->data_len);
            break;
        }
        {
            bemfa_request_t req;
            if (!bemfa_parse(ev->data, (size_t)ev->data_len, &req))
            {
                // 巴法云控制台“推送消息”发的是纯文本（如 “open”），不是 JSON；
                // 记一行下行原文，方便区分“消息没到”和“格式不认识”。
                ESP_LOGW(TAG, "下行载荷无法解析（%d 字节，首字节 0x%02x）", ev->data_len,
                         (unsigned)(uint8_t)ev->data[0]);
                break;
            }
            cloud_event_t item;
            memset(&item, 0, sizeof(item));
            item.kind = CLOUD_EVT_CMD;
            item.req = req;
            if (s_queue != NULL && xQueueSend(s_queue, &item, 0) == pdTRUE)
            {
                ESP_LOGI(TAG, "收到远程命令：%s", bemfa_cmd_name(req.cmd));
            }
            else
            {
                ESP_LOGW(TAG, "命令队列满，丢弃 %s", bemfa_cmd_name(req.cmd));
            }
        }
        break;

    case MQTT_EVENT_ERROR:
    {
        const esp_mqtt_error_codes_t *ec = (ev != NULL) ? ev->error_handle : NULL;
        if (ec == NULL)
        {
            ESP_LOGW(TAG, "MQTT 连接失败：无错误详情（检查 WiFi、DNS、服务器及端口）");
            break;
        }

        if (ec->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGE(TAG, "巴法云拒绝 MQTT 连接：返回码=%d（检查 UID 私钥及账号状态）",
                     (int)ec->connect_return_code);
        }
        else if (ec->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(TAG,
                     "MQTT 传输失败：ESP=%s TLS栈=%d 证书标志=0x%x socket errno=%d"
                     "（检查网络、DNS、端口与 TLS 配置）",
                     ec->esp_tls_last_esp_err != ESP_OK
                         ? esp_err_to_name(ec->esp_tls_last_esp_err)
                         : "ESP_OK",
                     ec->esp_tls_stack_err, ec->esp_tls_cert_verify_flags,
                     ec->esp_transport_sock_errno);
        }
        else
        {
            ESP_LOGE(TAG, "MQTT 错误：类型=%d，稍后自动重试", (int)ec->error_type);
        }
        break;
    }

    default:
        break;
    }
}

/**
 * @brief 创建并启动 MQTT 客户端（幂等）
 * @return ESP_OK 已启动（或早前已启动）
 * @note  连接地址用静态缓冲：esp-mqtt 的 esp_mqtt_client_config_t 只保存指针，URI 若在
 *        栈上会在返回后失效。9503 必须使用 mqtts:// 并校验系统 CA 证书。
 */
static esp_err_t start_mqtt_client(void)
{
    if (s_client != NULL)
    {
        return ESP_OK;
    }

    static char s_uri[96];
    const bool use_tls = s_port == APP_CFG_BEMFA_PORT_TLS;
    snprintf(s_uri, sizeof(s_uri), "%s://%s:%u", use_tls ? "mqtts" : "mqtt",
             APP_CFG_BEMFA_HOST, (unsigned)s_port);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .broker.verification.crt_bundle_attach = use_tls ? esp_crt_bundle_attach : NULL,
        .credentials.client_id = s_uid, // 巴法云：私钥即 client id，用户名/密码留空
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = CLOUD_MQTT_IN_BUFFER,
        .buffer.out_size = CLOUD_MQTT_OUT_BUFFER,
        .task.stack_size = 4 * 1024,
        .task.priority = 3,
    };
    ESP_LOGI(TAG, "创建 MQTT 客户端前：内部剩余=%u 最大块=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL)
    {
        ESP_LOGE(TAG, "创建 MQTT 客户端失败");
        return ESP_ERR_NO_MEM;
    }
    (void)esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    const esp_err_t start_err = esp_mqtt_client_start(s_client);
    if (start_err != ESP_OK)
    {
        ESP_LOGE(TAG, "启动 MQTT 客户端失败：%s", esp_err_to_name(start_err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return start_err;
    }
    ESP_LOGI(TAG, "MQTT 客户端已启动（%s，%s）：内部剩余=%u 最大块=%u", s_uri,
             use_tls ? "TLS/CA 校验" : "明文",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;
}

/** @brief 云端任务：串行处理上报 / 心跳 / 命令 */
static void cloud_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "云端任务已启动（心跳 %d s）", CLOUD_HEARTBEAT_MS / 1000);

    for (;;)
    {
        // 已配置 WiFi 且连上路由后才建 MQTT 客户端（见 cloud_service_init 的说明）
        /* 内部 RAM 不够就稍后再建（重试由本循环 100 ms 一拍自然完成）。
         * esp_mqtt_client_init 约要 6.8 KB 内部 RAM；实测与 voice_assistant_init()
         * 的 AFE/MultiNet 创建并发时，内部空闲会瞬间掉到 19 B / 最大块 0——
         * 此时建 MQTT 不只失败，还会在 esp_err_timer/ets_timer 里
         * ESP_ERROR_CHECK(ESP_ERR_NO_MEM) abort，表现为上电约一半概率反复重启。
         * 这里加一道硬门限：低于 MQTT_START_MIN_INTERNAL 就不碰，等语音初始化
         * 释放临时块后再建（正常情况下 WiFi 连上时已远超这个值）。 */
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_client == NULL && !s_paused && s_has_wifi_cfg && wifi_manager_is_connected() &&
            now_ms >= s_next_mqtt_retry_ms)
        {
            const size_t in_free =
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            const size_t in_block =
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (in_free >= MQTT_START_MIN_INTERNAL && in_block >= MQTT_START_MIN_BLOCK)
            {
                if (start_mqtt_client() != ESP_OK)
                {
                    s_next_mqtt_retry_ms = now_ms + 5000;
                }
            }
            else if (now_ms - s_last_mqtt_resource_log_ms >= 10000)
            {
                s_last_mqtt_resource_log_ms = now_ms;
                ESP_LOGW(TAG, "等待 MQTT 所需内部内存：空闲=%u/%u 最大块=%u/%u",
                         (unsigned)in_free, (unsigned)MQTT_START_MIN_INTERNAL,
                         (unsigned)in_block, (unsigned)MQTT_START_MIN_BLOCK);
            }
        }

        cloud_event_t item;
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (item.kind == CLOUD_EVT_LOG)
            {
                publish_log_seq(item.seq, item.slot);
                vTaskDelay(pdMS_TO_TICKS(CLOUD_PUBLISH_GAP_MS)); // 与下一条拉开间隔
            }
            else if (item.kind == CLOUD_EVT_CMD)
            {
                handle_cmd(&item.req);
            }
        }

        const int64_t now = esp_timer_get_time() / 1000;
        if (s_state == CLOUD_STATE_CONNECTED && now - s_last_heartbeat_ms >= CLOUD_HEARTBEAT_MS)
        {
            s_last_heartbeat_ms = now;
            publish_status();
        }
    }
}

esp_err_t cloud_service_init(void)
{
    if (s_queue != NULL)
    {
        return ESP_OK; // 幂等（MQTT 客户端可能是稍后惰性创建的，不能用 s_client 判断）
    }

    const esp_err_t cfg_err = net_config_get_bemfa(s_uid, sizeof(s_uid), s_topic_csv,
                                                   sizeof(s_topic_csv), &s_port);
    if (cfg_err != ESP_OK || s_uid[0] == '\0')
    {
        s_state = CLOUD_STATE_UNSET;
        ESP_LOGW(TAG, "未配置巴法云私钥，云端功能关闭（可在配网页面填写）");
        return ESP_OK;
    }
    const int topic_count = resolve_topics(s_topic_csv);
    if (topic_count == 0)
    {
        ESP_LOGW(TAG, "已配置私钥但未填主题：只发心跳（无主题无法发布）");
    }
    s_cfg_valid = true;
    s_state = CLOUD_STATE_CONNECTING;
    s_has_wifi_cfg = (net_config_get_wifi(NULL, 0, NULL, 0) == ESP_OK);

    s_queue = xQueueCreate(CLOUD_EVENT_QUEUE_LEN, sizeof(cloud_event_t));
    if (s_queue == NULL)
    {
        ESP_LOGE(TAG, "创建云端队列失败");
        return ESP_ERR_NO_MEM;
    }

    // MQTT 客户端由 cloud_task 在 WiFi 已连接且内存门限满足后创建：配网期内部 RAM 只剩 ~11 KB、
    // 最大块 ~4 KB，MQTT 任务栈 + 收发缓冲会把这些块切碎，导致配网门户 httpd_start
    // 申请不到任务栈（ESP_ERR_HTTPD_TASK，现场表现：手机连上热点但 192.168.4.1 打不开）。
    // 未配置 WiFi 时连 BSSID 都没有，客户端本身也只会反复 DNS 失败。
    ESP_LOGI(TAG, "云端参数已就绪，等待 WiFi 连接及内部内存门限后创建 MQTT 客户端");

    if (xTaskCreatePinnedToCore(cloud_task, "cloud_task", CLOUD_TASK_STACK, NULL, CLOUD_TASK_PRIO,
                                NULL, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "创建云端任务失败（内部 RAM 不足）");
        s_state = CLOUD_STATE_DISABLED;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "云端已启用：%d 个主题，服务器 %s:%u（%s）", topic_count,
             APP_CFG_BEMFA_HOST, (unsigned)s_port,
             s_port == APP_CFG_BEMFA_PORT_TLS ? "TLS" : "明文");
    return ESP_OK;
}

bool cloud_service_is_configured(void)
{
    return s_cfg_valid;
}

void cloud_service_pause(bool pause)
{
    s_paused = pause;
    if (!pause)
    {
        return;
    }

    // 已经有客户端时一并释放：MQTT 任务栈 4 KB + 收发缓冲正占着配网门户要用的
    // 内部 RAM（最大连续块只剩 4~5 KB 时 httpd_start 返回 ESP_ERR_HTTPD_TASK）。
    // 先摘句柄再停：停止过程中事件回调仍在跑，回调里用 s_client 的地方都做了空判断。
    if (s_client != NULL)
    {
        esp_mqtt_client_handle_t c = s_client;
        s_client = NULL;
        s_state = CLOUD_STATE_CONNECTING;
        esp_mqtt_client_stop(c);
        esp_mqtt_client_destroy(c);
        ESP_LOGI(TAG, "配网期间云端暂停：MQTT 客户端已释放");
    }
    else
    {
        ESP_LOGI(TAG, "配网期间云端暂停：不创建 MQTT 客户端");
    }
}

cloud_state_t cloud_service_get_state(void)
{
    return s_state;
}

void cloud_service_notify_log(uint32_t seq, const char *type)
{
    if (s_queue == NULL || seq == 0)
    {
        return;
    }
    if (type != NULL && strcmp(type, ACCESS_LOG_TYPE_STATUS) == 0)
    {
        return; // 状态类日志由心跳负责，不逐条上报
    }
    cloud_event_t item;
    memset(&item, 0, sizeof(item));
    item.kind = CLOUD_EVT_LOG;
    item.seq = seq;
    item.slot = (type != NULL && strcmp(type, ACCESS_LOG_TYPE_STRANGER) == 0)
                    ? BEMFA_TOPIC_STRANGER
                    : BEMFA_TOPIC_ACCESS;
    item.req.cmd = BEMFA_CMD_NONE;
    item.req.value = -1.0f;
    if (xQueueSend(s_queue, &item, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "上报队列满，跳过 #%u（本地日志仍在）", (unsigned)seq);
    }
}
