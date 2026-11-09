/**
 * @file    face_tracker.h
 * @brief   轻量人脸轨迹跟踪（单目标）：IoU 关联 + 轨迹号 + 稳定帧计数。
 *
 * @note    - 本阶段只跟踪“当前最大人脸”一个目标：门口场景通常只有一人，
 *            多目标跟踪留给 M3 的陌生人策略扩展（接口保持简单）。
 *            - 稳定判定：同一轨迹连续 FACE_TRACK_STABLE_FRAMES 帧匹配成功；
 *            用于抑制“路人一闪而过”时的识别与抓拍。
 *            - 纯 C、无堆分配。
 */

#ifndef __FACE_TRACKER_H__
#define __FACE_TRACKER_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 跟踪器输出 */
typedef struct
{
    int track_id;      /**< 轨迹号（无人脸时为 -1） */
    bool is_new;       /**< 本帧是否新建轨迹 */
    bool stable;       /**< 轨迹是否已稳定 */
    int stable_frames; /**< 连续匹配帧数 */
    int64_t last_seen_ms; /**< 最近一次匹配时间 */
} face_track_t;

/** @brief 重置跟踪器（清空轨迹） */
void face_tracker_reset(void);

/**
 * @brief 更新跟踪器
 * @param present 本帧是否检测到人脸
 * @param x,y,w,h 人脸框
 * @param ts_ms   当前时间（毫秒）
 * @param out     输出跟踪状态（不可为 NULL）
 * @return 当前轨迹号（无人脸时 -1）
 * @note 人脸消失超过 FACE_TRACK_TIMEOUT_MS 后轨迹号失效并清空统计。
 */
int face_tracker_update(bool present, int32_t x, int32_t y, int32_t w, int32_t h, int64_t ts_ms,
                        face_track_t *out);

/**
 * @brief 当前轨迹号（无人脸时 -1）
 */
int face_tracker_current_id(void);

#ifdef __cplusplus
}
#endif

#endif /* __FACE_TRACKER_H__ */
