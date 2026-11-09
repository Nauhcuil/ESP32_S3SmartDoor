/**
 * @file audio_service.c
 * @brief 音频服务实现（M5）：I2S TX 唯一 owner，提示音/播报串行播放
 *
 * 播放时序（每次播放都完整走一遍）：
 *   1. 拿 g_audio_mutex（单请求在飞），置 SYS_EVENT_AUDIO_BUSY；
 *   2. 写尾部静音到 I2S（清掉 DMA 里上一次残留的采样）→ ES8311 解除静音；
 *      → 延时 20 ms（DAC 内部滤波器稳定）→ 拉高 PA_EN(PCA9557 IO1)；
 *   3. 按块把单声道扩成 L/R 立体声写入 I2S；
 *   4. 补 30 ms 静音，让 DAC 把最后一个采样冲出去；
 *   5. ES8311 静音 → 延时 10 ms → 拉低 PA_EN（避免功放断电的“啪”声）；
 *   6. 清 AUDIO_BUSY、释放互斥量。
 *
 * @note PA_EN 在 PCA9557 的 IO1：上电默认拉低（pca9557_init 写 0x05），
 *       只在播放期间拉高，既省电也避免本底噪声。
 * @note 为什么用“ES8311 静音 + PA 通断”两步而不是只做其中一步：
 *       - 只关 PA：DAC 仍在输出，PA 通断瞬间会有直流跳变 → 爆音；
 *       - 只静音 DAC：PA 一直开，底噪大且与录音阶段抢电源。
 */

#include "audio_service.h"

#include <math.h>
#include <string.h>

#include "app_events.h"
#include "es7210_codec.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2s.h"
#include "pca9557.h"

/** 日志标签 */
static const char *TAG = "audio";

/**
 * @brief 每次读写 I2S 的帧数（每帧 4 字节）→ 1024 帧 = 21 ms @48 kHz
 * @note  这个值的两倍就是 s_stereo_buf 的元素数（int16）。**调小它不会省内部 RAM**：
 *        缓冲常驻堆，只影响每次 I2S 读写的粒度；语音任务每次自己读 VOICE_RX_FRAMES，
 *        与这里无关，不要为了“让语音先拿到数据”改它。
 */
#define AUDIO_CHUNK_FRAMES  (1024)
/** @brief 扩声道缓冲的元素数：单声道 1024 点 / 立体声 2048 点 */
#define AUDIO_CHUNK_SAMPLES (AUDIO_CHUNK_FRAMES * 2)
/** @brief 每次播放前后补的静音时长（毫秒） */
#define AUDIO_TAIL_SILENCE_MS (30)
/** @brief 单次 I2S 写超时 */
#define AUDIO_IO_TIMEOUT_MS (500)

/** @brief 当前音量（0~100），初始为编解码默认值 */
static volatile uint8_t s_volume = ES8311_VOICE_VOLUME;
/** @brief 初始化结果 */
static bool s_ready = false;
/** @brief 是否正在播放 */
static volatile bool s_busy = false;

/* ====================== 内部辅助 ====================== */

/**
 * @brief 立体声扩声道缓冲（8 KB，M4 起内部 RAM 紧张，放到 PSRAM）
 * @note 只被 audio_service 的播放路径使用，且全程持有 g_audio_mutex，无并发问题。
 *       PSRAM 作为 i2s_channel_write 的源缓冲没问题：驱动内部会拷进 DMA 缓冲。
 */
static int16_t *s_stereo_buf = NULL;

/** @brief PA 功放使能（PCA9557 IO1） */
static void pa_set(bool on)
{
    const esp_err_t ret = pca9557_set_output_state(AUDIO_PA_PIN, on ? 1 : 0);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "设置 PA_EN 失败：%s", esp_err_to_name(ret));
    }
}

/**
 * @brief 写一段静音帧到 I2S
 * @param frames 帧数（任意长度：按 AUDIO_CHUNK_FRAMES 分块，不越过缓冲大小）
 * @note  必须分块：s_stereo_buf 只有 AUDIO_CHUNK_FRAMES 帧的容量，
 *        直接按 frames 计算 memset 长度会越界（曾导致 30 ms 尾部静音写满 5.7 KB
 *        而缓冲只有 4 KB，堆被破坏后 LVGL 触摸报错并触发 TLSF 断言复位）。
 */
static void write_silence(size_t frames)
{
    if (s_stereo_buf == NULL)
    {
        return;
    }
    memset(s_stereo_buf, 0, sizeof(int16_t) * 2 * AUDIO_CHUNK_FRAMES);
    while (frames > 0)
    {
        const size_t n = (frames > AUDIO_CHUNK_FRAMES) ? AUDIO_CHUNK_FRAMES : frames;
        size_t written = 0;
        (void)i2s_tx_write(s_stereo_buf, n * I2S_FRAME_BYTES, &written, AUDIO_IO_TIMEOUT_MS);
        frames -= n;
    }
}

/** @brief 开始播放前的准备：清 DMA、解除静音、开 PA */
static void playback_begin(void)
{
    /* 先清掉 DMA 里上一次的残留采样，否则开头会冒出一小段旧音频 */
    write_silence(AUDIO_CHUNK_FRAMES);

    const esp_err_t ret = es8311_set_mute(false);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "ES8311 解除静音失败：%s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    pa_set(true);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/** @brief 播放结束后的收尾：补静音、静音 DAC、关 PA */
static void playback_end(void)
{
    write_silence(I2S_SAMPLE_RATE / 1000 * AUDIO_TAIL_SILENCE_MS);
    (void)es8311_set_mute(true);
    vTaskDelay(pdMS_TO_TICKS(10));
    pa_set(false);
}

#if AUDIO_SELFTEST
/** @brief 自检任务栈（提示音缓冲走 PSRAM，栈只需放对数/临时变量） */
#define AUDIO_TEST_STACK (3072)
/** @brief 自检任务优先级 */
#define AUDIO_TEST_PRIO  (2)

/**
 * @brief 音频链路自检任务：系统起来 10 s 后“嘀”一声
 * @note  必须在独立任务里跑：在 app_main 里阻塞 10 s 会推迟人脸/网络初始化。
 *        听到 1 kHz 提示音即说明 I2S、ES8311、PA_EN 全链路正常。
 */
static void audio_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "[自检] 播放 1 kHz / 800 ms 提示音");
    const esp_err_t ret = audio_service_play_tone(1000, 800, 40);
    ESP_LOGI(TAG, "[自检] 提示音播放%s", ret == ESP_OK ? "完成" : "失败（见上方错误日志）");
    if (ret != ESP_OK)
    {
        vTaskDelete(NULL);
        return;
    }

    /* 第二阶段：验证 ES7210 录音链路。留 3 s 给提示音余音/PA 断电，避免录进自己的嘀声 */
    vTaskDelay(pdMS_TO_TICKS(8000));
    ESP_LOGI(TAG, "[自检] 录音 1.5 s 并回放（请对着麦克风说话）");
    const esp_err_t rec = audio_service_record_test(1500, true);
    ESP_LOGI(TAG, "[自检] 录音链路%s", rec == ESP_OK ? "正常" : "失败（见上方错误日志）");
    vTaskDelete(NULL);
}
#endif

/* ====================== 对外接口 ====================== */

esp_err_t audio_service_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }

    /* 初始化顺序有硬依赖：I2S 先送出 MCLK/BCLK/WS，两块编解码才能正常上电工作 */
    ESP_RETURN_ON_ERROR(i2s_driver_init(), TAG, "I2S 初始化失败");
    ESP_RETURN_ON_ERROR(es8311_codec_init(), TAG, "ES8311 初始化失败");

    /* 扬声器侧默认静音、PA 关闭：只有真正播放时才打开 */
    (void)es8311_set_mute(true);
    pa_set(false);

    /* ES7210（麦克风）失败不影响播放：录音是 M5 第二阶段的功能，只告警 */
    if (es7210_codec_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "ES7210 初始化失败（麦克风不可用，播放不受影响）");
    }

    /* 扩声道缓冲放 PSRAM（8 KB）；I2S TX DMA 从该缓冲逐块拷走，PSRAM 足够 */
    s_stereo_buf = (int16_t *)heap_caps_malloc(sizeof(int16_t) * AUDIO_CHUNK_SAMPLES,
                                               MALLOC_CAP_SPIRAM);
    if (s_stereo_buf == NULL)
    {
        /* 退一步用内部 RAM：PSRAM 偶发吃紧时不能“没有声音” */
        s_stereo_buf =
            (int16_t *)heap_caps_malloc(sizeof(int16_t) * AUDIO_CHUNK_SAMPLES, MALLOC_CAP_8BIT);
        if (s_stereo_buf == NULL)
        {
            ESP_LOGE(TAG, "扩声道缓冲分配失败（PSRAM 与内部 RAM 都不足）");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "扩声道缓冲退到内部 RAM（%u 字节）", (unsigned)sizeof(int16_t) *
                                                              AUDIO_CHUNK_SAMPLES);
    }

    s_volume = ES8311_VOICE_VOLUME;
    s_ready = true;

    if (g_audio_mutex == NULL)
    {
        ESP_LOGW(TAG, "g_audio_mutex 未创建（app_events_init 未调用？），并发播放不受保护");
    }

    ESP_LOGI(TAG, "音频服务就绪：%d Hz、音量 %d%%、PA_EN=PCA9557 IO%d", I2S_SAMPLE_RATE, s_volume,
             AUDIO_PA_PIN);

#if AUDIO_SELFTEST
    /* 自检必须异步：init 在 app_main 里调用，阻塞 10 s 会推迟整个启动流程 */
    if (xTaskCreatePinnedToCore(audio_selftest_task, "audio_test", AUDIO_TEST_STACK, NULL,
                                AUDIO_TEST_PRIO, NULL, 1) != pdPASS)
    {
        ESP_LOGW(TAG, "音频自检任务创建失败");
    }
#endif
    return ESP_OK;
}

bool audio_service_is_ready(void)
{
    return s_ready;
}

bool audio_service_is_busy(void)
{
    return s_busy;
}

esp_err_t audio_service_set_volume(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "音频服务未初始化");
    ESP_RETURN_ON_ERROR(es8311_set_volume(percent), TAG, "设置音量失败");
    s_volume = percent > 100 ? 100 : percent;
    ESP_LOGI(TAG, "音量设置为 %d%%", s_volume);
    return ESP_OK;
}

uint8_t audio_service_get_volume(void)
{
    return s_volume;
}

/**
 * @brief 播放核心：串行化 + PA 时序 + 按块扩立体声写 I2S
 * @param pcm        源采样（单声道 int16）
 * @param frames     源采样点数
 * @param ratio      输出采样率 / 源采样率（1 / 2 / 3，必须是整数）
 * @param timeout_ms 等锁超时
 *
 * @note  ratio > 1 时用**线性插值**上采样（16 kHz 素材 -> 48 kHz）。语音素材
 *        用线性插值足够，不要换成多相滤波：多相要额外系数表与状态，收益听不出来。
 *        每块源数据 AUDIO_CHUNK_FRAMES / ratio 点，扩展后不超过 s_stereo_buf 容量；
 *        块的最后一帧插值时用自身当“下一帧”，避免末尾出现向 0 的假斜坡。
 */
static esp_err_t play_mono_core(const int16_t *pcm, size_t frames, uint32_t ratio, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "音频服务未初始化");
    ESP_RETURN_ON_FALSE(pcm != NULL && frames > 0, ESP_ERR_INVALID_ARG, TAG, "播放参数为空");
    ESP_RETURN_ON_FALSE(ratio >= 1 && ratio <= 8, ESP_ERR_INVALID_ARG, TAG, "上采样比非法");

    /* 单请求在飞：并发播放串行化（语音播报与提示音可能同时来） */
    if (g_audio_mutex != NULL)
    {
        if (xSemaphoreTake(g_audio_mutex, timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) !=
            pdTRUE)
        {
            ESP_LOGW(TAG, "音频忙，放弃本次播放（%u 帧）", (unsigned)frames);
            return ESP_ERR_TIMEOUT;
        }
    }

    s_busy = true;
    if (g_sys_events != NULL)
    {
        xEventGroupSetBits(g_sys_events, SYS_EVENT_AUDIO_BUSY);
    }

    playback_begin();

    esp_err_t ret = ESP_OK;
    const size_t src_per_chunk = AUDIO_CHUNK_FRAMES / ratio;
    for (size_t off = 0; off < frames; off += src_per_chunk)
    {
        const size_t n = (frames - off > src_per_chunk) ? src_per_chunk : (frames - off);
        size_t out = 0;
        for (size_t i = 0; i < n; i++)
        {
            const int32_t s0 = pcm[off + i];
            const int32_t s1 = (off + i + 1 < frames) ? pcm[off + i + 1] : s0;
            const int32_t step = (s1 - s0) / (int32_t)ratio;
            for (uint32_t k = 0; k < ratio; k++)
            {
                const int16_t v = (int16_t)(s0 + step * (int32_t)k);
                s_stereo_buf[2 * out] = v;
                s_stereo_buf[2 * out + 1] = v;
                out++;
            }
        }
        size_t written = 0;
        const esp_err_t w = i2s_tx_write(s_stereo_buf, out * I2S_FRAME_BYTES, &written,
                                         AUDIO_IO_TIMEOUT_MS);
        if (w != ESP_OK)
        {
            ESP_LOGE(TAG, "I2S 写入失败：%s（已写 %u/%u 帧）", esp_err_to_name(w),
                     (unsigned)(off + written / I2S_FRAME_BYTES), (unsigned)frames);
            ret = w;
            break;
        }
    }

    playback_end();

    s_busy = false;
    if (g_sys_events != NULL)
    {
        xEventGroupClearBits(g_sys_events, SYS_EVENT_AUDIO_BUSY);
    }
    if (g_audio_mutex != NULL)
    {
        xSemaphoreGive(g_audio_mutex);
    }
    return ret;
}

esp_err_t audio_service_play_mono16(const int16_t *pcm, size_t frames, uint32_t timeout_ms)
{
    return play_mono_core(pcm, frames, 1, timeout_ms);
}

esp_err_t audio_service_play_mono16_rate(const int16_t *pcm, size_t frames, uint32_t src_rate_hz,
                                         uint32_t timeout_ms)
{
    if (src_rate_hz == 0 || I2S_SAMPLE_RATE % src_rate_hz != 0)
    {
        ESP_LOGW(TAG, "源采样率 %u 不能整除 %u，拒绝播放", (unsigned)src_rate_hz,
                 (unsigned)I2S_SAMPLE_RATE);
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t ratio = I2S_SAMPLE_RATE / src_rate_hz;
    return play_mono_core(pcm, frames, ratio, timeout_ms);
}

esp_err_t audio_service_play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t amplitude_pct)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "音频服务未初始化");
    ESP_RETURN_ON_FALSE(freq_hz >= 100 && freq_hz <= 10000 && duration_ms > 0 && duration_ms <= 5000,
                        ESP_ERR_INVALID_ARG, TAG, "提示音参数非法");

    const size_t frames = (size_t)I2S_SAMPLE_RATE * duration_ms / 1000;
    if (amplitude_pct > 100)
    {
        amplitude_pct = 100;
    }
    const int32_t amp = 32767 * amplitude_pct / 100;
    const size_t fade = I2S_SAMPLE_RATE / 100; /* 10 ms 淡入淡出 */

    /* 提示音整段生成在栈上会过大（800 ms ≈ 1.5 KB），改用 PSRAM 临时缓冲。
     * 不拿音频互斥量也行：临时缓冲只在本函数内有效，真正互斥在 play_mono16 里 */
    int16_t *buf = (int16_t *)heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL)
    {
        buf = (int16_t *)heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, TAG, "提示音缓冲分配失败");

    for (size_t i = 0; i < frames; i++)
    {
        const float ph = 2.0f * (float)M_PI * (float)freq_hz * (float)i / (float)I2S_SAMPLE_RATE;
        int32_t v = (int32_t)(sinf(ph) * (float)amp);
        /* 淡入淡出：避免开头结尾的阶跃产生“啪”声 */
        if (i < fade)
        {
            v = v * (int32_t)i / (int32_t)fade;
        }
        else if (i + fade >= frames)
        {
            v = v * (int32_t)(frames - i) / (int32_t)fade;
        }
        buf[i] = (int16_t)v;
    }

    const esp_err_t ret = audio_service_play_mono16(buf, frames, 2000);
    free(buf);
    return ret;
}

esp_err_t audio_service_record_test(uint32_t duration_ms, bool playback)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "音频服务未初始化");
    ESP_RETURN_ON_FALSE(duration_ms >= 200 && duration_ms <= 2000, ESP_ERR_INVALID_ARG, TAG,
                        "录音时长非法（200~2000 ms）");

    const size_t total_frames = (size_t)I2S_SAMPLE_RATE * duration_ms / 1000;
    /* 原始立体声交织数据：48 kHz x 1.5 s x 4 B ≈ 288 KB，放 PSRAM */
    int16_t *raw = (int16_t *)heap_caps_malloc(total_frames * I2S_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(raw != NULL, ESP_ERR_NO_MEM, TAG, "录音缓冲分配失败（PSRAM 不足）");

    /* 录音与播报共用同一组时钟/编解码电源，用 g_audio_mutex 串行化 */
    if (g_audio_mutex != NULL)
    {
        if (xSemaphoreTake(g_audio_mutex, pdMS_TO_TICKS(3000)) != pdTRUE)
        {
            ESP_LOGW(TAG, "音频忙，录音自检跳过");
            heap_caps_free(raw);
            return ESP_ERR_TIMEOUT;
        }
    }

    s_busy = true;
    if (g_sys_events != NULL)
    {
        xEventGroupSetBits(g_sys_events, SYS_EVENT_AUDIO_BUSY);
    }

    /* RX DMA 是环形缓冲，先丢干净，否则会先读到几毫秒前的旧数据 */
    (void)i2s_rx_drain(0);

    size_t got = 0; /* 已存帧数 */
    while (got < total_frames)
    {
        const size_t want = total_frames - got;
        const size_t n = (want > AUDIO_CHUNK_FRAMES) ? AUDIO_CHUNK_FRAMES : want;
        size_t done = 0;
        const esp_err_t r = i2s_rx_read(raw + got * 2, n * I2S_FRAME_BYTES, &done,
                                        AUDIO_IO_TIMEOUT_MS);
        if (r != ESP_OK || done == 0)
        {
            ESP_LOGW(TAG, "录音读失败：%s（已录 %u/%u 帧）", esp_err_to_name(r), (unsigned)got,
                     (unsigned)total_frames);
            break;
        }
        got += done / I2S_FRAME_BYTES;
    }

    /* 统计左右声道的 RMS 与峰值：ES7210 只接 1 路模拟麦，哪一声道有数据只能实测 */
    uint64_t sum_l = 0;
    uint64_t sum_r = 0;
    int32_t peak_l = 0;
    int32_t peak_r = 0;
    for (size_t i = 0; i < got; i++)
    {
        const int32_t l = raw[2 * i];
        const int32_t r = raw[2 * i + 1];
        sum_l += (uint64_t)(l * l);
        sum_r += (uint64_t)(r * r);
        if (l > peak_l)
        {
            peak_l = l;
        }
        if (-l > peak_l)
        {
            peak_l = -l;
        }
        if (r > peak_r)
        {
            peak_r = r;
        }
        if (-r > peak_r)
        {
            peak_r = -r;
        }
    }

    const bool use_right = (sum_r > sum_l);
    const double rms_l = got > 0 ? sqrt((double)sum_l / (double)got) : 0.0;
    const double rms_r = got > 0 ? sqrt((double)sum_r / (double)got) : 0.0;
    ESP_LOGI(TAG, "录音 %u 帧（%u ms）：L RMS=%.0f 峰值=%ld | R RMS=%.0f 峰值=%ld",
             (unsigned)got, (unsigned)(got * 1000 / I2S_SAMPLE_RATE), rms_l, (long)peak_l, rms_r,
             (long)peak_r);

    if ((use_right ? peak_r : peak_l) < 100)
    {
        ESP_LOGW(TAG, "录音几乎没有电平（峰值 <100）：检查 ES7210 增益/麦克风偏置或接线");
    }

    /* 把更响的那一路压成单声道（就地覆盖，写指针永远落后读指针） */
    if (use_right)
    {
        for (size_t i = 0; i < got; i++)
        {
            raw[i] = raw[2 * i + 1];
        }
    }
    else
    {
        for (size_t i = 0; i < got; i++)
        {
            raw[i] = raw[2 * i];
        }
    }

    s_busy = false;
    if (g_sys_events != NULL)
    {
        xEventGroupClearBits(g_sys_events, SYS_EVENT_AUDIO_BUSY);
    }
    if (g_audio_mutex != NULL)
    {
        xSemaphoreGive(g_audio_mutex);
    }

    esp_err_t ret = ESP_OK;
    if (playback && got > 0)
    {
        ESP_LOGI(TAG, "回放录到的 %s 声道", use_right ? "右" : "左");
        ret = audio_service_play_mono16(raw, got, 3000);
    }

    heap_caps_free(raw);
    return ret;
}
