/**
 * @file    app_events.h
 * @brief   全局事件组与互斥量：解耦视觉、UI、网络、音频等任务之间的状态通知。
 *
 * @note    - 事件组用于“状态位”广播（WiFi/MQTT/存储/开门等），不携带数据；
 *            需要携带数据时使用各自的任务队列。
 *          - 互斥量用于跨任务共享资源的串行访问：
 *            g_log_mutex 保护日志文件写入；g_face_db_mutex 保护人脸库读/写；
 *            g_audio_mutex 保护 I2S 播放归属（M5 起使用）。
 *          - 所有句柄由 app_events_init() 在 app_main 中创建且只创建一次。
 */

#ifndef __APP_EVENTS_H__
#define __APP_EVENTS_H__

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 系统事件位 ==================== */
#define SYS_EVENT_WIFI_CONNECTED    BIT0   /* WiFi 已连接并获取 IP */
#define SYS_EVENT_TIME_VALID        BIT1   /* SNTP 对时完成，系统时间可信 */
#define SYS_EVENT_MQTT_CONNECTED    BIT2   /* 巴法云 MQTT 已连接 */
#define SYS_EVENT_FACE_DB_READY     BIT3   /* 人脸库已从 storage 分区加载 */
#define SYS_EVENT_AUDIO_BUSY        BIT4   /* 音频正在播放（语音识别需暂停） */
#define SYS_EVENT_UNLOCK_ACTIVE     BIT5   /* 模拟开门进行中 */
#define SYS_EVENT_PROVISIONING      BIT6   /* 处于 SoftAP 配网模式 */
#define SYS_EVENT_STORAGE_READY     BIT7   /* SPIFFS 已挂载（日志/抓拍可用） */

/* ==================== 全局句柄 ==================== */
extern EventGroupHandle_t g_sys_events;      /* 系统状态事件组 */
extern SemaphoreHandle_t g_log_mutex;        /* 日志文件写入互斥量 */
extern SemaphoreHandle_t g_face_db_mutex;    /* 人脸库读/写互斥量 */
extern SemaphoreHandle_t g_audio_mutex;      /* 音频播放互斥量 */

/**
 * @brief 创建全局事件组与互斥量（app_main 中调用一次）
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 内存不足；重复调用返回 ESP_OK
 */
esp_err_t app_events_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_EVENTS_H__ */
