/**
 * @file    face_liveness.h
 * @brief   启发式活体检测（反照片 / 反屏幕回放）+ 活体模型接口预留。
 *
 * @note    - ESP-DL 2.0 不提供反欺骗（anti-spoofing）模型（已核实 model_zoo），
 *            因此本阶段用启发式：纹理均匀度 + 连续帧运动 + 特征一致。
 *            - 接口 `liveness_detector_t` 为后续接入专用模型预留：只需实现
 *            `infer()` 并通过 `face_liveness_set_detector()` 注册，业务层不改。
 *            - 本模块只做统计，不持有摄像头帧；调用方（face_task）负责按帧调用
 *            `face_liveness_update()`。
 */

#ifndef __FACE_LIVENESS_H__
#define __FACE_LIVENESS_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "face_quality.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 活体判定结果 */
typedef enum
{
    LIVENESS_RESULT_UNKNOWN = 0, /**< 样本不足，无法判断 */
    LIVENESS_RESULT_LIVE,        /**< 判为真人 */
    LIVENESS_RESULT_SPOOF,       /**< 判为非活体（照片/屏幕） */
} liveness_result_t;

/** @brief 各子指标得分与综合分（0-100） */
typedef struct
{
    float texture_score;     /**< 纹理分布得分 */
    float motion_score;      /**< 帧间运动得分 */
    float consistency_score; /**< 特征一致得分 */
    float total;             /**< 加权综合分 */
    liveness_result_t result;
} liveness_report_t;

/**
 * @brief 活体模型抽象接口（预留：接入专用反欺骗模型时实现）
 * @note  输入为整帧 RGB565 与人脸框；返回 [0,1] 活体概率，<0 表示模型未就绪。
 */
typedef struct liveness_detector
{
    /** @brief 模型名称（用于日志/诊断页） */
    const char *name;
    /**
     * @brief 推理
     * @param ctx     实现私有上下文
     * @param rgb565  整帧 RGB565
     * @param frame_w 帧宽
     * @param frame_h 帧高
     * @param q       人脸框与质量
     * @return 活体概率 [0,1]；<0 表示模型/数据不可用
     */
    float (*infer)(void *ctx, const uint16_t *rgb565, int32_t frame_w, int32_t frame_h,
                   const face_quality_t *q);
    /** @brief 实现私有上下文 */
    void *ctx;
} liveness_detector_t;

/** @brief 活体模块当前参数（诊断页展示用） */
typedef struct
{
    bool enabled;             /**< 是否启用（NVS 可覆盖） */
    bool detector_ready;      /**< 是否已注册模型检测器 */
    int sample_count;         /**< 当前窗口内样本数 */
    float texture_ratio;      /**< 最近一次纹理均匀度比 */
    float motion;             /**< 最近一次帧间运动均值 */
    float consistency;        /**< 最近一次特征余弦一致度 */
} liveness_status_t;

/**
 * @brief 初始化活体模块（清空统计窗口）
 */
void face_liveness_init(void);

/**
 * @brief 开始一次新的活体评估窗口（新人脸轨迹或新一轮识别时调用）
 */
void face_liveness_reset(void);

/**
 * @brief 启用/禁用活体校验（NVS 设置变化时调用）
 * @param enable true 开启，false 关闭（关闭后一律判为 LIVE）
 */
void face_liveness_set_enabled(bool enable);

/**
 * @brief 查询是否启用
 */
bool face_liveness_is_enabled(void);

/**
 * @brief 注册活体模型检测器（预留接口）
 * @param detector 检测器实现，NULL 表示撤销
 */
void face_liveness_set_detector(const liveness_detector_t *detector);

/**
 * @brief 喂入一帧检测结果：更新纹理 / 运动 / 特征一致统计
 * @param rgb565  整帧 RGB565
 * @param frame_w 帧宽
 * @param frame_h 帧高
 * @param q       本帧人脸质量（必须 pass 才会被计入运动统计）
 * @param emb     本帧 112-d 特征向量（可为 NULL：只统计纹理与运动）
 * @param emb_len 特征长度
 * @return 当前综合评估结果（样本不足时返回 UNKNOWN）
 * @note  调用频率由 face_task 控制，建议 ≤ 10 Hz，且只在主页/识别流程中调用。
 */
liveness_result_t face_liveness_update(const uint16_t *rgb565, int32_t frame_w, int32_t frame_h,
                                       const face_quality_t *q, const float *emb,
                                       int emb_len);

/**
 * @brief 获取最近一次评估的完整报告
 * @param out 输出报告
 */
void face_liveness_get_report(liveness_report_t *out);

/**
 * @brief 获取模块当前状态（诊断页）
 * @param out 输出状态
 */
void face_liveness_get_status(liveness_status_t *out);

/**
 * @brief 活体结果转中文说明（用于状态栏/日志）
 */
const char *face_liveness_result_text(liveness_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* __FACE_LIVENESS_H__ */
