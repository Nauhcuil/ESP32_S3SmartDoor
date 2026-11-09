/**
 * @file es7210_codec.c
 * @brief ES7210 四路 ADC 驱动实现（新版 I2C 驱动直接写寄存器）
 *
 * 寄存器序列参照 Espressif 官方 es7210 组件（es7210_config_codec），
 * 只做寄存器配置，不引入其旧版 I2C 依赖：
 *   1. 0x00 写 0xFF → 0x32：软件复位并设定上电初始化时间；
 *   2. 0x09/0x0A 写 0x30：时序控制；
 *   3. 0x23/0x22/0x21/0x20：ADC1~4 高通滤波系数；
 *   4. 0x11 写 0x62：I2S 格式 + 16bit（0x02 | 0x60）；0x12 写 0x00：非 TDM；
 *   5. 0x40 写 0xC3：模拟部分上电、VMID；
 *   6. 0x41/0x42 写 0x70：MICBIAS 2.87 V；
 *   7. 0x43~0x46 写 gain|0x10：四路模拟增益；
 *   8. 0x47~0x4A 写 0x08：四路 MIC 使能；
 *   9. 0x07 写 0x20：ADC OSR=32；
 *   10. 0x02 写 0x03：ADC 时钟分频 3、DLL 旁路、倍频使能（12.288 MHz → 48 kHz）；
 *   11. 0x04/0x05 写 0x03/0x00：LRCK 分频 768；
 *   12. 0x06 写 0x04：DLL 掉电；
 *   13. 0x4B/0x4C 写 0x0F：MIC 偏置与 ADC/PGA 上电；
 *   14. 0x00 写 0x71 → 0x41：使能器件。
 *
 * @note 时钟表（来自官方组件 es7210_coeff_div，mclk=12.288MHz、lrck=48kHz 行）：
 *       {12288000, 48000, adc_div=0x01, dll=0x01, doubler=0x01, osr=0x20,
 *        lrck_h=0x01, lrck_l=0x00}
 *       → reg02 = adc_div|(doubler<<6)|(dll<<7) = 0x01|0x40|0x80 = 0xC1，
 *         reg04/05 = 0x01/0x00。
 *       本文件按官方代码原样写入该行（0x02=0xC1、0x04=0x01、0x05=0x00）。
 */

#include "es7210_codec.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"

#define TAG "es7210"

/** @brief I2C 设备句柄（es7210_codec_init 注册获取） */
static i2c_master_dev_handle_t s_dev = NULL;

/**
 * @brief 写 ES7210 寄存器
 */
static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, pdMS_TO_TICKS(1000));
}

/** @brief 带错误检查的写寄存器宏（失败时直接返回） */
#define WRITE_REG(reg, val)                                                                        \
    do                                                                                             \
    {                                                                                              \
        const esp_err_t err = write_reg((reg), (val));                                             \
        if (err != ESP_OK)                                                                         \
        {                                                                                          \
            ESP_LOGE(TAG, "写寄存器 0x%02x 失败：%s", (unsigned)(reg), esp_err_to_name(err));       \
            return err;                                                                            \
        }                                                                                          \
    } while (0)

esp_err_t es7210_codec_read_reg(uint8_t reg, uint8_t *val)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL && val != NULL, ESP_ERR_INVALID_ARG, TAG, "参数非法");
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, pdMS_TO_TICKS(1000));
}

esp_err_t es7210_set_mic_gain(uint8_t gain_db)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "ES7210 未初始化");
    if (gain_db > 14)
    {
        gain_db = 14;
    }
    /* bit4 固定为 1（增益寄存器使能位），与官方组件一致 */
    const uint8_t v = (uint8_t)(gain_db | 0x10);
    return write_reg(0x43, v) == ESP_OK && write_reg(0x44, v) == ESP_OK &&
                   write_reg(0x45, v) == ESP_OK && write_reg(0x46, v) == ESP_OK
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t es7210_codec_init(void)
{
    /* 1. 注册 I2C 设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES7210_DEVICE_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_get_bus_handle(), &dev_cfg, &s_dev), TAG,
                        "注册 ES7210 失败");

    /* 2. 软件复位 + 上电初始化时间 */
    WRITE_REG(0x00, 0xFF);
    WRITE_REG(0x00, 0x32);
    WRITE_REG(0x09, 0x30);
    WRITE_REG(0x0A, 0x30);

    /* 3. ADC1~4 高通滤波 */
    WRITE_REG(0x23, 0x2A);
    WRITE_REG(0x22, 0x0A);
    WRITE_REG(0x21, 0x2A);
    WRITE_REG(0x20, 0x0A);

    /* 4. I2S 格式（标准 I2S）、16bit、非 TDM */
    WRITE_REG(0x11, 0x02 | 0x60);
    WRITE_REG(0x12, 0x00);

    /* 5. 模拟部分上电、VMID */
    WRITE_REG(0x40, 0xC3);

    /* 6. MICBIAS 2.87 V（模拟麦偏置） */
    WRITE_REG(0x41, 0x70);
    WRITE_REG(0x42, 0x70);

    /* 7. 模拟增益 */
    const uint8_t gain = (uint8_t)(ES7210_MIC_GAIN_DB | 0x10);
    WRITE_REG(0x43, gain);
    WRITE_REG(0x44, gain);
    WRITE_REG(0x45, gain);
    WRITE_REG(0x46, gain);

    /* 8. MIC1~4 使能 */
    WRITE_REG(0x47, 0x08);
    WRITE_REG(0x48, 0x08);
    WRITE_REG(0x49, 0x08);
    WRITE_REG(0x4A, 0x08);

    /* 9~11. 时钟系数：12.288 MHz → 48 kHz */
    WRITE_REG(0x07, 0x20); /* ADC OSR=32 */
    WRITE_REG(0x02, 0xC1); /* adc_div=1、doubler=1、dll_bypass=1 */
    WRITE_REG(0x04, 0x01); /* LRCK 分频高 4 位 */
    WRITE_REG(0x05, 0x00); /* LRCK 分频低 8 位（共 256） */

    /* 12. DLL 掉电 */
    WRITE_REG(0x06, 0x04);

    /* 13. MIC 偏置 & ADC/PGA 上电 */
    WRITE_REG(0x4B, 0x0F);
    WRITE_REG(0x4C, 0x0F);

    /* 14. 使能器件 */
    WRITE_REG(0x00, 0x71);
    WRITE_REG(0x00, 0x41);

    /* 读回关键寄存器供排查：0x12 应为 0x00（非 TDM），0x11 应为 0x62 */
    uint8_t r11 = 0, r12 = 0;
    (void)es7210_codec_read_reg(0x11, &r11);
    (void)es7210_codec_read_reg(0x12, &r12);
    ESP_LOGI(TAG, "ES7210 初始化完成：48 kHz、I2S 16bit、增益 %d dB（0x11=0x%02x 0x12=0x%02x）",
             ES7210_MIC_GAIN_DB * 3, r11, r12);
    return ESP_OK;
}
