/**
 * @file es8311_codec.h
 * @brief ES8311 音频编解码芯片自实现驱动头文件（M5：48 kHz 播放 + 音量/静音）
 *
 * 功能说明：
 *   1. 根据 i2c.h 中的 I2C_VERSION 宏，在编译期选择两套实现：
 *      - I2C_VERSION_1（旧版驱动）：复用 espressif/es8311 组件初始化；
 *      - I2C_VERSION_2（新版驱动）：该组件仅支持旧版 I2C，故改为
 *        直接通过寄存器操作完成初始化；
 *   2. 默认配置：48 kHz 采样率、12.288 MHz 主时钟（256 倍频）、
 *      16bit 数据宽度、I2S 从机模式、音量 60%；
 *   3. 提供音量设置/读取与 DAC 静音开关。
 *
 * 使用说明：
 *   - 必须先调用 i2c_init() 成功初始化 I2C 总线后，
 *     再调用 es8311_codec_init() 完成芯片初始化；
 *   - 采样率/主时钟必须与 i2s.h 中的 I2S_SAMPLE_RATE / I2S_MCLK_MULTIPLE 一致，
 *     否则播放变调或无声。
 *
 * @note ES8311 器件地址由 CE 引脚决定：CE 拉低为 0x18，CE 拉高为 0x19。
 *       本驱动默认使用 0x18。
 */

#ifndef __ES8311_CODEC_H__
#define __ES8311_CODEC_H__

#include <stdbool.h>
#include <stdint.h>

#include "i2c.h" /* 依赖自定义 I2C 头文件，提供 esp_err_t、I2C 版本宏等定义 */

/* ES8311 器件 I2C 地址：CE 引脚拉低时地址为 0x18 */
#define ES8311_DEVICE_ADDR     (0x18)

/* 音频播放音量：取值范围 0~100，默认 60（对应 DAC 音量寄存器 0x32 的值 0x99） */
#define ES8311_VOICE_VOLUME    (60)

/* ====================== 函数声明（extern "C" 防止 C++ 名字修饰） ====================== */
#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief 初始化 ES8311 音频编解码芯片
 * @return esp_err_t 返回 ESP_OK 表示初始化成功，否则返回对应错误码
 *
 * 功能：完成 ES8311 芯片的完整初始化流程：
 *   1. 复位芯片（寄存器 0x00 复位序列）；
 *   2. 配置时钟源与分频系数（48 kHz 采样率、12.288 MHz MCLK）；
 *   3. 配置音频格式（I2S 从机模式、16bit 数据宽度）；
 *   4. 上电模拟电路、使能 DAC/耳机驱动、配置 ADC 滤波器；
 *   5. 配置麦克风（禁用，本板录音走 ES7210）与 DAC 播放音量（60%）。
 *
 * 注意：调用前必须先调用 i2c_init() 完成 I2C 总线初始化。
 */
extern esp_err_t es8311_codec_init(void);

/**
 * @brief 设置 DAC 播放音量
 * @param percent 0~100（0 最轻、100 最响），超出范围会被夹到边界
 * @return esp_err_t ESP_OK 成功
 *
 * @note 寄存器 0x32 与音量百分比的换算：reg = percent * 256 / 100 - 1（percent=0 时 reg=0）。
 */
extern esp_err_t es8311_set_volume(uint8_t percent);

/**
 * @brief 读取当前 DAC 播放音量
 * @param percent 输出：0~100
 * @return esp_err_t ESP_OK 成功
 *
 * @note 换算：percent = reg * 100 / 256 + 1（reg=0 时为 0）。
 */
extern esp_err_t es8311_get_volume(uint8_t *percent);

/**
 * @brief DAC 输出静音开关（寄存器 0x31 的 bit5/bit6，ADC 侧不动）
 * @param mute true 静音，false 取消静音
 * @return esp_err_t ESP_OK 成功
 */
extern esp_err_t es8311_set_mute(bool mute);

#ifdef __cplusplus
}
#endif

#endif /* __ES8311_CODEC_H__ */
