/**
 * @file    app_events.c
 * @brief   全局事件组与互斥量的创建实现。
 *
 * @note    FreeRTOS 的事件组/互斥量句柄在首次创建后保持有效，任务退出不会释放；
 *          因此只允许 app_main 调用一次。若创建过程中失败，会回收已创建的句柄，
 *          避免留下半初始化状态。
 */

#include "app_events.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "app_events";

EventGroupHandle_t g_sys_events = NULL;
SemaphoreHandle_t g_log_mutex = NULL;
SemaphoreHandle_t g_face_db_mutex = NULL;
SemaphoreHandle_t g_audio_mutex = NULL;

/**
 * @brief 创建全局事件组与互斥量（幂等）
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 任一句柄创建失败
 */
esp_err_t app_events_init(void)
{
    if (g_sys_events != NULL)
    {
        return ESP_OK; // 已初始化，直接返回成功
    }

    g_sys_events = xEventGroupCreate();
    g_log_mutex = xSemaphoreCreateMutex();
    g_face_db_mutex = xSemaphoreCreateMutex();
    g_audio_mutex = xSemaphoreCreateMutex();

    if (g_sys_events == NULL || g_log_mutex == NULL || g_face_db_mutex == NULL ||
        g_audio_mutex == NULL)
    {
        ESP_LOGE(TAG, "创建事件组/互斥量失败（内存不足）");

        // 回收已经创建成功的句柄，保证不会留下部分初始化的全局状态
        if (g_audio_mutex != NULL)
        {
            vSemaphoreDelete(g_audio_mutex);
            g_audio_mutex = NULL;
        }
        if (g_face_db_mutex != NULL)
        {
            vSemaphoreDelete(g_face_db_mutex);
            g_face_db_mutex = NULL;
        }
        if (g_log_mutex != NULL)
        {
            vSemaphoreDelete(g_log_mutex);
            g_log_mutex = NULL;
        }
        if (g_sys_events != NULL)
        {
            vEventGroupDelete(g_sys_events);
            g_sys_events = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "事件组与互斥量初始化完成");
    return ESP_OK;
}
