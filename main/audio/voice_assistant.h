/**
 * @file voice_assistant.h
 * @brief 离线语音助手（M5）：I2S RX 唯一 owner，唤醒词 + 中文命令词识别
 *
 * 功能说明：
 *   1. voice_assistant_init() 创建 voice_task（Core0、优先级 4、栈 8 KB，**栈在 PSRAM**），
 *      由它独占 I2S RX 读数——audio_service 只写 TX，两者互不干扰；
 *   2. 数据流：
 *         I2S RX 48 kHz 立体声
 *           → 取单声道 + 3 倍抽取 → 16 kHz 单声道
 *           → esp-sr AFE（降噪/VAD/WakeNet9 “Hi,乐鑫”）
 *           → 唤醒后进入多轮命令：MultiNet7（mn7_cn）中文命令词
 *           → voice_assistant 派发到 face_service / net_service / ui_app
 *   3. 命令词**必须用拼音登记**（mn7_cn 的解码单元是拼音），见 voice_assistant.c
 *      的 s_commands[] / s_phonemes[]（9 条，命令 ID = 数组下标）；
 *   4. 播报期间（SYS_EVENT_AUDIO_BUSY 或 voice_assistant_set_input_paused(true)）
 *      不跑 AFE：麦克风会听见自己的扬声器，不屏蔽就会出现“播报把自己唤醒 /
 *      命令词误触发”；播报结束后 300 ms 恢复，同时丢弃这段时间积压在 RX DMA 里的
 *      音频并重置引擎。**暂停标志必须成对恢复**：voice_prompt 的播报线程在
 *      每条播报结束后清除它，漏清会让 voice_task 永久停在“只读不识别”，
 *      表现为“唤醒一次之后再怎么喊 Hi 乐鑫都没反应”。
 *
 * 使用说明：
 *   - 必须在 audio_service_init() 之后调用（I2S 已初始化），
 *     且在 face_service / door_controller / access_log 初始化之后（回调里要用）。
 *   - 模型来自 model 分区（partitions.csv），由 esp-sr 组件在 idf.py flash 时自动写入；
 *     sdkconfig 必须选 CONFIG_SR_WN_WN9_HILEXIN + CONFIG_SR_MN_CN_MULTINET7_QUANT。
 *   - 失败不阻塞启动：模型找不到/分区为空时只打印错误，返回 ESP_ERR_NOT_FOUND，
 *     其余功能照常运行。
 */

#ifndef __VOICE_ASSISTANT_H__
#define __VOICE_ASSISTANT_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 语音链路自检：按固定间隔打印“麦克风电平”，不做识别
 * @note  发布固件必须置 0。置 1 时 voice_task 每 2 s 打印一次 16 kHz 输入的
 *        RMS/峰值，用来在没有唤醒模型的情况下确认 ES7210 → 抽取 → AFE 输入正常。
 *        M5 实测输入正常（静默 RMS 30~90、说话 300+），诊断页“语音”行也已实时显示
 *        状态与电平，因此默认关闭，避免每 2 s 一条日志干扰唤醒词/命令词联调。
 */
#define VOICE_SELFTEST 0

/** @brief 唤醒词（WakeNet 模型里内置的词，这里只用于日志/界面提示） */
#define VOICE_WAKE_WORD "Hi,乐鑫"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化语音助手并启动 voice_task
 * @return ESP_OK 成功；ESP_ERR_NOT_SUPPORTED 未选模型；ESP_ERR_NOT_FOUND 分区里没有模型；
 *         ESP_ERR_INVALID_STATE 音频服务未就绪；ESP_ERR_NO_MEM 任务创建失败
 *
 * @note 幂等：重复调用返回 ESP_OK。
 */
esp_err_t voice_assistant_init(void);

/**
 * @brief 语音助手是否可用（模型加载成功、任务在跑）
 */
bool voice_assistant_is_ready(void);

/**
 * @brief 读一次 16 kHz 麦克风电平（供诊断页显示）
 * @param rms   输出 RMS（0~32767）
 * @param peak  输出峰值（0~32767）
 * @return true 有数据
 *
 * @note 由 voice_task 每处理一帧输入时更新，调用方不阻塞。
 */
bool voice_assistant_get_level(uint32_t *rms, uint32_t *peak);

/**
 * @brief 读取当前状态的中文说明（诊断页/主页状态卡）
 * @return "待唤醒" / "聆听命令" / "播报中，暂停" / "未就绪"
 */
const char *voice_assistant_state_text(void);

/**
 * @brief 最近一次识别到的命令词（UTF-8，无则空串）
 */
const char *voice_assistant_last_command(void);

/**
 * @brief 让 voice_task 在下一轮进入“本地输入校准”
 *
 * @note  **为什么要暂停**：扬声器播报的声音会被麦克风收回来，AFE 的
 *        `channel_monitor()`（运行期声道校正）只要连续 1.5 s 听到“较响且两路差 1.6 倍”
 *        就会判定声道反了并切换、`reset_engines()` 回待唤醒。播报若正好盖住一次
 *        真实喊话的窗口，就会出现“喊了没反应”。播报前调用本函数，voice_task 在
 *        播报结束 + 300 ms 内既不跑 AFE 也不做声道统计。
 */
void voice_assistant_set_input_paused(bool paused);

/**
 * @brief 请求语音任务清理识别上下文并回到待唤醒状态
 *
 * @note  只投递任务通知，不在调用线程操作 AFE/MultiNet；用于人脸录入完成后
 *        结束此前由“注册人脸”命令开启的命令聆听窗口。
 */
void voice_assistant_rearm_wake(void);

/**
 * @brief 设置音量（0~100）并播报一档音量提示
 * @param vol 目标音量，越界自动夹到 0~100
 * @return ESP_OK 已写入 ES8311；其他值为写寄存器失败
 *
 * @note  语音命令“声音大一点/小一点”与设置页的音量 +/- 共用本入口：
 *        写 ES8311、更新 audio_service 的音量缓存、按档位投递一条播报
 *        （≤0 → 音量已调小，≥70 → 音量已调大，其余 → 音量已调小）。
 *        非阻塞，可在任意任务调用。
 */
esp_err_t voice_assistant_set_volume_and_report(int vol);

#ifdef __cplusplus
}
#endif

#endif /* __VOICE_ASSISTANT_H__ */
