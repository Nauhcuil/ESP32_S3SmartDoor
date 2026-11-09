/**
 * @file    face_selftest.h
 * @brief   模型自检：用 ESP-DL 示例中确定能检出人脸的图像喂给 MSR01 / Recognizer，
 *          与摄像头实时帧的结果对照，区分“模型推理链路有问题”与“摄像头数据有问题”。
 *
 * @note    自检开关在对应 .cpp 中（FACE_SELFTEST / FACE_SELFTEST_IMAGE_DEBUG），
 *          正式固件必须保持 0。图像取自 components/esp-dl/examples 的 image.hpp。
 */

#ifndef __FACE_SELFTEST_H__
#define __FACE_SELFTEST_H__

#ifdef __cplusplus

class HumanFaceDetectMSR01;

/**
 * @brief 对示例图像跑一次 MSR01 两级检测
 * @param detector MSR01 检测器
 * @return 候选框数量；图像数组不存在时返回 -1
 * @note  输入为 RGB888（示例图像原始格式），shape 为 {高, 宽, 3}。
 */
long face_selftest_detect(HumanFaceDetectMSR01 &detector);

#endif /* __cplusplus */

#endif /* __FACE_SELFTEST_H__ */
