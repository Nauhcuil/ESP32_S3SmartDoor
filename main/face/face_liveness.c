/**
 * @file    face_liveness.c
 * @brief   启发式活体检测实现：纹理均匀度 + 帧间运动 + 特征一致性。
 *
 * @note    指标含义与权重：
 *          - 纹理（35%）：真人脸的 3x3 分区梯度能量差异大；照片/屏幕回放更“平”。
 *          - 运动（45%）：对归一化到 24x24 的人脸 ROI 求相邻帧平均绝对差；
 *            静态照片接近 0，真人因表情/呼吸/微动明显大于 1。
 *          - 一致（20%）：相邻帧 112-d 特征的余弦相似度，主要作为“同一张脸”门限。
 *          综合分 ≥ 55 且运动均值 ≥ 0.6 且纹理得分 ≥ 40 才判 LIVE。
 *          阈值集中在本文件顶部；后续接入专用模型时只需注册 liveness_detector_t。
 *
 * @note    阈值是按“真人站在门口不动”这一最常见场景定的，早期版本（综合 65 / 运动
 *          1.2 / 纹理 50）在实机上会把刚注册完的真人判成“活体检测失败”——
 *          静止时 24x24 ROI 的帧间灰度差只有 0.5~1.0，且若 9 个分区里有任意一块过于
 *          平坦（脸颊、额头），纹理得分会一直停在初值 0，两项硬门限都过不去。
 *          现在：纹理只要求至少 5 个分区有效即可评估、初值取中性 50（“无法评估”
 *          不等于“过平”）；运动与综合分门限下调。识别日志会打印三项原始指标，
 *          如仍有误判，请以日志里的 纹理/运动/一致 数值为准继续调整。
 */

#include "face_liveness.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "face_liveness";

/* ==================== 判定阈值 ==================== */
#define LIVE_MIN_SAMPLES        5      /**< 至少累计样本数 */
#define LIVE_MAX_SAMPLES        8      /**< 统计窗口上限 */
#define LIVE_TOTAL_PASS         55.0f  /**< 综合分下限 */
#define LIVE_MOTION_PASS        0.6f   /**< 帧间运动均值下限（0-255 灰度差） */
#define LIVE_MOTION_FULL        2.5f   /**< 运动满分对应的灰度差 */
/**
 * @brief 纹理比区间（最大分区梯度能量 / 最小分区梯度能量）
 * @note  实机实测：真人脸（QVGA、脸高约 100 px、正常室内光）的比值只有 1.4~1.5，
 *        原 1.4~2.4 的映射会让真人拿 2 分、被纹理门限直接否决。按实测把区间下移，
 *        同时把纹理门限降到 25：纹理只作“过平”的排除项，不再一票否决。
 */
#define LIVE_TEXTURE_LOW        1.15f  /**< 纹理比低于此值视为“过平” */
#define LIVE_TEXTURE_HIGH       1.75f  /**< 纹理比高于此值视为纹理充分 */
#define LIVE_TEXTURE_GATE       25.0f  /**< 纹理得分硬门限 */
#define LIVE_TEXTURE_MIN_BLOCKS 5      /**< 至少要这么多分区有效才更新纹理得分 */
#define LIVE_TEXTURE_NEUTRAL    50.0f  /**< 纹理无法评估时的中性分（不判“过平”） */
#define LIVE_ROI_GRID           24     /**< 运动比较用的 ROI 网格边长 */
#define LIVE_EMB_MAX            512    /**< 特征向量最大长度 */

/** @brief 模块状态 */
static bool s_enabled = false;
static const liveness_detector_t *s_detector = NULL;

/** @brief 当前窗口统计 */
static int s_samples = 0;
static float s_motion_sum = 0.0f;
static int s_motion_n = 0;
static float s_texture_ratio = 1.15f;
/**
 * @brief 纹理得分：初值/重置值取中性分
 * @note  初值不能是 0——0 会被当成“纹理过平”的硬证据直接否决真人。没评估过就是
 *        “不知道”，用中性分参与加权即可。
 */
static float s_texture_score = LIVE_TEXTURE_NEUTRAL;
static float s_consistency = 1.0f;

/** @brief 上一帧 24x24 灰度 ROI 与是否有效 */
static uint8_t s_prev_lum[LIVE_ROI_GRID * LIVE_ROI_GRID];
static bool s_prev_valid = false;

/** @brief 上一帧特征（用于余弦一致度） */
static float s_prev_emb[LIVE_EMB_MAX];
static int s_prev_emb_len = 0;

/** @brief 最近一次报告 */
static liveness_report_t s_report = {0.0f, 0.0f, 0.0f, 0.0f, LIVENESS_RESULT_UNKNOWN};

/**
 * @brief 灰度裁剪到 [0,1] 区间
 */
static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/**
 * @brief 把当前人脸 ROI 重采样为 LIVE_ROI_GRID x LIVE_ROI_GRID 灰度图
 * @return true 成功；false 区域过小
 */
static bool resample_roi(const uint16_t *rgb565, int32_t frame_w, int32_t frame_h, int32_t x,
                         int32_t y, int32_t w, int32_t h, uint8_t *out)
{
    if (w < LIVE_ROI_GRID || h < LIVE_ROI_GRID)
    {
        return false;
    }

    const int32_t step_x = w / LIVE_ROI_GRID;
    const int32_t step_y = h / LIVE_ROI_GRID;
    for (int32_t j = 0; j < LIVE_ROI_GRID; j++)
    {
        const int32_t sy = y + j * step_y;
        if (sy < 0 || sy >= frame_h)
        {
            return false;
        }
        const uint16_t *row = rgb565 + (size_t)sy * (size_t)frame_w;
        uint8_t *dst = out + (size_t)j * (size_t)LIVE_ROI_GRID;
        for (int32_t i = 0; i < LIVE_ROI_GRID; i++)
        {
            const int32_t sx = x + i * step_x;
            if (sx < 0 || sx >= frame_w)
            {
                return false;
            }
            const uint16_t v = row[sx];
            // 摄像头字节流“高字节在前”：低地址字节 = RRRRRGGG，高地址字节 = GGGBBBBB，
            // 按 uint16_t 读出后 bit15..8 = GGGBBBBB、bit7..0 = RRRRRGGG。
            const uint32_t lo = v & 0xFF;
            const uint32_t hi = (v >> 8) & 0xFF;
            const uint32_t r = lo >> 3;
            const uint32_t g = ((hi >> 3) << 3) | (lo & 0x07);
            const uint32_t b = hi & 0x1F;
            dst[i] = (uint8_t)(((77 * ((r << 3) | (r >> 2))) + (150 * ((g << 2) | (g >> 4))) +
                                (29 * ((b << 3) | (b >> 2)))) >>
                               8);
        }
    }
    return true;
}

/**
 * @brief 计算两个归一化特征的余弦相似度
 */
static float cosine_similarity(const float *a, const float *b, int len)
{
    float dot = 0.0f;
    float na = 0.0f;
    float nb = 0.0f;
    for (int i = 0; i < len; i++)
    {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na <= 1e-6f || nb <= 1e-6f)
    {
        return 0.0f;
    }
    return dot / (sqrtf(na) * sqrtf(nb));
}

void face_liveness_init(void)
{
    s_enabled = false;
    s_detector = NULL;
    face_liveness_reset();
    ESP_LOGI(TAG, "活体模块已初始化（启发式：纹理/运动/一致性）");
}

void face_liveness_reset(void)
{
    s_samples = 0;
    s_motion_sum = 0.0f;
    s_motion_n = 0;
    s_texture_ratio = 1.15f;
    s_texture_score = LIVE_TEXTURE_NEUTRAL;
    s_consistency = 1.0f;
    s_prev_valid = false;
    s_prev_emb_len = 0;
    s_report.total = 0.0f;
    s_report.result = LIVENESS_RESULT_UNKNOWN;
}

void face_liveness_set_enabled(bool enable)
{
    s_enabled = enable;
    ESP_LOGI(TAG, "活体校验%s", enable ? "开启" : "关闭");
}

bool face_liveness_is_enabled(void)
{
    return s_enabled;
}

void face_liveness_set_detector(const liveness_detector_t *detector)
{
    s_detector = detector;
    ESP_LOGI(TAG, "活体模型检测器：%s", detector && detector->name ? detector->name : "未注册");
}

liveness_result_t face_liveness_update(const uint16_t *rgb565, int32_t frame_w, int32_t frame_h,
                                       const face_quality_t *q, const float *emb, int emb_len)
{
    if (!s_enabled)
    {
        s_report.total = 100.0f;
        s_report.result = LIVENESS_RESULT_LIVE;
        return LIVENESS_RESULT_LIVE;
    }
    if (rgb565 == NULL || q == NULL || !q->pass)
    {
        return s_report.result;
    }

    // ---- 1. 纹理均匀度 ----
    float ratio = 1.0f;
    const int valid_blocks =
        face_texture_block_ratio(rgb565, frame_w, frame_h, q->x, q->y, q->w, q->h, 3, &ratio);
    if (valid_blocks >= LIVE_TEXTURE_MIN_BLOCKS)
    {
        s_texture_ratio = ratio;
        // ratio 越大越像真人；<1.6 视为过平
        s_texture_score =
            100.0f * clamp01((ratio - LIVE_TEXTURE_LOW) / (LIVE_TEXTURE_HIGH - LIVE_TEXTURE_LOW));
    }

    // ---- 2. 帧间运动：归一化 ROI 的灰度平均绝对差 ----
    uint8_t cur_lum[LIVE_ROI_GRID * LIVE_ROI_GRID];
    if (resample_roi(rgb565, frame_w, frame_h, q->x, q->y, q->w, q->h, cur_lum))
    {
        if (s_prev_valid)
        {
            uint32_t diff = 0;
            for (int i = 0; i < LIVE_ROI_GRID * LIVE_ROI_GRID; i++)
            {
                diff += (uint32_t)abs((int)cur_lum[i] - (int)s_prev_lum[i]);
            }
            s_motion_sum += (float)diff / (float)(LIVE_ROI_GRID * LIVE_ROI_GRID);
            s_motion_n++;
        }
        memcpy(s_prev_lum, cur_lum, sizeof(cur_lum));
        s_prev_valid = true;
    }

    // ---- 3. 特征一致性 ----
    if (emb != NULL && emb_len > 0)
    {
        if (emb_len > LIVE_EMB_MAX)
        {
            emb_len = LIVE_EMB_MAX;
        }
        if (s_prev_emb_len == emb_len)
        {
            s_consistency = cosine_similarity(emb, s_prev_emb, emb_len);
        }
        memcpy(s_prev_emb, emb, (size_t)emb_len * sizeof(float));
        s_prev_emb_len = emb_len;
    }

    s_samples = s_samples < LIVE_MAX_SAMPLES ? s_samples + 1 : LIVE_MAX_SAMPLES;

    // ---- 4. 综合评分 ----
    const float motion_avg = s_motion_n > 0 ? s_motion_sum / (float)s_motion_n : 0.0f;
    const float motion_score = 100.0f * clamp01(motion_avg / LIVE_MOTION_FULL);
    const float consistency_score = 100.0f * clamp01((s_consistency - 0.80f) / 0.18f);

    float total;
    if (s_detector != NULL && s_detector->infer != NULL)
    {
        const float p = s_detector->infer(s_detector->ctx, rgb565, frame_w, frame_h, q);
        total = p >= 0.0f ? p * 100.0f : 0.0f;
    }
    else
    {
        total = 0.35f * s_texture_score + 0.45f * motion_score + 0.20f * consistency_score;
    }

    s_report.texture_score = s_texture_score;
    s_report.motion_score = motion_score;
    s_report.consistency_score = consistency_score;
    s_report.total = total;

    if (s_samples < LIVE_MIN_SAMPLES)
    {
        s_report.result = LIVENESS_RESULT_UNKNOWN;
    }
    else if (total >= LIVE_TOTAL_PASS && motion_avg >= LIVE_MOTION_PASS &&
             s_texture_score >= LIVE_TEXTURE_GATE)
    {
        s_report.result = LIVENESS_RESULT_LIVE;
    }
    else
    {
        s_report.result = LIVENESS_RESULT_SPOOF;
    }
    return s_report.result;
}

void face_liveness_get_report(liveness_report_t *out)
{
    if (out != NULL)
    {
        *out = s_report;
    }
}

void face_liveness_get_status(liveness_status_t *out)
{
    if (out == NULL)
    {
        return;
    }
    out->enabled = s_enabled;
    out->detector_ready = (s_detector != NULL && s_detector->infer != NULL);
    out->sample_count = s_samples;
    out->texture_ratio = s_texture_ratio;
    out->motion = s_motion_n > 0 ? s_motion_sum / (float)s_motion_n : 0.0f;
    out->consistency = s_consistency;
}

const char *face_liveness_result_text(liveness_result_t r)
{
    switch (r)
    {
    case LIVENESS_RESULT_LIVE:
        return "活体通过";
    case LIVENESS_RESULT_SPOOF:
        return "疑似照片/屏幕";
    default:
        return "活体检测中";
    }
}
