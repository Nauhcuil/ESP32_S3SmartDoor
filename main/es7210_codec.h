/**
 * @file es7210_codec.h
 * @brief ES7210 四路 ADC（麦克风阵列）驱动头文件（M5）
 *
 * 功能说明：
 *   1. 通过 I2C 直接写寄存器完成 ES7210 初始化：48 kHz、I2S 从机、16bit；
 *   2. 本板麦克风是**模拟麦**，接 ES7210 的模拟输入（MICBIAS 2.87 V 供电），
 *      与 ES8311 无关——ES8311 只负责 DAC 播放；
 *   3. 提供寄存器读写接口，供上电自检打印芯片状态。
 *
 * 使用说明：
 *   - 必须在 i2c_init() 与 i2s_driver_init() 之后调用：ES7210 是 I2S 从机，
 *     需要 ESP32 先送出 MCLK/BCLK/WS；
 *   - 采样率/主时钟必须与 i2s.h 一致（48 kHz / 12.288 MHz）。
 *
 * @note 为什么不用 espressif/es7210 组件：该组件（1.0.1）内部用的是旧版 I2C 驱动
 *       （i2c_cmd_link_create 等），与本站 "必须使用新版驱动" 的约束冲突
 *       （混用会报 "driver_ng is not allowed"）。这里只照搬其寄存器序列。
 */

#ifndef __ES7210_CODEC_H__
#define __ES7210_CODEC_H__

#include <stdint.h>

#include "esp_err.h"

/* ES7210 器件 I2C 地址：由 CE 引脚决定，本板为 0x41 */
#define ES7210_DEVICE_ADDR     (0x41)

/* 麦克风模拟增益（寄存器 0x43~0x46 的低 4 位，单位 dB，0~14 对应 0~37.5 dB；
 * 默认 10 = 30 dB，与官方例程一致） */
#define ES7210_MIC_GAIN_DB     (10)

/* ====================== 函数声明（extern "C" 防止 C++ 名字修饰） ====================== */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ES7210：软件复位 + 时钟 + I2S 格式 + 麦克风偏置/增益 + 使能
 * @return esp_err_t ESP_OK 成功；I2C 通信失败返回对应错误码
 *
 * @note 配置为 4 声道非 TDM 的 I2S：ESP32 RX 取到的是「MIC1/MIC2」两路（左右声道）。
 *       若要 4 路全收，把寄存器 0x12 改成 0x02 并让 I2S RX 改 4 槽 TDM，
 *       TX 侧也必须同步改 4 槽（全双工共用 BCLK/WS，帧长必须一致）。
 */
extern esp_err_t es7210_codec_init(void);

/**
 * @brief 读 ES7210 单个寄存器（自检/排查用）
 * @param reg 寄存器地址
 * @param val 输出：读到的值
 * @return esp_err_t ESP_OK 成功
 */
extern esp_err_t es7210_codec_read_reg(uint8_t reg, uint8_t *val);

/**
 * @brief 设置四路麦克风模拟增益
 * @param gain_db 0~14（对应 0~37.5 dB，步进 3 dB），超出范围会被夹到边界
 * @return esp_err_t ESP_OK 成功
 */
extern esp_err_t es7210_set_mic_gain(uint8_t gain_db);

#ifdef __cplusplus
}
#endif

#endif /* __ES7210_CODEC_H__ */
