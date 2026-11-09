/**
 * @file    app_config.h
 * @brief   应用级编译期默认配置（阈值、门禁模拟、监控周期、NVS 键名等）。
 *
 * @note    - 这里的值只是“出厂默认值”，运行时可由 NVS 覆盖（例如识别阈值、活体开关）。
 *          - 与硬件相关的可调项集中在此，避免散落在业务代码中。
 *          - 本项目不做真实继电器/电锁：开门以屏幕覆盖层 + 语音播报模拟，
 *            DOOR_RELAY_GPIO 保留为 -1，后续接继电器只需改此宏。
 */

#ifndef __APP_CONFIG_H__
#define __APP_CONFIG_H__

/* ==================== 应用标识 ==================== */
#define APP_CFG_NAME                 "DoorCam"     /* 设备名（配网热点前缀） */
#define APP_CFG_VERSION              "0.1.0"       /* 固件版本（M0 基线） */

/* ==================== 人脸识别默认参数 ==================== */
/*
 * 默认识别阈值
 * - S8（M6 起，8-bit 量化）：esp-dl 官方建议 S8 模型用 0.62~0.68，取中间 0.65。
 *   实测本板 S8 同一人相似度约 0.7~0.9、不同人 <0.4，0.65 既能防误认又不会把本人拒之门外。
 * - S16（旧）：0.55（与 esp-dl 默认一致）。
 * 改这里（或换模型）时必须把 APP_CFG_THRESHOLD_REV +1，否则已烧录的板子会继续用
 * NVS 里的旧阈值（face_service_init 只按修订号判断要不要重置）。
 */
#define APP_CFG_FACE_THRESHOLD       0.65f
#define APP_CFG_LIVENESS_ENABLE      1             /* 默认开启活体校验（M2 起生效） */
#define APP_CFG_ENROLL_TIMEOUT_MS    10000         /* 单次注册超时 */

/* ==================== 门禁模拟 ==================== */
#define APP_CFG_DOOR_RELAY_GPIO      (-1)          /* 预留：继电器 GPIO，-1 表示未接 */
#define APP_CFG_DOOR_OPEN_SIM_MS     3000          /* 模拟开门覆盖层显示时长 */

/* ==================== 系统监控 ==================== */
#define APP_CFG_SYS_MONITOR_PERIOD_MS 5000         /* fps / 耗时 / 内存 / 栈水位打印周期 */

/* ==================== NVS 配置 ==================== */
#define APP_CFG_NVS_NAMESPACE        "doorcam"     /* 业务参数命名空间 */
#define APP_CFG_NVS_KEY_THRESHOLD    "face_thr"    /* 识别阈值（float） */
#define APP_CFG_NVS_KEY_LIVENESS     "liveness"    /* 活体开关（u8） */
#define APP_CFG_NVS_KEY_ADMIN_PIN    "admin_pin"   /* 管理员 PIN（u32，默认 1234） */
#define APP_CFG_ADMIN_PIN_DEFAULT    1234          /* 出厂管理员 PIN */
#define APP_CFG_NVS_KEY_SNAPSHOT     "snap_on"     /* 陌生人抓拍开关（u8，默认开） */
#define APP_CFG_NVS_KEY_MODEL_VER    "model_ver"   /* 人脸识别模型版本（u8：8 = S8，16 = S16） */
#define APP_CFG_NVS_KEY_THR_REV      "thr_rev"     /* 阈值默认值修订号（u8，见 APP_CFG_THRESHOLD_REV） */
/**
 * @brief 识别阈值默认值的“修订号”
 * @note  NVS 里存的是用户改过的阈值，换模型/换默认值后旧值会一直留着（现场表现：
 *        已切到 S8 模型，阈值却还是 S16 时代的 0.55）。改动 APP_CFG_FACE_THRESHOLD
 *        或更换识别模型时，把这里 +1，face_service_init() 会把 NVS 阈值重置为新默认值
 *        并记下新修订号；用户之后在设置页的微调不受影响。
 */
#define APP_CFG_THRESHOLD_REV        1

/* ==================== 网络（M4） ==================== */
/* WiFi 与巴法云参数全部存 NVS（命名空间 net_cfg），**不得硬编码到源码**；
 * 出厂状态下两项都为空：设备启动即进入 SoftAP 配网，由手机浏览器填写。 */
#define APP_CFG_NVS_NET_NAMESPACE    "net_cfg"      /* 网络参数命名空间（与 NVS 键名表一致） */
#define APP_CFG_NVS_KEY_WIFI_SSID    "wifi_ssid"    /* WiFi 名称 */
#define APP_CFG_NVS_KEY_WIFI_PASS    "wifi_pass"    /* WiFi 密码 */
#define APP_CFG_NVS_KEY_BEMFA_UID    "bemfa_uid"    /* 巴法云私钥 */
#define APP_CFG_NVS_KEY_BEMFA_TOPIC  "bemfa_topic"  /* 巴法云主题名 */
#define APP_CFG_NVS_KEY_BEMFA_PORT   "bemfa_port"   /* 巴法云端口（u16，9501 明文 / 9503 TLS） */

#define APP_CFG_WIFI_RETRY_MAX       3              /* 连续失败次数达到此值后自动进入配网 */
#define APP_CFG_WIFI_RETRY_DELAY_MS  8000           /* 两次重连的间隔（路由器重启一般 20~30 s 恢复） */
#define APP_CFG_WIFI_RETRY_LONG_MS   15000          /* 配网模式下的后台重连间隔（不打断配网页） */
#define APP_CFG_PROV_AP_CHANNEL      1              /* 配网热点信道 */
#define APP_CFG_PROV_AP_MAX_CONN     4              /* 配网热点最大客户端数 */

#define APP_CFG_BEMFA_HOST           "bemfa.com"    /* 巴法云 MQTT 域名（公网固定） */
#define APP_CFG_BEMFA_PORT_PLAIN     9501           /* 巴法云明文 MQTT 端口 */
#define APP_CFG_BEMFA_PORT_TLS       9503           /* 巴法云 TLS MQTT 端口 */
/* 配网表单的预填主题（仅作默认建议，实际值以 NVS 为准；顺序见 bemfa_topic_slot_t：
   门禁事件, 远程命令, 心跳回执, 陌生人告警） */
#define APP_CFG_BEMFA_TOPICS_DEFAULT "doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger"

/* ==================== 对时（M4） ==================== */
#define APP_CFG_SNTP_SERVER_0        "ntp.aliyun.com"   /* 首选：阿里云 NTP（国内可达） */
#define APP_CFG_SNTP_SERVER_1        "cn.pool.ntp.org"  /* 备选 1 */
#define APP_CFG_SNTP_SERVER_2        "pool.ntp.org"     /* 备选 2 */
#define APP_CFG_SNTP_MAX_SERVERS     3                  /* 与 CONFIG_LWIP_SNTP_MAX_SERVERS 一致 */
#define APP_CFG_SNTP_SYNC_INTERVAL_MS 900000            /* 对时周期 15 min（界面显示到秒，漂移要小） */

/* ==================== 存储（M3） ==================== */
#define APP_CFG_SPIFFS_BASE          "/spiffs"     /* SPIFFS 挂载点 */
#define APP_CFG_LOG_DIR              "/spiffs/log"   /* 门禁日志目录 */
#define APP_CFG_SNAP_DIR             "/spiffs/snap"  /* 陌生人抓拍目录 */
#define APP_CFG_LOG_MAX_BYTES        (256 * 1024)  /* 单个日志文件上限，超过则轮转 */
#define APP_CFG_LOG_KEEP_FILES       4             /* 轮转后保留的日志文件数（含当前） */
#define APP_CFG_SNAP_MAX_FILES       60            /* 抓拍保留张数（超出删除最旧） */

/* ==================== 陌生人判定 / 抓拍节流 ==================== */
/* 手动识别模式下一次点击只识别一帧，“连续 6 帧”会让陌生人永远确认不了；
 * 改为 1 次未匹配识别即确认，避免同一轨迹的重复冷却由 STRANGER_COOLDOWN_MS 兜底。 */
#define APP_CFG_STRANGER_CONFIRM_FRAMES 1          /* 同一轨迹连续未注册的识别帧数 → 确认陌生人 */
#define APP_CFG_STRANGER_COOLDOWN_MS    60000      /* 同一轨迹再次告警的最小间隔 */
#define APP_CFG_SNAP_GLOBAL_GAP_MS      30000      /* 全局抓拍最小间隔（1 张/30 s） */
#define APP_CFG_SNAP_HOURLY_MAX         10         /* 每小时最多抓拍张数 */

#endif /* __APP_CONFIG_H__ */
