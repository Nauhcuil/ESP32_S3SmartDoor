/**
 * @file i2s.h
 * @brief I2S 音频外设驱动接口（M5：48 kHz 全双工，ES8311 播放 + ES7210 录音）
 *
 * 功能说明：
 *   1. 定义 I2S0 全双工通道的引脚宏（MCLK/BCLK/WS/DOUT/DIN）与音频参数宏；
 *   2. 对外提供 I2S 初始化、发送写入、接收读取、通道使能/关闭等接口；
 *   3. 采样率固定 48 kHz、MCLK 256 倍频 = 12.288 MHz——这是板载 ES7210/ES8311
 *      共用 MCLK 时可用的对齐点（ES7210 系数表里 12.288 MHz/48 kHz 有现成条目，
 *      6.144 MHz 则没有 16 kHz 条目）。
 *
 * @note 引脚来源（板级原理图）：
 *       - ES8311（播放）：MCLK=GPIO38、SCLK=GPIO14、LRCK=GPIO13、DSDIN=GPIO45；
 *       - ES7210（录音）：MCLK=GPIO38、SCLK=GPIO14、LRCK=GPIO13、SD_OUT=GPIO12。
 *       两块编解码共用同一组 MCLK/BCLK/WS，由 ESP32 作 I2S 主机产生。
 */

#ifndef __I2S_H__
#define __I2S_H__

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"    /* GPIO 驱动头文件，提供 GPIO_NUM_x 引脚宏定义 */
#include "driver/i2s_std.h" /* I2S 标准模式驱动头文件，提供 i2s_chan_handle_t 等类型 */
#include "esp_err.h"        /* ESP 错误码类型定义 */

/* ====================== I2S 引脚宏定义 ================================= */
#define I2S_NUM         (0)            /* I2S 外设编号：使用 I2S0 */
#define I2S_MCK_IO      (GPIO_NUM_38)  /* 主时钟 MCLK：两块编解码共用 */
#define I2S_BCK_IO      (GPIO_NUM_14)  /* 位时钟 BCLK：两块编解码共用 */
#define I2S_WS_IO       (GPIO_NUM_13)  /* 字选择 WS：两块编解码共用 */
#define I2S_DO_IO       (GPIO_NUM_45)  /* 数据输出 DOUT → ES8311 DSDIN（播放） */
#define I2S_DI_IO       (GPIO_NUM_12)  /* 数据输入 DIN ← ES7210 SD_OUT（录音） */

/* ====================== 音频参数宏 ===================================== */
/** 采样率：48 kHz（ES7210/ES8311 共用 MCLK 时的对齐点） */
#define I2S_SAMPLE_RATE         (48000)
/** MCLK 倍频：256 倍 → 48 kHz x 256 = 12.288 MHz */
#define I2S_MCLK_MULTIPLE       (256)
/** MCLK 实际频率（Hz） */
#define I2S_MCLK_FREQ_HZ        (I2S_SAMPLE_RATE * I2S_MCLK_MULTIPLE)
/** 单帧字节数：16bit x 立体声 */
#define I2S_FRAME_BYTES         (4)

/* ====================== 函数声明（extern "C" 防止 C++ 名字修饰） ============ */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2S0 全双工通道（主模式、16bit、Philips I2S、48 kHz）
 * @return esp_err_t ESP_OK 成功；其余为失败原因
 *
 * 功能：
 *   1. 用 i2s_new_channel 一次性创建 TX/RX 两个通道（全双工，共用 BCLK/WS）；
 *   2. 两者都按标准模式配置：48 kHz、16bit、立体声、MCLK 256 倍频；
 *   3. 使能 TX 与 RX——RX 必须常开，编解码的 LRCK/BCLK 才不会在停读时消失。
 *
 * @note 幂等：重复调用直接返回 ESP_OK。
 */
esp_err_t i2s_driver_init(void);

/**
 * @brief 向 I2S TX 写入数据（阻塞到写完成或超时）
 * @param data       待写数据（16bit 立体声交织，即 4 字节/帧）
 * @param bytes      字节数，必须是 4 的整数倍
 * @param written    输出：实际写入字节数（可为 NULL）
 * @param timeout_ms 等待超时（毫秒）
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t i2s_tx_write(const void *data, size_t bytes, size_t *written, uint32_t timeout_ms);

/**
 * @brief 从 I2S RX 读取数据（阻塞到读完成或超时）
 * @param data       存放数据的缓冲区（16bit 立体声交织）
 * @param bytes      期望读取的字节数，必须是 4 的整数倍
 * @param read       输出：实际读取字节数（可为 NULL）
 * @param timeout_ms 等待超时（毫秒）
 * @return esp_err_t ESP_OK 成功；ESP_ERR_TIMEOUT 表示本次没读到足够数据
 */
esp_err_t i2s_rx_read(void *data, size_t bytes, size_t *read, uint32_t timeout_ms);

/**
 * @brief 使能/关闭 I2S TX 通道
 * @param enable true 使能，false 关闭
 * @return esp_err_t ESP_OK 成功
 *
 * @note 关闭 TX 后 DOUT 停止输出、DMA 缓冲自动清零；播放前再使能即可。
 */
esp_err_t i2s_tx_set_enabled(bool enable);

/**
 * @brief 使能/关闭 I2S RX 通道
 * @param enable true 使能，false 关闭
 * @return esp_err_t ESP_OK 成功
 *
 * @note 当前实现始终开启 RX（编解码依赖 BCLK/WS 常驻），暂不对外开放停用。
 */
esp_err_t i2s_rx_set_enabled(bool enable);

/**
 * @brief 丢弃 RX DMA 中已积压的数据（最多 max_frames 帧）
 * @param max_frames 最多丢弃的帧数（每帧 4 字节）；传 0 表示按 DMA 缓冲容量丢完
 * @return size_t 实际丢弃的帧数
 *
 * @note 语音任务启动前、或长时间不读 I2S 之后必须调用：RX DMA 是环形缓冲，
 *       旧数据会一直排队，直接开始读会先读到几十毫秒之前的音频（唤醒响应变慢、
 *       甚至把上一句播报当成唤醒词）。读超时设 0，不阻塞。
 */
size_t i2s_rx_drain(size_t max_frames);

#ifdef __cplusplus
}
#endif

#endif /* __I2S_H__ */
