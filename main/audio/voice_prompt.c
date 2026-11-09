/**
 * @file voice_prompt.c
 * @brief 语音播报实现：内嵌离线语音片段（PCM 16 kHz/16bit/单声道）+ 常驻任务播放
 *
 * 设计要点（都是实机/代码约束逼出来的，改动前请先看这里）：
 *   1. **用离线素材而不是 ESP-TTS**：esp-sr 2.4.7 自带的 ESP-TTS 小乐音库
 *      链接进来要 ~4.5 MB 代码 + 2.9 MB 音库数据，而 factory 分区只剩 2.87 MB，
 *      必须改分区表；且首次合成要几十 KB 临时内存、在 WiFi 之后很容易失败。
 *      固定短句用离线素材：16 条合计约 0.65 MB，零运行时开销、发音稳定。
 *   2. **文案素材在 `main/audio/prompts/` 下的 p_*.pcm**，由 CMakeLists.txt 的
 *      EMBED_FILES 嵌入，生成链接符号 `_binary_p_xxx_pcm_start/end`。
 *      替换成真人录音时**保持 16 kHz / 16bit / 小端单声道裸 PCM**（无 WAV 头），
 *      文件名不变即可，无需改代码。
 *   3. **不要在调用线程里播放**：调用方只投递队列，一个长期运行的播报任务负责播放。
 *      任务使用静态 TCB、PSRAM 栈，避免运行期从已耗尽的内部堆动态创建任务。
 *   4. **一条一条播，不排队**：线程里循环“取一条 → play_mono16”。
 *      audio_service 内部按 1024 帧块扩立体声，缓冲在 PSRAM，不占线程栈。
 *   5. **排队深度 8 + 合并**：8 条足够；溢出时丢最旧的（宁可少说一句，
 *      也不要积压到几秒后突然说话）。1.5 s 内同一条文案合并，避免
 *      “未检测到人脸”随识别周期连说多遍。
 */

#include "voice_prompt.h"

#include <string.h>

#include "audio_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "voice_assistant.h"

static const char *TAG = "prompt";

/** @brief 播报任务栈（字节，PSRAM） */
#define PROMPT_TASK_STACK (3072)
/**
 * @brief 播报线程优先级（**必须高于 face_task 的 5**）
 *
 * @note  I2S TX 的 DMA 只有 4 x 240 帧 = 20 ms（见 i2s.c 的 dma_desc_num/dma_frame_num，
 *        内部 DMA RAM 太紧，加不深）。播报线程晚 20 ms 补不上下一块数据，DMA 就会
 *        在 auto_clear 下输出静音——这就是“语音播报有点卡顿”的根因：face_task 在
 *        Core0 上以优先级 5 每 200 ms 跑一次 20~70 ms 的推理，播报线程（原优先级 2）
 *        被整段挤掉，音频里就出现一截一截的空洞。
 *        提高到 6 之后播报线程只在 DMA 腾出空间的一瞬间被唤醒补数据，其余时间仍
 *        阻塞在 i2s_channel_write 里让出 CPU，不会拖慢人脸流水线。
 */
#define PROMPT_TASK_PRIO 6
/** @brief 文案队列深度 */
#define PROMPT_QUEUE_LEN 8
/** @brief 同一文案合并窗口（毫秒） */
#define PROMPT_MERGE_MS 1500
/** @brief 单条播放的最长等待（毫秒）：异常时不让线程永远卡住 */
#define PROMPT_PLAY_TIMEOUT_MS 15000
/** @brief 素材采样率：16 kHz 单声道 16bit（离线 TTS 生成，体积只有 48 kHz 的 1/3） */
#define PROMPT_SRC_RATE_HZ 16000

/* ====================== 内嵌素材（CMakeLists.txt EMBED_FILES 生成） ====================== */
/* 链接器符号：_binary_p_<名字>_pcm_start / _end */
extern const uint8_t _binary_p_hello_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_hello_pcm_end[];
extern const uint8_t _binary_p_recognizing_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_recognizing_pcm_end[];
extern const uint8_t _binary_p_open_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_open_pcm_end[];
extern const uint8_t _binary_p_liveness_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_liveness_pcm_end[];
extern const uint8_t _binary_p_unknown_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_unknown_pcm_end[];
extern const uint8_t _binary_p_no_face_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_no_face_pcm_end[];
extern const uint8_t _binary_p_too_far_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_too_far_pcm_end[];
extern const uint8_t _binary_p_quality_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_quality_pcm_end[];
extern const uint8_t _binary_p_enroll_ok_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_enroll_ok_pcm_end[];
extern const uint8_t _binary_p_enroll_err_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_enroll_err_pcm_end[];
extern const uint8_t _binary_p_denied_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_denied_pcm_end[];
extern const uint8_t _binary_p_query_log_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_query_log_pcm_end[];
extern const uint8_t _binary_p_menu_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_menu_pcm_end[];
extern const uint8_t _binary_p_vol_up_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_vol_up_pcm_end[];
extern const uint8_t _binary_p_vol_down_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_vol_down_pcm_end[];
extern const uint8_t _binary_p_cancel_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_cancel_pcm_end[];
extern const uint8_t _binary_p_remote_pcm_start[] __attribute__((aligned(4)));
extern const uint8_t _binary_p_remote_pcm_end[];

/** @brief 素材描述：起始指针 / 结束指针（字节）；空素材表示该编号不播 */
typedef struct
{
    const uint8_t *begin;
    const uint8_t *end;
    const char *text; /**< 日志用文案（与素材内容一致即可） */
} prompt_asset_t;

/** @brief 素材表（下标 = voice_prompt_id_t，顺序必须一致） */
static const prompt_asset_t s_assets[VOICE_PROMPT_COUNT] = {
    {_binary_p_hello_pcm_start, _binary_p_hello_pcm_end, "唤醒提示音"}, /* WOKEN */
    {_binary_p_recognizing_pcm_start, _binary_p_recognizing_pcm_end, "正在识别"},
    {_binary_p_open_pcm_start, _binary_p_open_pcm_end, "识别成功，门已打开"},
    {_binary_p_liveness_pcm_start, _binary_p_liveness_pcm_end, "活体检测失败"},
    {_binary_p_unknown_pcm_start, _binary_p_unknown_pcm_end, "未识别人员"},
    {_binary_p_no_face_pcm_start, _binary_p_no_face_pcm_end, "未检测到人脸"},
    {_binary_p_too_far_pcm_start, _binary_p_too_far_pcm_end, "请靠近一点"},
    {_binary_p_quality_pcm_start, _binary_p_quality_pcm_end, "请调整角度"},
    {_binary_p_enroll_ok_pcm_start, _binary_p_enroll_ok_pcm_end, "注册成功"},
    {_binary_p_enroll_err_pcm_start, _binary_p_enroll_err_pcm_end, "注册失败，请重试"},
    {_binary_p_denied_pcm_start, _binary_p_denied_pcm_end, "陌生人请离开"},
    {_binary_p_query_log_pcm_start, _binary_p_query_log_pcm_end, "正在查询记录"},
    {_binary_p_menu_pcm_start, _binary_p_menu_pcm_end, "已进入设置"},
    {_binary_p_vol_up_pcm_start, _binary_p_vol_up_pcm_end, "音量已调大"},
    {_binary_p_vol_down_pcm_start, _binary_p_vol_down_pcm_end, "音量已调小"},
    {_binary_p_cancel_pcm_start, _binary_p_cancel_pcm_end, "已取消"},
    {_binary_p_remote_pcm_start, _binary_p_remote_pcm_end, "远程开门成功"},
};

static QueueHandle_t s_queue = NULL;
static volatile bool s_speaking = false;
static StaticTask_t s_prompt_tcb;
static StackType_t *s_prompt_stack = NULL;
static TaskHandle_t s_prompt_task_handle = NULL;
/** @brief 每条文案最近一次入队的时间（合并窗口） */
static int64_t s_last_enqueue_ms[VOICE_PROMPT_COUNT];

/**
 * @brief 播放暂存缓冲（PSRAM）
 * @note  EMBED_FILES 生成的符号在 `.rodata.embedded` 段里**没有强制对齐**，
 *        直接 `(int16_t*)` 强转可能得到奇地址。这里 memcpy 到 PSRAM 的
 *        对齐缓冲（按最长素材一次分配，约 70 KB，复用）。
 * @warning **只在播报线程里第一次播放时才分配**，不能在初始化时预先分配：
 *          初始化阶段（WiFi 连上前后、esp-sr 建 AFE/MultiNet）内部 RAM 与堆的
 *          碎片状态决定成败，预先占一块、或者从这里退回内部 RAM 分配，
 *          都可能把 esp-sr 后续的小块申请（合法定时器等）挤到失败。
 */
static int16_t *s_stage = NULL;
static size_t s_stage_cap = 0; /**< 容量（采样点数） */

/** @brief 把素材拷进对齐缓冲；不够大就重新分配（PSRAM 分配失败则本次不播） */
static int16_t *stage_clip(const prompt_asset_t *a, size_t samples)
{
    if (samples > s_stage_cap)
    {
        int16_t *buf = (int16_t *)heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (buf == NULL)
        {
            ESP_LOGW(TAG, "暂存缓冲 %u 字节分配失败（PSRAM 不足）", (unsigned)(samples * sizeof(int16_t)));
            return NULL;
        }
        if (s_stage != NULL)
        {
            heap_caps_free(s_stage);
        }
        s_stage = buf;
        s_stage_cap = samples;
    }
    memcpy(s_stage, a->begin, samples * sizeof(int16_t));
    return s_stage;
}

/**
 * @brief 播报线程：循环取文案并播放
 * @param arg 未使用
 */
static void prompt_task(void *arg)
{
    (void)arg;
    voice_prompt_id_t id;
    /* 本线程是否正持有“语音输入暂停”标志。**必须成对清除**：
     * voice_assistant 只记录这个标志、不会自己超时恢复，漏清一次 voice_task 就
     * 永久停在 busy 分支——只读 I2S 不跑 AFE，现场表现正是“唤醒一次并执行命令
     * 之后，怎么喊 Hi 乐鑫都没反应”。（早期版本每条播报前置位、结束后忘了清，
     * 就是这个问题。） */
    while (true)
    {
        if (xQueueReceive(s_queue, &id, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if ((int)id < 0 || (int)id >= VOICE_PROMPT_COUNT)
        {
            continue;
        }
        const prompt_asset_t *a = &s_assets[id];
        const bool has_asset = a->begin != NULL && a->end != NULL && a->end > a->begin;
        const size_t bytes = has_asset ? (size_t)(a->end - a->begin) : 0;
        if (has_asset && bytes < 2)
        {
            continue;
        }

        /* 播报前暂停语音输入：否则 AFE 的运行期声道校正会把扬声器声音
         * 当成“麦克风在另一个声道”，1.5 s 内切换声道并 reset 回待唤醒，
         * 现场表现是“播报完喊一句话没反应”。
         * **播放结束必须同一条路径清掉**：不清的话标志永久
         * 置位，voice_task 从此只读不识别。 */
        voice_assistant_set_input_paused(true);
        s_speaking = true;
        const int16_t *pcm = has_asset ? stage_clip(a, bytes / 2) : NULL;
        const esp_err_t err =
            has_asset
                ? (pcm == NULL
                       ? ESP_ERR_NO_MEM
                       : audio_service_play_mono16_rate(pcm, bytes / 2, PROMPT_SRC_RATE_HZ,
                                                        PROMPT_PLAY_TIMEOUT_MS))
                : audio_service_play_tone(1500, 150, 40);
        s_speaking = false;
        voice_assistant_set_input_paused(false);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "播放失败（%s）：%s", a->text, esp_err_to_name(err));
        }
        else
        {
            if (has_asset)
            {
                ESP_LOGI(TAG, "播报：%s（%u ms）", a->text,
                         (unsigned)((bytes / 2) * 1000 / PROMPT_SRC_RATE_HZ));
            }
            else
            {
                ESP_LOGW(TAG, "提示音素材缺失（%s），已播放蜂鸣提示", a->text);
            }
        }
    }
}

esp_err_t voice_prompt_init(void)
{
    if (s_queue == NULL)
    {
        s_queue = xQueueCreate(PROMPT_QUEUE_LEN, sizeof(voice_prompt_id_t));
        if (s_queue == NULL)
        {
            ESP_LOGE(TAG, "播报队列创建失败");
            return ESP_ERR_NO_MEM;
        }
        memset(s_last_enqueue_ms, 0, sizeof(s_last_enqueue_ms));
    }

    if (s_prompt_task_handle == NULL)
    {
        s_prompt_stack =
            (StackType_t *)heap_caps_malloc(PROMPT_TASK_STACK,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_prompt_stack == NULL)
        {
            ESP_LOGE(TAG, "播报任务 PSRAM 栈分配失败（需要 %u 字节）",
                     (unsigned)PROMPT_TASK_STACK);
            return ESP_ERR_NO_MEM;
        }
        s_prompt_task_handle =
            xTaskCreateStaticPinnedToCore(prompt_task, "voice_prompt",
                                          PROMPT_TASK_STACK / sizeof(StackType_t), NULL,
                                          PROMPT_TASK_PRIO, s_prompt_stack, &s_prompt_tcb, 0);
        if (s_prompt_task_handle == NULL)
        {
            heap_caps_free(s_prompt_stack);
            s_prompt_stack = NULL;
            ESP_LOGE(TAG, "播报静态任务创建失败");
            return ESP_ERR_NO_MEM;
        }
    }

    size_t total = 0;
    for (int i = 0; i < VOICE_PROMPT_COUNT; i++)
    {
        if (s_assets[i].begin != NULL && s_assets[i].end != NULL &&
            s_assets[i].end > s_assets[i].begin)
        {
            total += (size_t)(s_assets[i].end - s_assets[i].begin);
        }
    }
    ESP_LOGI(TAG, "语音播报已就绪：%d 条离线素材，合计 %u 字节（16 kHz 单声道）",
             VOICE_PROMPT_COUNT, (unsigned)total);
    return ESP_OK;
}

esp_err_t voice_prompt_say(voice_prompt_id_t id)
{
    if (s_queue == NULL || (int)id < 0 || (int)id >= VOICE_PROMPT_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t now = esp_timer_get_time() / 1000;
    if (now - s_last_enqueue_ms[id] < PROMPT_MERGE_MS)
    {
        return ESP_OK; /* 合并 */
    }
    s_last_enqueue_ms[id] = now;

    /* 非阻塞投递；满了丢最旧的一条，再放新的 */
    if (xQueueSend(s_queue, &id, 0) != pdTRUE)
    {
        voice_prompt_id_t old;
        (void)xQueueReceive(s_queue, &old, 0);
        (void)xQueueSend(s_queue, &id, 0);
    }
    return ESP_OK;
}

bool voice_prompt_is_speaking(void)
{
    return s_speaking;
}

bool voice_prompt_tts_ready(void)
{
    /* 离线素材模式下恒为“可用”：没有合成器，素材是否为空看 s_assets */
    return s_queue != NULL;
}
