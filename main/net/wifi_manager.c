/**
 * @file    wifi_manager.c
 * @brief   WiFi 连接管理实现：STA 连接/自动重连、SoftAP 配网、配网后重启生效。
 *
 * @note    1. 全程只用事件回调，不开轮询任务：ESP-IDF 的 WiFi 事件在系统事件任务
 *              （esp_event 默认循环，内部 RAM）里分发，重连用 esp_timer 一次性定时器，
 *              不占用 Core0 的 CPU，也不与 face_task 抢栈。
 *          2. esp_wifi_set_storage(WIFI_STORAGE_RAM)：凭据我们自己存在 NVS 的 net_cfg 里，
 *              不让 WiFi 驱动再存一份（两份会不同步，换 AP 时出现"连上旧密码"）。
 *          3. 配网热点为 APSTA：STA 侧继续按凭据重连，一旦成功 IP_EVENT_STA_GOT_IP 会置位，
 *              界面上就能看到"已连接"（不过保存新凭据仍会重启，逻辑更简单）。
 */

#include "wifi_manager.h"

#include <string.h>

#include "app_config.h"
#include "app_events.h"
#include "cloud_service.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "net_config.h"
#include "prov_http.h"

static const char *TAG = "wifi_manager"; // 日志标签

/* ==================== 内部状态 ==================== */
/** @brief 当前状态（任意任务读取，枚举读写是原子的） */
static volatile wifi_manager_state_t s_state = WIFI_MGR_NO_CONFIG;
/** @brief esp_wifi_start() 是否已调用（只初始化一次） */
static bool s_wifi_started = false;
/** @brief 是否已保存凭据 */
static bool s_has_creds = false;
/** @brief 连续失败次数（拿到 IP 后清零） */
static int s_retry = 0;
/** @brief 保存的凭据副本（连接时使用，避免每次读 NVS） */
static char s_ssid[NET_CFG_SSID_MAX];
static char s_pass[NET_CFG_PASS_MAX];
/** @brief 配网热点名（"DoorCam-XXXX"） */
static char s_ap_ssid[24] = APP_CFG_NAME;
/** @brief 重连 / 重启定时器 */
static esp_timer_handle_t s_retry_timer = NULL;
static esp_timer_handle_t s_reboot_timer = NULL;
/** @brief 是否已创建 netif（防止重复初始化） */
static bool s_netif_ready = false;
/** @brief 配网热点配置是否已写入驱动（避免 AP 启动后再改配置导致重启热点） */
static bool s_ap_cfg_applied = false;
/** @brief 是否暂停主动连接（见 wifi_manager_set_connect_paused 的说明） */
static volatile bool s_connect_paused = false;

/**
 * @brief 重连定时器回调：发起一次连接
 * @note  esp_timer 回调在定时器任务里执行，esp_wifi_connect() 不阻塞。
 */
static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (!s_wifi_started)
    {
        return;
    }
    if (s_connect_paused)
    {
        ESP_LOGD(TAG, "连接已暂停（等待语音引擎初始化完成），本次重连跳过");
        return;
    }
    ESP_LOGI(TAG, "尝试连接 WiFi：%s", s_ssid);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_connect 返回 %s", esp_err_to_name(err));
    }
}

/**
 * @brief 重启定时器回调（配网保存后延迟重启，先让 HTTP 响应发完）
 */
static void reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "配网参数已保存，重启生效");
    esp_restart();
}

/**
 * @brief 安排一次重连
 * @param delay_ms 延时
 */
static void schedule_retry(uint32_t delay_ms)
{
    if (s_retry_timer != NULL)
    {
        esp_timer_stop(s_retry_timer); // 已有等待中的重连则重新计时
        esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
    }
}

/**
 * @brief WiFi / IP 事件处理
 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            if (s_has_creds && !wifi_manager_is_provisioning() && !s_connect_paused)
            {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
            xEventGroupClearBits(g_sys_events, SYS_EVENT_WIFI_CONNECTED);

            if (!s_has_creds)
            {
                break; // 没有凭据：等用户配网
            }

            if (wifi_manager_is_provisioning())
            {
                // 配网期间后台继续重连，但不计入失败次数、也不弹新状态
                s_state = WIFI_MGR_PROVISIONING;
                schedule_retry(APP_CFG_WIFI_RETRY_LONG_MS);
                ESP_LOGD(TAG, "配网模式下 STA 断开（reason=%d），稍后重连", ev->reason);
                break;
            }

            // 配网期间用户可能主动让 STA 断开再重连（手机连热点会让 STA 短暂离开原 AP，
            // 原因码 2/8）。这时的断开不算“连接失败”，否则会把 s_retry 顶到上限，
            // 用户手动开热点后一秒就被自动跳出配网模式（现场表现：点“网络配置”→
            // 输密码→热点刚出现就消失、界面仍显示“已连接”）。
            if (ev->reason == WIFI_REASON_AUTH_LEAVE ||
                ev->reason == WIFI_REASON_ASSOC_LEAVE)
            {
                ESP_LOGD(TAG, "STA 主动断开（reason=%d），不计入失败次数", ev->reason);
                s_state = WIFI_MGR_CONNECTING;
                break;
            }

            s_retry++;
            ESP_LOGW(TAG, "WiFi 断开（reason=%d），第 %d/%d 次重连", ev->reason, s_retry,
                     APP_CFG_WIFI_RETRY_MAX);

            if (s_retry >= APP_CFG_WIFI_RETRY_MAX)
            {
                // 连续失败：自动开配网热点，避免设备"失联"
                ESP_LOGW(TAG, "WiFi 连续 %d 次连接失败，自动开配网热点", s_retry);
                wifi_manager_start_provisioning();
                break;
            }

            s_state = WIFI_MGR_CONNECTING;
            schedule_retry(APP_CFG_WIFI_RETRY_DELAY_MS);
            break;
        }

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "配网热点已启动：%s（打开浏览器访问 192.168.4.1）", s_ap_ssid);
            break;

        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "配网热点已关闭");
            break;

        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_retry = 0;
        s_state = WIFI_MGR_CONNECTED;
        xEventGroupSetBits(g_sys_events, SYS_EVENT_WIFI_CONNECTED);
        ESP_LOGI(TAG, "WiFi 已连接：" IPSTR "（%s）", IP2STR(&ev->ip_info.ip), s_ssid);
    }
}

/**
 * @brief 生成配网热点名：DoorCam-XXXX（MAC 后两字节）
 */
static void build_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", APP_CFG_NAME, mac[4], mac[5]);
}

/**
 * @brief 写入配网热点配置（幂等）
 * @return ESP_OK 表示配置已生效
 * @note  **必须在 esp_wifi_start() 之前调用**（未配网路径）。AP 已启动时再调
 *        esp_wifi_set_config(WIFI_IF_AP, ...)，驱动会先停 AP 再重开以应用新配置，
 *        日志里出现“热点已关闭/已启动”的抖动，并且会重新分配 beacon 缓冲
 *        （内部 RAM 只剩 3.5 KB 时这一步最危险）。所以启动前就写好，之后不再改。
 */
static esp_err_t apply_ap_config(void)
{
    if (s_ap_cfg_applied)
    {
        return ESP_OK;
    }

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel = APP_CFG_PROV_AP_CHANNEL;
    ap_cfg.ap.max_connection = APP_CFG_PROV_AP_MAX_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN; // 临时热点：不设密码，门户只写网络参数

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err == ESP_OK)
    {
        s_ap_cfg_applied = true;
    }
    return err;
}

esp_err_t wifi_manager_init(void)
{
    if (s_wifi_started)
    {
        return ESP_OK;
    }

    build_ap_ssid();
    // 先打一行：内部 RAM 不足时可能卡在某个初始化步骤，日志能定位停在哪一步
    ESP_LOGI(TAG, "WiFi 初始化开始（热点名 %s）", s_ap_ssid);

    // ---- 网络栈 ----
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_netif_init 失败：%s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "创建默认事件循环失败：%s", esp_err_to_name(err));
        return err;
    }

    if (!s_netif_ready)
    {
        if (esp_netif_create_default_wifi_sta() == NULL)
        {
            ESP_LOGE(TAG, "创建 STA netif 失败");
            return ESP_FAIL;
        }
        // AP netif 一次创建好：配网时再创建会返回 NULL（同名 netif 已存在）
        if (esp_netif_create_default_wifi_ap() == NULL)
        {
            ESP_LOGW(TAG, "创建 AP netif 失败（配网可能不可用）");
        }
        s_netif_ready = true;
    }

    // ---- 定时器 ----
    const esp_timer_create_args_t retry_args = {
        .callback = retry_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
    };
    if (s_retry_timer == NULL && esp_timer_create(&retry_args, &s_retry_timer) != ESP_OK)
    {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t reboot_args = {
        .callback = reboot_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reboot",
    };
    if (s_reboot_timer == NULL && esp_timer_create(&reboot_args, &s_reboot_timer) != ESP_OK)
    {
        return ESP_ERR_NO_MEM;
    }

    // ---- WiFi 驱动 ----
    // 只用本工程真正需要的缓冲（参数见 sdkconfig.defaults 的“WiFi 裁剪”一节）：
    //   静态 RX 4 项（固定占内部 RAM，DMA 访问）、RX MGMT 动态 3 项、
    //   静态 TX 4 项 + 缓存 TX 8 项（经 prefer-SPIRAM 优先落 PSRAM）、管理短缓冲 6 项。
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    // 打印每块 buff 的分配结果与内部 RAM 剩余，用来定位 ESP_ERR_NO_MEM 到底卡在哪一块
    ESP_LOGI(TAG, "esp_wifi_init 参数：rx_mgmt=%d tx_type=%d static_tx=%d cache_tx=%d "
                  "static_rx=%d dynamic_rx=%d mgmt_sbuf=%d beacon=%d",
             init_cfg.rx_mgmt_buf_num, init_cfg.tx_buf_type, init_cfg.static_tx_buf_num,
             init_cfg.cache_tx_buf_num, init_cfg.static_rx_buf_num,
             init_cfg.dynamic_rx_buf_num, init_cfg.mgmt_sbuf_num, init_cfg.beacon_max_len);
    err = esp_wifi_init(&init_cfg);
    ESP_LOGI(TAG, "esp_wifi_init 后：内部空闲=%u 最大块=%u | PSRAM 空闲=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "esp_wifi_init 返回 %s", esp_err_to_name(err));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init 失败：%s", esp_err_to_name(err));
        return err;
    }
    // 凭据由 net_config 管理，驱动侧不再保存（避免两份配置不一致）。
    // **必须在 esp_wifi_start() 之后设置**：IDF 5.x 的 esp_wifi_set_storage 要求 WiFi
    // 已启动，启动前调用返回 ESP_ERR_WIFI_MODE（这里原来没检查返回值，凭据就悄悄
    // 落进了 WiFi 自己的 NVS，与 net_cfg 两份并存）。所以放在下面的 esp_wifi_start 之后。

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    // ---- 凭据 ----
    s_has_creds = (net_config_get_wifi(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass)) == ESP_OK);

    // **先设模式、再写接口配置**：esp_wifi_set_config() 要求对应接口已存在，
    // 在 set_mode() 之前调用会返回 ESP_ERR_WIFI_MODE（配网保存凭据后重启，
    // STA 配置写不进去，表现为一直连不上、MQTT 反复解析域名失败）。
    // 未配网直接以 APSTA 启动：稍后 wifi_manager_start_provisioning() 开热点时不再切模式。
    err = esp_wifi_set_mode(s_has_creds ? WIFI_MODE_STA : WIFI_MODE_APSTA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 WiFi 模式失败：%s", esp_err_to_name(err));
        return err;
    }

    if (s_has_creds)
    {
        wifi_config_t sta_cfg;
        memset(&sta_cfg, 0, sizeof(sta_cfg));
        strncpy((char *)sta_cfg.sta.ssid, s_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, s_pass, sizeof(sta_cfg.sta.password) - 1);
        // WPA2/PSK 由密码长度自动推导；threshold.authmode 留默认（开放+WEP+WPA2 均可）
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "写入 STA 配置失败：%s", esp_err_to_name(err));
        }
        s_state = WIFI_MGR_CONNECTING;
    }
    else
    {
        s_state = WIFI_MGR_NO_CONFIG;
        ESP_LOGW(TAG, "未配置 WiFi，启动配网模式（热点 %s）", s_ap_ssid);
    }
    // 未配网时把热点配置在 esp_wifi_start 之前写好（AP 启动后再写会让驱动重启热点）
    if (!s_has_creds)
    {
        err = apply_ap_config();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "写入配网热点配置失败：%s（热点可能用默认参数）", esp_err_to_name(err));
        }
    }
    // CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y：WiFi 的 wifi_malloc/wifi_calloc（静态 TX、
    // 缓存 TX、beacon eb 等）优先落 PSRAM；静态 RX 必须留在内部 RAM（DMA），不在此列。
    // 这样内部 RAM 尽量留给 face_task 的栈与 LVGL 绘制缓冲。
    ESP_LOGI(TAG, "esp_wifi_start 前：内部空闲=%u 最大块=%u | PSRAM 空闲=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start 失败：%s", esp_err_to_name(err));
        return err;
    }
    s_wifi_started = true;

    // 见上面说明：存储策略只能在 WiFi 启动后设置
    esp_err_t serr = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (serr != ESP_OK)
    {
        ESP_LOGW(TAG, "设置 WiFi 存储为 RAM 失败：%s", esp_err_to_name(serr));
    }

    /* 关闭 WiFi 省电（modem sleep）——这是**启动崩溃**的根治点。
     * 默认 WIFI_PS_MIN_MODEM 下，WiFi 每几个 beacon 就 modem sleep/wake 一次，
     * 每次唤醒走 pm_dream → esp_phy_enable → phy_track_pll_init，
     * 而后者用 esp_timer_create 从**内部 RAM** 申请定时器。
     * voice_assistant_init() 的 AFE/MultiNet 创建（约 -28 KB 内部 RAM）与这个
     * 定时器创建并发，实测约一半的上电会在这里 abort：
     *   ESP_ERR_NO_MEM at phy_common.c line 118 (phy_track_pll_init)
     *   backtrace: pm_dream -> pm_tbtt_process -> ppTask
     * 结果是反复重启（串口里每 5 s 一次 ESP-ROM）。
     * 本机是市电供电的门禁，功耗不是约束；关闭省电后 phy 常开、定时器只创建一次，
     * 同时 MQTT 下行延迟也变稳定。 */
    serr = esp_wifi_set_ps(WIFI_PS_NONE);
    if (serr != ESP_OK)
    {
        ESP_LOGW(TAG, "关闭 WiFi 省电失败：%s", esp_err_to_name(serr));
    }

    if (!s_has_creds)
    {
        wifi_manager_start_provisioning();
    }
    return ESP_OK;
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    return s_state;
}

bool wifi_manager_is_connected(void)
{
    return (xEventGroupGetBits(g_sys_events) & SYS_EVENT_WIFI_CONNECTED) != 0;
}

bool wifi_manager_is_provisioning(void)
{
    return s_state == WIFI_MGR_PROVISIONING;
}

void wifi_manager_set_connect_paused(bool paused)
{
    s_connect_paused = paused;
    if (paused)
    {
        ESP_LOGI(TAG, "已暂停 WiFi 主动连接（等待语音引擎初始化完成）");
        return;
    }
    if (!s_wifi_started || !s_has_creds || wifi_manager_is_provisioning() ||
        wifi_manager_is_connected())
    {
        return;
    }
    ESP_LOGI(TAG, "恢复 WiFi 连接：%s", s_ssid);
    s_state = WIFI_MGR_CONNECTING;
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_connect 返回 %s", esp_err_to_name(err));
    }
}

esp_err_t wifi_manager_start_provisioning(void)
{
    if (!s_wifi_started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == WIFI_MGR_PROVISIONING)
    {
        return ESP_OK; // 幂等
    }

    esp_err_t err;

    // **顺序必须是“先设模式、再写接口配置”**（与 STA 配置同一个约束）。
    // 已联上路由时当前模式是 STA，此时直接 esp_wifi_set_config(WIFI_IF_AP, ...) 会返回
    // ESP_ERR_WIFI_MODE，配网流程在这里失败，现场表现为：设置页输完密码后热点根本不
    // 出现、界面仍显示“已连接”。
    // 未配网启动时 init 已经是 APSTA 且配置已写好（s_ap_cfg_applied），这里只补一次
    // 幂等调用，不会重启热点；已联网时切到 APSTA 后写配置会让驱动重启一次热点
    //（只在进入配网时发生一次，日志里可见“配网热点已关闭/已启动”）。
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_APSTA)
    {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "切换 APSTA 失败：%s", esp_err_to_name(err));
            return err;
        }
    }
    err = apply_ap_config();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "启动配网热点失败：%s", esp_err_to_name(err));
        return err;
    }

    s_state = WIFI_MGR_PROVISIONING;
    s_retry = 0; // 进入配网即清零失败计数：否则后台重连再断一次就再次触发自动配网（热点反复重启）
    xEventGroupSetBits(g_sys_events, SYS_EVENT_PROVISIONING);

    // 先把云端（MQTT 任务 4 KB + 收发缓冲）让出来，再启动 HTTP 门户：
    // 否则 httpd_start 分不到任务栈（ESP_ERR_HTTPD_TASK），手机连上热点也打不开页面。
    cloud_service_pause(true);

    // HTTP 门户失败只影响配网，不改变 WiFi 状态
    if (prov_http_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "配网门户启动失败（热点仍可用）");
    }

    // 已配置过凭据时，后台继续尝试连接原 AP（用户可能只是想看看热点）
    if (s_has_creds)
    {
        schedule_retry(APP_CFG_WIFI_RETRY_LONG_MS);
    }

    ESP_LOGW(TAG, "已进入配网模式：热点 %s，访问 http://192.168.4.1", s_ap_ssid);
    return ESP_OK;
}

void wifi_manager_get_ap_ssid(char *buf, size_t size)
{
    if (buf == NULL || size == 0)
    {
        return;
    }
    snprintf(buf, size, "%s", s_ap_ssid);
}

int8_t wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        return ap.rssi;
    }
    return 0;
}

void wifi_manager_get_ssid(char *buf, size_t size)
{
    if (buf == NULL || size == 0)
    {
        return;
    }
    snprintf(buf, size, "%s", s_ssid);
}

esp_err_t wifi_manager_apply_credentials(const char *ssid, const char *pass)
{
    esp_err_t err = net_config_set_wifi(ssid, pass);
    if (err != ESP_OK)
    {
        return err;
    }
    if (s_reboot_timer != NULL)
    {
        esp_timer_start_once(s_reboot_timer, 2 * 1000 * 1000); // 2 s 后重启
    }
    return ESP_OK;
}
