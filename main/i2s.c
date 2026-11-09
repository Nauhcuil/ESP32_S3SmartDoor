/**
 * @file i2s.c
 * @brief I2S0 全双工音频驱动实现（M5）
 *
 * 功能说明：
 *   1. 创建 I2S0 的 TX/RX 两个标准模式通道（主模式、16bit、立体声、48 kHz），
 *      TX → ES8311（扬声器播放），RX ← ES7210（麦克风阵列录音）；
 *   2. 两块编解码共用 MCLK(38)/BCLK(14)/WS(13)，由 ESP32 作主机产生，
 *      因此 MCLK 必须同时满足两片的时钟需求：12.288 MHz = 48 kHz x 256；
 *   3. 对外提供阻塞式读写接口，供 audio_service（播放）与后续 voice 任务（录音）使用。
 *
 * 使用说明：
 *   - 初始化顺序：i2c_init() → i2s_driver_init() → es8311_codec_init() / es7210_codec_init()；
 *   - RX 通道随驱动一起使能后**始终保持开启**：ES7210 靠外部 BCLK/WS 工作，
 *     停读时若关掉 RX，时钟消失会让 ES7210 状态机错乱、恢复录音时出现爆音；
 *     代价只是 DMA 环形缓冲被持续覆盖（约 6 x 240 x 4 B ≈ 5.8 KB DMARAM）。
 *
 * @note 旧版实现里循环播放 canon.pcm 的 i2s_music 任务已删除（M5 起播放统一走
 *       audio_service，且该任务在写失败时直接 abort()，不适合常驻固件）。
 */

#include "i2s.h"

#include <string.h>

#include "esp_check.h"         /* ESP_RETURN_ON_ERROR / ESP_RETURN_ON_FALSE */
#include "esp_log.h"           /* ESP_LOGE / ESP_LOGI */
#include "freertos/FreeRTOS.h" /* portMAX_DELAY / pdMS_TO_TICKS */

/** 日志标签 */
static const char *TAG = "i2s";

/** @brief I2S 发送通道句柄（i2s_driver_init 创建） */
static i2s_chan_handle_t s_tx = NULL;
/** @brief I2S 接收通道句柄（i2s_driver_init 创建） */
static i2s_chan_handle_t s_rx = NULL;

esp_err_t i2s_driver_init(void)
{
    /* 幂等：已经初始化过就直接返回 */
    if (s_tx != NULL && s_rx != NULL)
    {
        return ESP_OK;
    }

    /* 通道配置：I2S0、主机模式；auto_clear 让 TX 在无数据时自动输出 0，
     * 避免停止播放后 DOUT 上残留旧采样被 ES8311 反复播放（爆音） */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    /* DMA 缓冲从默认 6x240 帧降到 4x240 帧：TX/RX 各约 3.8 KB 内部 DMA RAM
     * （默认值各 5.8 KB）。M4 之后内部 RAM 的“最大连续块”只剩 ~8.7 KB，
     * 界面工作线程要 3 KB，这里省下的 4 KB 留给它；20 ms 缓冲足够播放不欠载。 */
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 240;

    /* 一次性创建 TX + RX 两个通道：同一外设下的全双工组合，自动共用 BCLK/WS */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "创建 I2S 全双工通道失败");

    /* 标准模式参数：TX 与 RX 完全一致（同一组时钟引脚，配置必须相同） */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO, /* 主时钟：12.288 MHz，两块编解码共用 */
            .bclk = I2S_BCK_IO, /* 位时钟：48 kHz x 32 bit = 1.536 MHz */
            .ws = I2S_WS_IO,    /* 字选择：48 kHz */
            .dout = I2S_DO_IO,  /* 数据输出 → ES8311 DSDIN */
            .din = I2S_DI_IO,   /* 数据输入 ← ES7210 SD_OUT */
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /* MCLK 倍频 256（16.384 MHz 以下的最大值）：12.288 MHz 正好落在 ES7210
     * 系数表的 48 kHz 行，也满足 ES8311 的预分频要求 */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE;

    /* TX/RX 分别初始化并映射到同一组时钟引脚 */
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "初始化 I2S TX 失败");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "初始化 I2S RX 失败");

    /* 两个通道都使能：RX 常开以保证 BCLK/WS 持续输出给编解码芯片 */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "使能 I2S TX 失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "使能 I2S RX 失败");

    ESP_LOGI(TAG, "I2S0 全双工初始化完成：%d Hz，MCLK=%d Hz（256 倍频），TX=GPIO%d RX=GPIO%d",
             I2S_SAMPLE_RATE, (int)I2S_MCLK_FREQ_HZ, (int)I2S_DO_IO, (int)I2S_DI_IO);
    return ESP_OK;
}

esp_err_t i2s_tx_write(const void *data, size_t bytes, size_t *written, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_tx != NULL, ESP_ERR_INVALID_STATE, TAG, "I2S TX 未初始化");
    ESP_RETURN_ON_FALSE(data != NULL && bytes >= I2S_FRAME_BYTES, ESP_ERR_INVALID_ARG, TAG,
                        "写入参数非法");

    size_t done = 0;
    const esp_err_t ret = i2s_channel_write(s_tx, data, bytes, &done,
                                            timeout_ms == 0 ? portMAX_DELAY
                                                            : pdMS_TO_TICKS(timeout_ms));
    if (written != NULL)
    {
        *written = done;
    }
    return ret;
}

esp_err_t i2s_rx_read(void *data, size_t bytes, size_t *read, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_rx != NULL, ESP_ERR_INVALID_STATE, TAG, "I2S RX 未初始化");
    ESP_RETURN_ON_FALSE(data != NULL && bytes >= I2S_FRAME_BYTES, ESP_ERR_INVALID_ARG, TAG,
                        "读取参数非法");

    size_t done = 0;
    const esp_err_t ret = i2s_channel_read(s_rx, data, bytes, &done,
                                           timeout_ms == 0 ? portMAX_DELAY
                                                           : pdMS_TO_TICKS(timeout_ms));
    if (read != NULL)
    {
        *read = done;
    }
    return ret;
}

esp_err_t i2s_tx_set_enabled(bool enable)
{
    ESP_RETURN_ON_FALSE(s_tx != NULL, ESP_ERR_INVALID_STATE, TAG, "I2S TX 未初始化");
    return enable ? i2s_channel_enable(s_tx) : i2s_channel_disable(s_tx);
}

esp_err_t i2s_rx_set_enabled(bool enable)
{
    ESP_RETURN_ON_FALSE(s_rx != NULL, ESP_ERR_INVALID_STATE, TAG, "I2S RX 未初始化");
    return enable ? i2s_channel_enable(s_rx) : i2s_channel_disable(s_rx);
}

size_t i2s_rx_drain(size_t max_frames)
{
    if (s_rx == NULL)
    {
        return 0;
    }

    /* 240 帧 = 5 ms @48 kHz，与 DMA 块大小一致；960 字节栈缓冲 */
    enum { DRAIN_FRAMES = 240 };
    int16_t scratch[DRAIN_FRAMES * 2];

    const size_t limit = (max_frames == 0) ? (size_t)-1 : max_frames;
    size_t total = 0;
    while (total < limit)
    {
        size_t done = 0;
        /* 超时 0：只取已经到达的数据，绝不阻塞 */
        const esp_err_t ret = i2s_channel_read(s_rx, scratch, sizeof(scratch), &done, 0);
        if (ret != ESP_OK || done == 0)
        {
            break;
        }
        total += done / I2S_FRAME_BYTES;
    }
    return total;
}
