/**
 * @file audio_service.h
 * @brief 音频服务（M5）：I2S TX 的唯一 owner，负责提示音/语音播报的串行播放
 *
 * 功能说明：
 *   1. audio_service_init() 一次完成 I2S 全双工、ES8311（播放）、ES7210（录音）初始化；
 *   2. 对外提供“播放一段 16bit 单声道 PCM”的统一入口，内部负责：
 *      加锁 → 拉高 PA_EN(PCA9557 IO1) → ES8311 解除静音 → 单声道扩立体声写 I2S
 *      → 补尾部静音 → 静音 + 拉低 PA_EN → 清 AUDIO_BUSY；
 *   3. 任意任务都可以调用播放接口（阻塞式），并发调用被 g_audio_mutex 串行化。
 *
 * 使用说明：
 *   - 调用顺序：i2c_init() → pca9557_init() → app_events_init() → audio_service_init()；
 *   - 播放期间 SYS_EVENT_AUDIO_BUSY 置位，后续 voice 任务据此暂停唤醒词识别，
 *     避免“播报自己被麦克风听见 → 自触发”；
 *   - 采样率固定 48 kHz。提示音源文件如果是 16 kHz，需先重采样（M5 语音阶段处理）。
 */

#ifndef __AUDIO_SERVICE_H__
#define __AUDIO_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ====================== 编译期开关 ====================== */
/**
 * @brief 上电自检（M5 音频链路）：10 s 播放 1 kHz 提示音，18 s 录音 1.5 s 并回放
 * @note  发布固件必须置 0；调试期置 1 可在没有界面入口时验证
 *        I2S/ES8311/PA（听到“嘀——”一声即播放链路通）与 ES7210 录音链路。
 *        两条链路均已在实机验证通过（提示音可闻、录音 RMS≈57 峰值≈222），
 *        因此默认关闭：它每 8 s 占用一次 RX/PA，会干扰唤醒词与命令词联调。
 */
#define AUDIO_SELFTEST 0

/** @brief PA_EN 所在的 PCA9557 引脚号（IO1，高电平使能功放） */
#define AUDIO_PA_PIN    (1)

/* ====================== 函数声明（extern "C" 防止 C++ 名字修饰） ====================== */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化音频子系统（I2S 全双工 + ES8311 + ES7210 + PA 关闭）
 * @return esp_err_t ESP_OK 成功；任一步失败返回对应错误码（不阻塞其他功能）
 *
 * @note 幂等：重复调用直接返回 ESP_OK。
 */
esp_err_t audio_service_init(void);

/**
 * @brief 音频子系统是否就绪（初始化成功且编解码器可用）
 */
bool audio_service_is_ready(void);

/**
 * @brief 是否有音频正在播放
 */
bool audio_service_is_busy(void);

/**
 * @brief 设置播放音量
 * @param percent 0~100
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t audio_service_set_volume(uint8_t percent);

/**
 * @brief 读取当前播放音量
 * @return uint8_t 0~100（未初始化时返回默认值）
 */
uint8_t audio_service_get_volume(void);

/**
 * @brief 播放一段 16bit 单声道 PCM（48 kHz）
 * @param pcm        采样数据（小端、单声道）
 * @param frames     采样点数（不是字节数）
 * @param timeout_ms 等待其它播放结束的最长时间（毫秒），0 表示一直等
 * @return esp_err_t ESP_OK 播放完成；ESP_ERR_TIMEOUT 没等到；其余为 I2S 错误
 *
 * @note 内部把单声道复制成左右两声道。播放时阻塞调用者，长音频建议放到任务里播。
 */
esp_err_t audio_service_play_mono16(const int16_t *pcm, size_t frames, uint32_t timeout_ms);

/**
 * @brief 播放一段 16bit 单声道 PCM（**可指定源采样率**，内部线性插值上采样到 48 kHz）
 * @param pcm         采样数据（小端、单声道）
 * @param frames      采样点数（不是字节数）
 * @param src_rate_hz 源采样率，必须整除 48 kHz（16000 / 24000 / 48000）
 * @param timeout_ms  等待其它播放结束的最长时间（毫秒）
 * @return esp_err_t ESP_OK 播放完成；ESP_ERR_INVALID_ARG 采样率不支持；其余同 play_mono16
 *
 * @note 语音播报素材用 16 kHz（离线 TTS 生成的标准格式，体积只有 48 kHz 的三分之一），
 *       播放时按 3 倍线性插值补齐，听感与直接 48 kHz 素材无差别。
 */
esp_err_t audio_service_play_mono16_rate(const int16_t *pcm, size_t frames, uint32_t src_rate_hz,
                                         uint32_t timeout_ms);

/**
 * @brief 播放一段正弦提示音（内部生成，不占 Flash）
 * @param freq_hz         频率（Hz），建议 1000~2000
 * @param duration_ms     时长（毫秒）
 * @param amplitude_pct   幅度百分比 0~100（建议 30~50，太大容易破音）
 * @return esp_err_t ESP_OK 成功
 *
 * @note 首尾各 10 ms 线性淡入淡出，避免“啪”声爆音。
 */
esp_err_t audio_service_play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t amplitude_pct);

/**
 * @brief 录音链路自检：录一段声音、打印左右声道的 RMS/峰值，可选回放
 * @param duration_ms 录音时长（毫秒），允许 200~2000
 * @param playback    true 时把录到的那一路（选更响的一声道）从扬声器放出来
 * @return esp_err_t ESP_OK 成功
 *
 * @note 用途是**验证 ES7210 这一侧**（M5 第二阶段）：串口日志会出现
 *       “L RMS / R RMS / 峰值”，回放能听到自己的声音即说明
 *       ES7210 → I2S DIN(GPIO12) → 内存 → ES8311 → 功放 整条链路都通。
 *       ES7210 是 4 通道芯片，但本板只接了 1 路模拟麦，数据落在哪一声道
 *       由寄存器 0x12/0x11 的 I2S 格式决定——所以这里不假设声道，直接看日志。
 *       录制期间会持有 g_audio_mutex，与播报互斥；回放放在释放之后。
 */
esp_err_t audio_service_record_test(uint32_t duration_ms, bool playback);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_SERVICE_H__ */
