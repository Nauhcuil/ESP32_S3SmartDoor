/**
 * @file voice_prompt.h
 * @brief 语音播报：内嵌 PCM 素材 + 缺失素材时的提示音兜底
 *
 * 功能说明：
 *   1. 固定短句以 16 kHz/16bit/单声道 PCM 素材嵌入固件，由 audio_service 播放；
 *   2. 素材缺失时自动播放提示音，业务代码不需要分支；
 *   3. 所有播报都串行在一个常驻任务（PSRAM 栈、静态 TCB）里，
 *      **调用方永不阻塞**：识别/开门发生在 face_task、命令词发生在
 *      voice_task（PSRAM 栈），两者都经不起阻塞或碰 Flash。
 *
 * 使用说明：
 *   - 初始化顺序：audio_service_init() → voice_assistant_init() →
 *     voice_prompt_init()；初始化会创建队列与常驻播报任务。
 *   - 播报期间 audio_service 会置 SYS_EVENT_AUDIO_BUSY，voice_task 据此丢弃
 *     麦克风输入并在播报后 reset，避免“听见自己”。
 *   - 文案是**固定的几条**（不拼接，不支持动态姓名）：人名/数字需要额外的文本
 *     合成能力，当前只播放内嵌短句素材。
 */

#ifndef __VOICE_PROMPT_H__
#define __VOICE_PROMPT_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief 播报文案编号（数组下标，见 voice_prompt.c 的 s_assets[]） */
typedef enum
{
    VOICE_PROMPT_WOKEN = 0,  /**< 唤醒后双音提示（p_hello.pcm） */
    VOICE_PROMPT_RECOGNIZING,/**< 请勿移动，正在识别 */
    VOICE_PROMPT_OPEN,       /**< 识别成功，门已打开 */
    VOICE_PROMPT_LIVENESS,   /**< 活体检测失败，禁止通行 */
    VOICE_PROMPT_UNKNOWN,    /**< 未识别到已注册人员 */
    VOICE_PROMPT_NO_FACE,    /**< 未检测到人脸，请正对摄像头 */
    VOICE_PROMPT_TOO_FAR,    /**< 请靠近一点 */
    VOICE_PROMPT_QUALITY,    /**< 请调整角度，光线太暗或画面模糊 */
    VOICE_PROMPT_ENROLL_OK,  /**< 注册成功 */
    VOICE_PROMPT_ENROLL_ERR, /**< 注册失败，请重试 */
    VOICE_PROMPT_DENIED,     /**< 陌生人，禁止通行 */
    VOICE_PROMPT_QUERY_LOG,  /**< 正在查询门禁记录 */
    VOICE_PROMPT_MENU,       /**< 已进入设置，请说命令词 */
    VOICE_PROMPT_VOL_UP,     /**< 音量已调大 */
    VOICE_PROMPT_VOL_DOWN,   /**< 音量已调小 */
    VOICE_PROMPT_CANCEL,     /**< 已取消 */
    VOICE_PROMPT_REMOTE,     /**< 远程开门成功 */
    VOICE_PROMPT_COUNT,      /**< 文案条数（哨兵） */
} voice_prompt_id_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化播报队列与常驻播报任务
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 队列或 PSRAM 栈分配失败
 *
 * @note 幂等。常驻任务使用静态 TCB 与 PSRAM 栈，避免运行期从内部堆动态创建任务。
 */
esp_err_t voice_prompt_init(void);

/**
 * @brief 播报一条固定文案（非阻塞）
 * @param id 文案编号
 * @return ESP_OK 已排队；ESP_ERR_INVALID_ARG 队列未就绪或文案编号非法
 *
 * @note 可在任意任务、任意锁内调用；同一条文案 1.5 s 内重复请求会被合并。
 */
esp_err_t voice_prompt_say(voice_prompt_id_t id);

/**
 * @brief 播报是否在途 / 播报队列是否可用（诊断页）
 */
bool voice_prompt_is_speaking(void);
bool voice_prompt_tts_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* __VOICE_PROMPT_H__ */
