/**
 * @file    face_quality.h
 * @brief   人脸质量评估：尺寸、亮度、清晰度（Laplacian 方差）、偏航角估算。
 *
 * @note    - 输入统一为摄像头原生 RGB565 字节流（高字节在前：低地址字节 = RRRRRGGG），
 *            以 uint16_t 读出后 bit15..8 = GGGBBBBB、bit7..0 = RRRRRGGG（见
 *            face_quality.c 的 rgb565_to_luma）；区域坐标为
 *            整帧坐标，超出边界会被裁剪。
 *            - 所有指标只做“够不够好”的粗判，不追求与学术指标严格一致；
 *            阈值集中在 face_quality.c 顶部，便于现场调参。
 *            - 纯 C、无堆分配：可被 face_task 每帧调用，也可被 UI 诊断页调用。
 */

#ifndef __FACE_QUALITY_H__
#define __FACE_QUALITY_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 质量门限与结果（阈值同时用于注册门限与识别门限） */
typedef struct
{
    int32_t x;           /**< 人脸框左上角 X（整帧坐标） */
    int32_t y;           /**< 人脸框左上角 Y（整帧坐标） */
    int32_t w;           /**< 人脸框宽度（像素） */
    int32_t h;           /**< 人脸框高度（像素） */
    float brightness;    /**< 平均亮度（0-255） */
    float sharpness;     /**< 灰度 Laplacian 方差，越大越清晰 */
    float yaw;           /**< 偏航估算（相对脸宽的归一化位移，绝对值越大越偏侧） */
    bool size_ok;        /**< 短边是否达到最小像素要求 */
    bool brightness_ok;  /**< 亮度是否在合理区间 */
    bool sharpness_ok;   /**< 清晰度是否达标 */
    bool frontal_ok;     /**< 是否接近正脸 */
    bool pass;           /**< 以上全部满足（注册/识别硬门限） */
} face_quality_t;

/**
 * @brief 计算人脸区域质量指标
 * @param rgb565   整帧 RGB565 数据
 * @param frame_w  帧宽
 * @param frame_h  帧高
 * @param x        人脸框左上角 X
 * @param y        人脸框左上角 Y
 * @param w        人脸框宽
 * @param h        人脸框高
 * @param landmarks 5 个关键点 x/y（10 个 int32，可为 NULL）；用于估算偏航角
 * @param out      输出结果（不可为 NULL）
 * @return true 计算成功；false 区域非法或调用方未提供有效参数
 * @note  帧内取样步长自动按区域大小调整，最大约 64x64 个采样点，单次耗时 < 1 ms。
 */
bool face_quality_eval(const uint16_t *rgb565, int32_t frame_w, int32_t frame_h, int32_t x,
                       int32_t y, int32_t w, int32_t h, const int32_t *landmarks,
                       face_quality_t *out);

/**
 * @brief 计算整帧（或指定区域）平均亮度，供活体/自动曝光诊断使用
 * @param rgb565 整帧 RGB565 数据
 * @param frame_w 帧宽
 * @param frame_h 帧高
 * @return 平均亮度（0-255），参数非法时返回 -1
 */
float face_frame_brightness(const uint16_t *rgb565, int32_t frame_w, int32_t frame_h);

/**
 * @brief 计算人脸区域的分区梯度能量比（纹理均匀度）
 * @param rgb565   整帧 RGB565 数据
 * @param frame_w  帧宽
 * @param frame_h  帧高
 * @param x,y,w,h  人脸框（整帧坐标）
 * @param grid     每边分区数（2 或 3；3x3 更敏感）
 * @param ratio    输出：各有效分区平均梯度能量的 max/min 比（越接近 1 越“平”）
 * @return 有效分区数（过暗或无梯度分区跳过），0 表示无法评估
 * @note  真人脸在 3x3 分区上的纹理能量差异明显；照片/屏幕回放通常更均匀（ratio < 1.6）。
 *        函数内部按 2 像素步长取样，单次耗时 < 1 ms。
 */
int face_texture_block_ratio(const uint16_t *rgb565, int32_t frame_w, int32_t frame_h, int32_t x,
                             int32_t y, int32_t w, int32_t h, int grid, float *ratio);

#ifdef __cplusplus
}
#endif

#endif /* __FACE_QUALITY_H__ */
