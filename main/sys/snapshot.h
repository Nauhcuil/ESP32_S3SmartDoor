/**
 * @file    snapshot.h
 * @brief   陌生人抓拍（M3）：JPEG 落盘到 `/spiffs/snap/`，按数量环形保留。
 *
 * @note    - 编码（frame2jpg）必须由持帧的 face_task 完成（帧归还后不能再读）；
 *            本模块只负责写文件与清理，写入在独立任务中进行，不阻塞识别流水线。
 *          - 文件名由调用方生成（`snapshot_make_name()`），形如
 *            `20261103_153012_P3.jpg`（时间未同步时为 `up0000123456_P3.jpg`），
 *            同一文件名会同时写进门禁日志，便于云端/UI 关联。
 *          - 队列深度 2：写入慢时丢弃新抓拍（告警），不阻塞 face_task。
 */

#ifndef __SNAPSHOT_H__
#define __SNAPSHOT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化抓拍模块（创建队列与写盘任务）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 存储未就绪
 */
esp_err_t snapshot_init(void);

/**
 * @brief 生成抓拍文件名（不含目录）
 * @param out 输出缓冲（建议 ≥32 字节）
 * @param cap 缓冲大小
 * @param tag 标签（如 "S" 陌生人 / 姓名首字母），可为 NULL
 * @return ESP_OK 成功
 * @note  同一秒内多次调用会得到相同文件名，调用方应按节流策略避免。
 */
esp_err_t snapshot_make_name(char *out, size_t cap, const char *tag);

/**
 * @brief 提交一张已编码的 JPEG 给写盘任务（成功后缓冲所有权转移）
 * @param jpg   由 frame2jpg() 分配的缓冲（本函数与任务负责 free）
 * @param len   JPEG 长度
 * @param name  文件名（不含目录），见 snapshot_make_name()
 * @return ESP_OK 已入队；ESP_ERR_NO_MEM 队列满（已内部释放 jpg）；
 *         ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t snapshot_submit(uint8_t *jpg, size_t len, const char *name);

/** @brief 模块是否已启动（未启动时 face_task 跳过 JPEG 编码） */
bool snapshot_is_ready(void);

/** @brief 已保存的抓拍张数（粗略值，写入完成后更新） */
size_t snapshot_count(void);

/** @brief 删除全部抓拍文件 */
esp_err_t snapshot_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* __SNAPSHOT_H__ */
