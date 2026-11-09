/**
 * @file es8311_codec.c
 * @brief ES8311 音频编解码芯片驱动（兼容新旧版 I2C 接口）
 *
 * 根据 i2c.h 的 I2C_VERSION 宏，在编译期选择两套实现：
 *   - I2C_VERSION_1（旧版驱动）：复用 espressif es8311 组件，
 *     初始化代码与 11-audio_es8311 参考工程完全一致；
 *   - I2C_VERSION_2（新版驱动）：espressif 组件仅支持旧版 I2C，
 *     因此改为直接寄存器操作完成初始化。
 *
 * 默认配置：48 kHz 采样率、12.288 MHz 主时钟、16bit、I2S 从机模式、音量 60%。
 * 使用：i2c_init() -> es8311_codec_init()。
 *
 * @note M5 起采样率从 16 kHz 改为 48 kHz、MCLK 从 6.144 MHz 改为 12.288 MHz：
 *       本板麦克风走 ES7210，两片编解码共用 MCLK，而 ES7210 的时钟系数表里
 *       12.288 MHz/48 kHz 有现成条目、6.144 MHz 没有 16 kHz 条目。
 *       对应寄存器：reg02=0x00（预分频 1、预倍频 0）、reg06=0x00（BCLK 分频 1）、
 *       reg07/08=0x01/0x00（LRCK 分频 256，与 MCLK/采样率一致）。
 */

#include "es8311_codec.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2s.h" /* 复用 I2S_SAMPLE_RATE / I2S_MCLK_FREQ_HZ（必须与 I2S 一致） */

#define TAG "es8311"

#if (I2C_VERSION == I2C_VERSION_1)
#include "es8311.h" /* espressif es8311 组件头文件（仅旧版 I2C 可用） */

/* 旧版 I2C 下的编解码器句柄（音量/静音接口复用组件 API 时需要） */
static es8311_handle_t s_handle = NULL;
#else
/* 新版 I2C 驱动：ES8311 设备句柄（es8311_codec_init() 中注册获取） */
static i2c_master_dev_handle_t s_dev = NULL;

/**
 * @brief 写 ES8311 寄存器
 * @param reg  寄存器地址
 * @param data 写入值
 * @return esp_err_t ESP_OK 成功
 */
static esp_err_t es8311_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(s_dev, buf, 2, pdMS_TO_TICKS(1000));
}

/**
 * @brief 读 ES8311 寄存器
 * @param reg  寄存器地址
 * @param data 输出：读到的值
 * @return esp_err_t ESP_OK 成功
 */
static esp_err_t es8311_read_reg(uint8_t reg, uint8_t *data)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, 1, pdMS_TO_TICKS(1000));
}

/**
 * @brief 初始化寄存器序列 [地址, 值]
 *
 * 复位后各寄存器为默认值，这里直接写完整值。时钟列按 12.288 MHz / 48 kHz 计算：
 *   reg02 = (pre_div-1)<<5 | mult<<3 → 预分频 2、预倍频 1 → 0x08
 *   reg03 = fs_mode<<6 | adc_osr     → 单倍速、OSR=16      → 0x10
 *   reg04 = dac_osr                  → OSR=16              → 0x10
 *   reg05 = (adc_div-1)<<4 | (dac_div-1) → 1/1             → 0x00
 *   reg06 = bclk_div-1               → 1                     → 0x00
 *   reg07/reg08 = lrck 分频 12bit    → 256                   → 0x01/0x00
 */
static const uint8_t es8311_init_seq[][2] = {
    /* 时钟：使能 MCLK，并按 12.288MHz/48kHz 配置分频系数 */
    {0x01, 0x3F}, /* 使能全部时钟（MCLK 输入、不反相） */
    {0x02, 0x00}, /* 预分频 1、预倍频 0（官方系数表 12.288MHz/48kHz 行） */
    {0x03, 0x10}, /* 单倍速采样、ADC OSR=16 */
    {0x04, 0x10}, /* DAC OSR=16 */
    {0x05, 0x00}, /* ADC/DAC 时钟分频 1 */
    {0x06, 0x00}, /* BCLK 分频 1（bit5=0 不反相） */
    {0x07, 0x01}, /* LRCK 分频高字节 */
    {0x08, 0x00}, /* LRCK 分频低字节（共 256） */

    /* 音频格式：I2S 从机模式（reg00 bit6=0 保持默认）、16bit */
    {0x09, 0x0C}, /* DAC 串行端口 16bit */
    {0x0A, 0x0C}, /* ADC 串行端口 16bit */

    /* 上电与系统配置 */
    {0x0D, 0x01}, /* 上电模拟电路 */
    {0x0E, 0x02}, /* 使能模拟 PGA 与 ADC 调制器 */
    {0x12, 0x00}, /* 上电 DAC */
    {0x13, 0x10}, /* 使能耳机驱动输出 */
    {0x1C, 0x6A}, /* ADC 均衡器旁路、数字域直流偏移消除 */
    {0x37, 0x08}, /* DAC 均衡器旁路 */

    /* 麦克风与音量 */
    {0x17, 0xC8}, /* ADC 增益（本板录音走 ES7210，此路不用） */
    {0x14, 0x1A}, /* 模拟麦克风（禁用数字麦克风）、最大 PGA 增益 */
    {0x32, 0x98}, /* DAC 音量 60%（60*256/100 - 1 = 0x98） */
};
#endif /* I2C_VERSION == I2C_VERSION_1 */

esp_err_t es8311_codec_init(void)
{
#if (I2C_VERSION == I2C_VERSION_1)
    /* ==================== 旧版 I2C：复用 espressif es8311 组件 ==================== */
    s_handle = es8311_create(I2C_NUM, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(s_handle != NULL, ESP_FAIL, TAG, "es8311 create failed");

    /* 时钟配置：48 kHz、12.288 MHz、MCLK 来自 MCLK 引脚 */
    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = I2S_MCLK_FREQ_HZ,
        .sample_frequency = I2S_SAMPLE_RATE,
    };

    ESP_RETURN_ON_ERROR(es8311_init(s_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16),
                        TAG, "es8311 init failed");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(s_handle, I2S_MCLK_FREQ_HZ, I2S_SAMPLE_RATE),
                        TAG, "set es8311 sample frequency failed");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(s_handle, ES8311_VOICE_VOLUME, NULL), TAG,
                        "set es8311 volume failed");
    /* 本板录音走 ES7210，ES8311 的麦克风通路关闭 */
    ESP_RETURN_ON_ERROR(es8311_microphone_config(s_handle, false), TAG,
                        "set es8311 microphone failed");

    ESP_LOGI(TAG, "ES8311 初始化完成：%d Hz、MCLK=%d Hz、音量 %d%%", I2S_SAMPLE_RATE,
             (int)I2S_MCLK_FREQ_HZ, ES8311_VOICE_VOLUME);
    return ESP_OK;
#else
    /* ==================== 新版 I2C：直接寄存器操作 ==================== */
    /* 将 ES8311 注册到 I2C 总线，获取设备句柄 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_DEVICE_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_get_bus_handle(), &dev_cfg, &s_dev), TAG,
                        "add device failed");

    /* 复位序列：0x1F 复位 -> 延时 20ms -> 0x00 解除复位 -> 0x80 上电 */
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x00, 0x1F), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x00, 0x00), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x00, 0x80), TAG, "reset failed");

    /* 依次写入初始化寄存器序列 */
    for (size_t i = 0; i < sizeof(es8311_init_seq) / sizeof(es8311_init_seq[0]); i++)
    {
        ESP_RETURN_ON_ERROR(es8311_write_reg(es8311_init_seq[i][0], es8311_init_seq[i][1]), TAG,
                            "write reg failed");
    }

    ESP_LOGI(TAG, "ES8311 初始化完成：%d Hz、MCLK=%d Hz、音量 %d%%", I2S_SAMPLE_RATE,
             (int)I2S_MCLK_FREQ_HZ, ES8311_VOICE_VOLUME);
    return ESP_OK;
#endif /* I2C_VERSION == I2C_VERSION_1 */
}

esp_err_t es8311_set_volume(uint8_t percent)
{
    if (percent > 100)
    {
        percent = 100;
    }
    /* 60% -> 0x98；0% 单独取 0（否则 0*256/100-1 会下溢成 255，反而最大声） */
    const uint8_t reg = (percent == 0) ? 0 : (uint8_t)(percent * 256 / 100 - 1);

#if (I2C_VERSION == I2C_VERSION_1)
    ESP_RETURN_ON_FALSE(s_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "编解码器未初始化");
    return es8311_voice_volume_set(s_handle, percent, NULL);
#else
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "编解码器未初始化");
    return es8311_write_reg(0x32, reg);
#endif
}

esp_err_t es8311_get_volume(uint8_t *percent)
{
    ESP_RETURN_ON_FALSE(percent != NULL, ESP_ERR_INVALID_ARG, TAG, "参数为空");

#if (I2C_VERSION == I2C_VERSION_1)
    ESP_RETURN_ON_FALSE(s_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "编解码器未初始化");
    int vol = 0;
    ESP_RETURN_ON_ERROR(es8311_voice_volume_get(s_handle, &vol), TAG, "读取音量失败");
    *percent = (uint8_t)(vol < 0 ? 0 : (vol > 100 ? 100 : vol));
    return ESP_OK;
#else
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "编解码器未初始化");
    uint8_t reg = 0;
    ESP_RETURN_ON_ERROR(es8311_read_reg(0x32, &reg), TAG, "读取音量寄存器失败");
    /* reg=0 表示 0%；其余按 reg*100/256+1 反算，与组件一致 */
    uint32_t v = (reg == 0) ? 0 : (uint32_t)reg * 100 / 256 + 1;
    *percent = (uint8_t)(v > 100 ? 100 : v);
    return ESP_OK;
#endif
}

esp_err_t es8311_set_mute(bool mute)
{
#if (I2C_VERSION == I2C_VERSION_1)
    ESP_RETURN_ON_FALSE(s_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "编解码器未初始化");
    return es8311_voice_mute(s_handle, mute);
#else
    /* 0x31 低 5 位是其它功能位，必须先读回再改 bit5/bit6（DAC 左右声道静音） */
    uint8_t reg = 0;
    ESP_RETURN_ON_ERROR(es8311_read_reg(0x31, &reg), TAG, "读取静音寄存器失败");
    if (mute)
    {
        reg |= 0x60;
    }
    else
    {
        reg &= (uint8_t)~0x60;
    }
    return es8311_write_reg(0x31, reg);
#endif
}
