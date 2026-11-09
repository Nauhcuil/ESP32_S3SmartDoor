/**
 * @file    face_quality.c
 * @brief   人脸质量评估实现（尺寸 / 亮度 / Laplacian 清晰度 / 偏航 / 分区纹理）。
 *
 * @note    1. 全部基于采样网格计算，单帧耗时 < 1 ms；不做堆分配，可在 AI 任务中每帧调用。
 *          2. 网格边长上限 48：既够判断清晰度，又把栈占用控制在 2.3 KB。
 *          3. 阈值放在文件顶部，现场调试只需改这里或由 face_liveness 覆盖。
 */

#include "face_quality.h"

#include <math.h>
#include <string.h>

/* ==================== 质量门限（出厂默认） ==================== */
#define QUAL_MIN_SIDE_PX        64    /**< 人脸框短边最小像素 */
#define QUAL_BRIGHT_MIN         40.0f /**< 平均亮度下限 */
#define QUAL_BRIGHT_MAX         220.0f/**< 平均亮度上限 */
#define QUAL_SHARPNESS_MIN      90.0f /**< Laplacian 方差下限 */
#define QUAL_YAW_MAX            0.25f /**< 偏航归一化位移上限 */
#define QUAL_GRID_MAX           48    /**< 采样网格每边最大点数 */

/**
 * @brief RGB565 转 8 位灰度
 * @note  用整数近似 BT.601：Y = 0.299R + 0.587G + 0.114B。
 *        5/6/5 位分量先扩展到 8 位，避免低亮度段被截断。
 *        **字节序**：摄像头（GC0308 DVP）输出的是“高字节在前”的 RGB565 字节流，
 *        低地址字节 = RRRRRGGG，高地址字节 = GGGBBBBB。XTensa 是小端，按 uint16_t
 *        读出后 bit15..8 = GGGBBBBB、bit7..0 = RRRRRGGG。因此不能按 bit15..11 = R
 *        解码（那是“低字节在前”的位域），否则 R/G/B 会互相串位，亮度、清晰度、
 *        纹理全部失真。ESP-DL 的 uint16 输入路径同样按这个布局解释（已用自检验证）。
 */
static inline uint8_t rgb565_to_luma(uint16_t v)
{
    const uint32_t hi = (v >> 8) & 0xFF; // GGGBBBBB
    const uint32_t lo = v & 0xFF;        // RRRRRGGG
    const uint32_t r = lo >> 3;
    const uint32_t g = ((hi >> 3) << 3) | (lo & 0x07);
    const uint32_t b = hi & 0x1F;
    const uint32_t r8 = (r << 3) | (r >> 2);
    const uint32_t g8 = (g << 2) | (g >> 4);
    const uint32_t b8 = (b << 3) | (b >> 2);
    return (uint8_t)((77 * r8 + 150 * g8 + 29 * b8) >> 8);
}

float face_frame_brightness(const uint16_t *rgb565, int32_t frame_w, int32_t frame_h)
{
    if (rgb565 == NULL || frame_w <= 0 || frame_h <= 0)
    {
        return -1.0f;
    }

    // 整帧最多取 64x64 个采样点
    const int32_t step_x = frame_w > 64 ? frame_w / 64 : 1;
    const int32_t step_y = frame_h > 64 ? frame_h / 64 : 1;
    uint32_t sum = 0;
    uint32_t count = 0;

    for (int32_t y = 0; y < frame_h; y += step_y)
    {
        const uint16_t *row = rgb565 + (size_t)y * (size_t)frame_w;
        for (int32_t x = 0; x < frame_w; x += step_x)
        {
            sum += rgb565_to_luma(row[x]);
            count++;
        }
    }
    return count ? (float)sum / (float)count : -1.0f;
}

bool face_quality_eval(const uint16_t *rgb565, int32_t frame_w, int32_t frame_h, int32_t x,
                       int32_t y, int32_t w, int32_t h, const int32_t *landmarks,
                       face_quality_t *out)
{
    if (rgb565 == NULL || out == NULL || frame_w <= 0 || frame_h <= 0 || w <= 0 || h <= 0)
    {
        return false;
    }

    // ---- 1. 裁剪到帧内 ----
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;
    if (x1 > frame_w)
    {
        x1 = frame_w;
    }
    if (y1 > frame_h)
    {
        y1 = frame_h;
    }
    if (x1 - x0 < 8 || y1 - y0 < 8)
    {
        memset(out, 0, sizeof(*out));
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;

    // ---- 2. 采样为灰度网格 ----
    int32_t gw = out->w < QUAL_GRID_MAX ? out->w : QUAL_GRID_MAX;
    int32_t gh = out->h < QUAL_GRID_MAX ? out->h : QUAL_GRID_MAX;
    const int32_t step_x = out->w / gw;
    const int32_t step_y = out->h / gh;
    if (step_x <= 0 || step_y <= 0)
    {
        return false;
    }

    uint8_t lum[QUAL_GRID_MAX * QUAL_GRID_MAX];
    uint32_t sum = 0;
    for (int32_t j = 0; j < gh; j++)
    {
        const uint16_t *row =
            rgb565 + (size_t)(y0 + j * step_y) * (size_t)frame_w + (size_t)x0;
        uint8_t *dst = lum + (size_t)j * (size_t)gw;
        for (int32_t i = 0; i < gw; i++)
        {
            const uint8_t v = rgb565_to_luma(row[i * step_x]);
            dst[i] = v;
            sum += v;
        }
    }
    out->brightness = (float)sum / (float)(gw * gh);

    // ---- 3. Laplacian 方差（4 邻域二阶差分） ----
    double lap_sum = 0.0;
    double lap_sq_sum = 0.0;
    uint32_t lap_n = 0;
    for (int32_t j = 1; j < gh - 1; j++)
    {
        const uint8_t *prev = lum + (size_t)(j - 1) * (size_t)gw;
        const uint8_t *cur = lum + (size_t)j * (size_t)gw;
        const uint8_t *next = lum + (size_t)(j + 1) * (size_t)gw;
        for (int32_t i = 1; i < gw - 1; i++)
        {
            const int32_t lap = 4 * (int32_t)cur[i] - (int32_t)prev[i] - (int32_t)next[i] -
                                (int32_t)cur[i - 1] - (int32_t)cur[i + 1];
            lap_sum += lap;
            lap_sq_sum += (double)lap * (double)lap;
            lap_n++;
        }
    }
    if (lap_n > 0)
    {
        const double mean = lap_sum / (double)lap_n;
        const double var = lap_sq_sum / (double)lap_n - mean * mean;
        out->sharpness = (float)(var > 0.0 ? var : 0.0);
    }

    // ---- 4. 偏航估算：鼻尖相对双眼中点的水平偏移 / 眼距 ----
    // keypoint 排列：[0,1]左眼 [2,3]左嘴角 [4,5]鼻尖 [6,7]右眼 [8,9]右嘴角
    float yaw = 0.0f;
    if (landmarks != NULL)
    {
        const float eye_dx = (float)(landmarks[6] - landmarks[0]);
        const float eye_dist = fabsf(eye_dx);
        if (eye_dist >= 8.0f)
        {
            const float eye_mid = ((float)landmarks[0] + (float)landmarks[6]) * 0.5f;
            yaw = fabsf(((float)landmarks[4] - eye_mid) / eye_dist);
        }
    }
    out->yaw = yaw;

    // ---- 5. 判定 ----
    const int32_t min_side = out->w < out->h ? out->w : out->h;
    out->size_ok = min_side >= QUAL_MIN_SIDE_PX;
    out->brightness_ok = out->brightness >= QUAL_BRIGHT_MIN && out->brightness <= QUAL_BRIGHT_MAX;
    out->sharpness_ok = out->sharpness >= QUAL_SHARPNESS_MIN;
    out->frontal_ok = yaw <= QUAL_YAW_MAX;
    out->pass = out->size_ok && out->brightness_ok && out->sharpness_ok && out->frontal_ok;
    return true;
}

int face_texture_block_ratio(const uint16_t *rgb565, int32_t frame_w, int32_t frame_h, int32_t x,
                             int32_t y, int32_t w, int32_t h, int grid, float *ratio)
{
    if (rgb565 == NULL || ratio == NULL || grid < 2 || grid > 4 || frame_w <= 0 || frame_h <= 0)
    {
        return 0;
    }

    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;
    if (x1 > frame_w)
    {
        x1 = frame_w;
    }
    if (y1 > frame_h)
    {
        y1 = frame_h;
    }
    const int32_t rw = x1 - x0;
    const int32_t rh = y1 - y0;
    if (rw < grid * 8 || rh < grid * 8)
    {
        return 0;
    }

    const int32_t bw = rw / grid;
    const int32_t bh = rh / grid;
    float min_e = 1e9f;
    float max_e = 0.0f;
    int valid = 0;

    for (int32_t by = 0; by < grid; by++)
    {
        for (int32_t bx = 0; bx < grid; bx++)
        {
            // 每块内按 2 像素步长统计梯度幅值
            double e = 0.0;
            uint32_t n = 0;
            const int32_t sx0 = x0 + bx * bw;
            const int32_t sy0 = y0 + by * bh;
            for (int32_t j = 1; j < bh - 1; j += 2)
            {
                const uint16_t *row = rgb565 + (size_t)(sy0 + j) * (size_t)frame_w;
                const uint16_t *row_up = rgb565 + (size_t)(sy0 + j - 1) * (size_t)frame_w;
                const uint16_t *row_dn = rgb565 + (size_t)(sy0 + j + 1) * (size_t)frame_w;
                for (int32_t i = 1; i < bw - 1; i += 2)
                {
                    const int32_t px = sx0 + i;
                    const int32_t gx = (int32_t)rgb565_to_luma(row[px + 1]) -
                                       (int32_t)rgb565_to_luma(row[px - 1]);
                    const int32_t gy = (int32_t)rgb565_to_luma(row_dn[px]) -
                                       (int32_t)rgb565_to_luma(row_up[px]);
                    e += (double)(gx * gx + gy * gy);
                    n++;
                }
            }
            if (n == 0)
            {
                continue;
            }
            const float avg = (float)(e / (double)n);
            // 排除完全平坦（过曝/纯色）分区，避免除零与误判
            if (avg < 1.0f)
            {
                continue;
            }
            valid++;
            if (avg < min_e)
            {
                min_e = avg;
            }
            if (avg > max_e)
            {
                max_e = avg;
            }
        }
    }

    if (valid < grid * grid)
    {
        return valid; // 有效分区不足，调用方应视为“无法评估”
    }
    *ratio = (min_e > 0.0f) ? max_e / min_e : 0.0f;
    return valid;
}
