/**
 * @file    storage.h
 * @brief   SPIFFS 文件系统挂载（M3）：门禁日志与陌生人抓拍的落盘基础。
 *
 * @note    - `storage` 分区是 esp-dl 的裸人脸库分区，**不能**挂文件系统；
 *            日志/抓拍使用独立的 `spiffs` 分区（2 MB），挂载点 `/spiffs`。
 *          - 挂载失败不阻塞启动：`storage_is_ready()` 返回 false，业务模块跳过
 *            文件写入但仍可正常识别/开门。
 *          - 挂载成功后置位 SYS_EVENT_STORAGE_READY，供后续网络模块判断。
 */

#ifndef __STORAGE_H__
#define __STORAGE_H__

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂载 SPIFFS 并创建日志/抓拍目录
 * @return ESP_OK 成功；其他值表示挂载失败（已记录告警，业务可继续）
 */
esp_err_t storage_init(void);

/** @brief 文件系统是否可用（初始化成功且未卸载） */
bool storage_is_ready(void);

/**
 * @brief 读取 SPIFFS 容量信息
 * @param total 总字节数（可为 NULL）
 * @param used  已用字节数（可为 NULL）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未挂载
 */
esp_err_t storage_usage(size_t *total, size_t *used);

#ifdef __cplusplus
}
#endif

#endif /* __STORAGE_H__ */
