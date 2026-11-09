/**
 * @file    face_tracker.c
 * @brief   单目标人脸轨迹跟踪实现。
 *
 * @note    IoU 阈值 0.25：人脸快速移动时框重叠会短时变小，阈值过高会频繁新建轨迹，
 *          导致识别冷却反复触发。连续 3 帧匹配即视为稳定。
 */

#include "face_tracker.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "face_tracker";

#define FACE_TRACK_IOU_MIN        0.25f /**< 关联 IoU 下限 */
#define FACE_TRACK_STABLE_FRAMES  3     /**< 稳定所需连续帧数 */
#define FACE_TRACK_TIMEOUT_MS     500   /**< 人脸消失后轨迹保留时长 */

/** @brief 当前轨迹状态 */
static struct
{
    bool active;
    int track_id;
    int next_track_id;
    int32_t x, y, w, h;
    int stable_frames;
    int64_t last_seen_ms;
} s_track;

void face_tracker_reset(void)
{
    memset(&s_track, 0, sizeof(s_track));
    s_track.track_id = -1;
    s_track.next_track_id = 1;
}

/**
 * @brief 计算两个框的 IoU
 */
static float iou(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t bx, int32_t by,
                 int32_t bw, int32_t bh)
{
    const int32_t x1 = ax > bx ? ax : bx;
    const int32_t y1 = ay > by ? ay : by;
    const int32_t x2 = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
    const int32_t y2 = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
    if (x2 <= x1 || y2 <= y1)
    {
        return 0.0f;
    }
    const float inter = (float)(x2 - x1) * (float)(y2 - y1);
    const float uni = (float)aw * (float)ah + (float)bw * (float)bh - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

int face_tracker_update(bool present, int32_t x, int32_t y, int32_t w, int32_t h, int64_t ts_ms,
                        face_track_t *out)
{
    if (out == NULL)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->track_id = -1;
    out->last_seen_ms = ts_ms;

    if (s_track.next_track_id <= 0)
    {
        face_tracker_reset();
    }

    if (!present)
    {
        if (s_track.active && (ts_ms - s_track.last_seen_ms) > FACE_TRACK_TIMEOUT_MS)
        {
            ESP_LOGD(TAG, "轨迹 %d 超时结束", s_track.track_id);
            s_track.active = false;
            s_track.track_id = -1;
        }
        else if (s_track.active)
        {
            // 短暂丢失：保留轨迹号，但稳定计数清零，避免复用旧状态
            s_track.stable_frames = 0;
            out->track_id = s_track.track_id;
            out->last_seen_ms = s_track.last_seen_ms;
        }
        return out->track_id;
    }

    // 新人脸或旧轨迹已失效：新建轨迹
    const float overlap =
        s_track.active ? iou(s_track.x, s_track.y, s_track.w, s_track.h, x, y, w, h) : 0.0f;
    if (!s_track.active || overlap < FACE_TRACK_IOU_MIN)
    {
        s_track.active = true;
        s_track.track_id = s_track.next_track_id++;
        s_track.stable_frames = 0;
        out->is_new = true;
        ESP_LOGI(TAG, "新建轨迹 %d", s_track.track_id);
    }

    s_track.x = x;
    s_track.y = y;
    s_track.w = w;
    s_track.h = h;
    s_track.last_seen_ms = ts_ms;
    if (s_track.stable_frames < 1000)
    {
        s_track.stable_frames++;
    }

    out->track_id = s_track.track_id;
    out->stable_frames = s_track.stable_frames;
    out->stable = s_track.stable_frames >= FACE_TRACK_STABLE_FRAMES;
    return out->track_id;
}

int face_tracker_current_id(void)
{
    return s_track.active ? s_track.track_id : -1;
}
