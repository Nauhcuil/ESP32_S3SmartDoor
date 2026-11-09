/**
 * @file    app_nvs.h
 * @brief   NVS 业务参数读写：识别阈值、活体开关、管理员 PIN。
 *
 * @note    - 全部在 "doorcam" 命名空间（APP_CFG_NVS_NAMESPACE），键名见 app_config.h。
 *            - 读失败（键不存在/值越界）时返回出厂默认值，不在启动阶段擦除 NVS。
 *            - 本模块不负责 WiFi/云端参数，那些在 M4 的网络模块中处理。
 */

#ifndef __APP_NVS_H__
#define __APP_NVS_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 读取识别阈值
 * @return 阈值；无有效值时返回 APP_CFG_FACE_THRESHOLD
 */
float app_nvs_get_threshold(void);

/**
 * @brief 写入识别阈值
 * @param t 阈值 [-1, 1]，越界返回 ESP_ERR_INVALID_ARG
 */
esp_err_t app_nvs_set_threshold(float t);

/**
 * @brief 读取活体开关
 * @return true 开启；无有效值时按 APP_CFG_LIVENESS_ENABLE 返回
 */
bool app_nvs_get_liveness(void);

/**
 * @brief 写入活体开关
 */
esp_err_t app_nvs_set_liveness(bool enable);

/**
 * @brief 读取人脸识别模型版本标记（M6：S16 → S8 迁移用）
 * @return 上次写入的版本号；从未写入返回 0
 *
 * @note  esp-dl 的人脸库在 storage 分区里带模型校验码，换模型（S16 ↔ S8）后
 *        `set_ids_from_flash()` 读不出任何记录。用这个键记录“库里数据属于哪个模型”，
 *        启动时不匹配就清库并提示重新录入，避免出现“重启后库里空着但分区有数据”的困惑。
 */
uint8_t app_nvs_get_model_ver(void);

/**
 * @brief 写入人脸识别模型版本标记
 */
esp_err_t app_nvs_set_model_ver(uint8_t ver);

/**
 * @brief 读取“识别阈值默认值修订号”（见 APP_CFG_THRESHOLD_REV）
 * @return 上次写入的修订号；从未写入返回 0
 */
uint8_t app_nvs_get_threshold_rev(void);

/**
 * @brief 写入“识别阈值默认值修订号”
 */
esp_err_t app_nvs_set_threshold_rev(uint8_t rev);

/**
 * @brief 读取管理员 PIN
 * @return PIN；无有效值时返回 APP_CFG_ADMIN_PIN_DEFAULT
 */
uint32_t app_nvs_get_admin_pin(void);

/**
 * @brief 写入管理员 PIN
 */
esp_err_t app_nvs_set_admin_pin(uint32_t pin);

/**
 * @brief 读取陌生人抓拍开关（M3）
 * @return true 开启；无有效值时默认开启
 */
bool app_nvs_get_snapshot_enabled(void);

/**
 * @brief 写入陌生人抓拍开关
 */
esp_err_t app_nvs_set_snapshot_enabled(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* __APP_NVS_H__ */
