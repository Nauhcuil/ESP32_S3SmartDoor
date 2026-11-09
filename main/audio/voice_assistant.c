/**
 * @file voice_assistant.c
 * @brief 离线语音助手实现（M5）：I2S RX → 3 倍抽取 → esp-sr AFE → 唤醒 → MultiNet 命令词
 *
 * 数据流与关键参数：
 *   - I2S RX 48 kHz/16bit/立体声（ES7210 输出）。esp-sr 的 AFE 只接受
 *     **16 kHz 单声道 int16**，所以每帧立体声只取一路、每 3 帧取 1 帧：
 *     48 kHz / 3 = 16 kHz。取哪一路（L/R）在启动时由 300 ms 录音的 RMS 自动判定，
 *     因为本板只接了 1 路模拟麦，ES7210 把它放在哪个声道取决于寄存器 0x11 的格式。
 *   - AFE：AFE_TYPE_SR + AFE_MODE_LOW_COST，内存分配 AFE_MEMORY_ALLOC_MORE_PSRAM
 *     （M4 之后内部 RAM 只剩 ~10 KB，AFE 若优先吃内部 RAM 会让界面线程无法启动）。
 *   - 两段状态机：
 *       待唤醒 →（WakeNet9 “Hi,乐鑫” 命中）→ 聆听命令（MultiNet，10 s 超时）
 *       → 命中命令词 → 派发 → 回待唤醒；超时或播报期间被打断 → 回待唤醒。
 *
 * 并发与资源：
 *   - voice_task 是 I2S RX 的唯一读者；audio_service 是 TX 的唯一写者，互不冲突。
 *   - 播报期间（SYS_EVENT_AUDIO_BUSY）仍持续读 RX（否则 DMA 溢出），但只丢弃、
 *     不做识别；播报结束后 300 ms 恢复并重置 AFE/MultiNet 缓冲。
 *   - 模型（约 3.0 MB）位于 model 分区，esp_srmodel_init() 用 mmap 只读访问，
 *     不占 PSRAM 堆。所有工作缓冲都放在 PSRAM。
 *
 * @note 命令派发一律**不阻塞**：语音“打开门禁”只是代替按键按一下识别键，
 *       真正的人脸识别 → 活体 → 开门仍由 face_task 走正常流水线；
 *       查询记录/跳页面只调 UI 接口，网络状态只读缓存的摘要字符串。
 */

#include "voice_assistant.h"

#include <math.h>
#include <string.h>

#include "app_events.h"
#include "audio_service.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_afe_sr_models.h"
#include "esp_timer.h"
#include "face_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h" /* xTaskCreatePinnedToCoreWithCaps（PSRAM 栈） */
#include "freertos/task.h"
#include "i2s.h"
#include "model_path.h"
#include "net_service.h"
#include "ui_app.h"
#include "ui_home.h"
#include "ui_strings.h"
#include "voice_prompt.h"

/** 日志标签 */
static const char *TAG = "voice";

/** @brief 抽取比：48 kHz → 16 kHz */
#define VOICE_DECIMATE        (I2S_SAMPLE_RATE / 16000)
/**
 * @brief voice_task 栈大小
 * @note  栈从 **PSRAM** 申请（xTaskCreatePinnedToCoreWithCaps + MALLOC_CAP_SPIRAM）。
 *        M5 实测：WiFi 初始化后内部空闲只剩 ~7.7 KB、最大块 7.1 KB，
 *        8 KB 内部栈必然建不出来（日志“voice_task 创建失败”，连音都不会出错，
 *        直接是 xTaskCreate 返回 pdFAIL）。PSRAM 栈需要
 *        CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y（默认开启），
 *        Flash 操作期间该任务会随缓存关闭一起挂起，本任务只做 I2S 读数 +
 *        AFE/MultiNet 调用，不落 Flash，安全。峰值栈水位 ~2 KB，8 KB 余量充足。
 */
#define VOICE_TASK_STACK      (8192)
/** @brief voice_task 优先级（低于 face_task 的 5、高于 LVGL 的 1） */
#define VOICE_TASK_PRIO       (4)
#define VOICE_TASK_NOTIFY_REARM_WAKE (1UL << 0)
/** @brief 每次从 I2S 读的帧数：1920 帧 = 40 ms @48 kHz → 抽取后 640 点 @16 kHz */
#define VOICE_RX_FRAMES       (1920)
/** @brief I2S 读超时 */
#define VOICE_RX_TIMEOUT_MS   (100)
/** @brief 命令词聆听窗口（毫秒）：唤醒后这么久没有命中就回待唤醒 */
#define VOICE_CMD_WINDOW_MS   (10000)
/**
 * @brief 唤醒/命中后“暂不采信” MultiNet 结果的时间（毫秒）
 *
 * @note  必须保留这段静默：AFE 的环形缓冲里还留着唤醒词之前的音频（含唤醒词本身），
 *        fetch 出来的前几块并不是“唤醒之后说的话”。实测唤醒后 0.5 s 内就命中过
 *        一条“打开门”（用户根本没说完命令），直接触发人脸识别开门——这就是
 *        “喊完 Hi 乐鑫就自己开门”的根因。reset_engines() 清掉环形缓冲后，
 *        再用这个窗口把残留的几块也丢掉，之后才开始采信。
 */
#define VOICE_CMD_ARM_MS      (400)
/**
 * @brief 同一条命令的最短重复间隔（毫秒）
 * @note  防“同一段音频在同一次聆听里被认出两次”；真人再说一遍通常 ≥1 s。
 */
#define VOICE_CMD_REPEAT_MS   (1200)
/**
 * @brief 语音“开门”的硬限速（毫秒）
 *
 * @note  现场故障：MultiNet 在底噪上每秒命中一次“打开门”，每次都会触发一轮人脸识别
 *        并（原来）响一声提示音，喇叭“嘀嘀嘀”不停、门禁状态反复切换。识别闸门与
 *        VAD 语音帧证据（守卫 v5）是第一道过滤，但底噪误判的表现形式不稳定，而
 *        “连续开门”的代价最大，所以这里再加一道与内容无关的硬限速：
 *        8 s 内最多触发一次语音开门。真人重复说“打开门”一般也不会快过这个间隔。
 */
#define VOICE_OPEN_COOLDOWN_MS (8000)
/**
 * @brief MultiNet 结果的最低置信度（**参考量，不参与放行判断**）
 *
 * @note  原设计用它挡噪声，但实测 mn7_cn 返回的 prob[0] 恒为 0（见 mn_step 的
 *        守卫 v5 说明），这条判据既挡不住噪声、又会把真人命令一起挡掉。
 *        现在只在日志里与 VOICE_MIN_PROB 对照打印，便于以后换模型时复查；
 *        是否采信由“输入侧 VAD 语音帧证据”决定。
 */
#define VOICE_MIN_PROB        (0.50f)
/** @brief 播报结束后多久恢复识别（毫秒）：给功放余响留时间 */
#define VOICE_RESUME_DELAY_MS (300)
/** @brief 启动时判定“哪一声道有麦克风”的录音时长 */
#define VOICE_PROBE_MS        (300)
/**
 * @brief 运行期声道校正窗口（毫秒）
 * @note  启动探测只录 300 ms，安静时两路 RMS 几乎相同（实测 63 / 63），选哪一路
 *        完全是抛硬币——选错就永远唤不醒。因此运行期持续统计两路能量，
 *        某个窗口内一路明显更响就自动切过去。窗口取 1.5 s：足够覆盖一个词。
 */
#define VOICE_CH_WINDOW_MS    (1500)
/** @brief 运行期声道校正：较响一路必须是另一路的多少倍（含噪声波动余量） */
#define VOICE_CH_RATIO        (1.6)
/**
 * @brief 运行期声道校正：峰值低于该值视为静默/底噪，不参与判断
 * @note  实测本板底噪 RMS≈300（增益 30 dB），单看 RMS 会把底噪当“有声音”；
 *        说话时峰值轻松过 3000~8000，底噪峰值约 800~1200，所以用峰值做门限。
 */
#define VOICE_CH_PEAK_MIN     (2000)
/**
 * @brief 识别帧的闸门：只在“听见语音”的时间段里喂 MultiNet、采信其结果
 *
 * @note  现场故障：喊完“Hi,乐鑫”后**一句话都没说**，设备却每分钟 ~10 次报
 *        “命令词识别：id=1 打开门（拼音 ）”，每次都触发一轮人脸识别并开门，60 s 以上不停。
 *        三条线索同时成立：派发间隔超过 VOICE_CMD_REPEAT_MS（1.2 s）、聆听窗口
 *        （10 s + 0.5 s 兜底）从未超时、命中拼音串为空。说明 MultiNet 在**纯静音输入**上
 *        被反复置 DETECTED（底噪经 AGC 放大后喂进网络，状态机反复“命中”）。
 *        所以不能只看 MultiNet 的判定，还要看这帧输入本身：
 *          - 16 kHz 输入峰值 ≥ VOICE_MN_GATE_PEAK_MIN（底噪峰值约 800~1200，说话轻松过 3000）；
 *          - AFE 的 VAD 认为本帧是“有人说话”（VAD_SPEECH）。
 *        没有语音时**既不给 MultiNet，也不读它的结果**，但照常 drain 掉 AFE 输出，
 *        避免队列溢出；聆听窗口照常计时（不重置），到点自然回待唤醒。
 *        宁可漏检（要求用户对着麦克风正常音量说话），也不能在静音下自己开门。
 */
#define VOICE_MN_GATE_PEAK_MIN      (1500) /**< 16 kHz 输入峰值门限（底噪约 800~1200） */
#define VOICE_MN_WINDOW_MS          (3000) /**< 闸门统计日志的窗口长度 */
#define VOICE_MN_GRACE_MS           (2000) /**< 最后一帧语音之后仍可采信时长（覆盖命令词收尾） */
/**
 * @brief 命中一条命令词前，自本轮开始至少要累计的“VAD 判说话且过门限”帧数
 *
 * @note  一帧 AFE 输出 512 点 = 32 ms，普通命令取 2 帧 ≈ 64 ms，降低轻声或短促
 *        命令漏识别；开门命令仍要求 3 帧 ≈ 100 ms。门限仍要求 VAD 判说话且输入
 *        峰值过线，以免单靠噪声触发。
 *        闸门本身是逐帧判断的，某几帧因瞬态噪声误开时，MultiNet 仍可能凭这些
 *        帧“凑”出一个 command_id；加上这条累计证据后，必须**先有人说了一段话**，
 *        才可能采信任何结果。
 */
#define VOICE_MN_MIN_SPEECH_FRAMES  (2)
/** @brief 开门命令保留更严格的语音证据，避免降低普通命令门限后误触发 */
#define VOICE_MN_OPEN_MIN_SPEECH_FRAMES (3)
/** @brief 音量命令单步变化 */
#define VOICE_VOLUME_STEP     (10)
/* 音量的统一入口见文件末尾的对外接口（voice_assistant_set_volume_and_report） */
/** @brief AFE 输出 -> MultiNet 的拼接缓冲最大点数（约 1 s） */
#define VOICE_MN_QUEUE        (16384)

/**
 * @brief 命令词表（MultiNet 词条；命令 ID = 数组下标）
 * @note 词条尽量短：MultiNet 对 3~5 字短语的识别率明显高于长句。
 *       “今天有谁来过”已按需求移除（该命令要读 SPIFFS 日志，且语音播报能力不足）；
 *       增删命令类别后 ID **整体顺延**，switch 分支与拼音表必须同步改，
 *       否则会出现“说 A 触发 B”。同一 ID 可登记多个拼音说法；启动日志中每个
 *       phrase 都应映射到预期的 Command ID。
 */
static const char *const s_commands[] = {
    UI_TXT_VOICE_CMD_OPEN,     /* 0 打开门禁：走人脸识别流水线（语音代替按键） */
    UI_TXT_VOICE_CMD_OPEN2,    /* 1 打开门：同上（同义短句，提高识别率） */
    UI_TXT_VOICE_CMD_LOG,      /* 2 查询记录：跳到门禁记录页 */
    UI_TXT_VOICE_CMD_ENROLL,   /* 3 注册人脸：跳到注册页 */
    UI_TXT_VOICE_CMD_NET,      /* 4 网络状态 */
    UI_TXT_VOICE_CMD_VOL_UP,   /* 5 声音大一点 */
    UI_TXT_VOICE_CMD_VOL_DOWN, /* 6 声音小一点 */
    UI_TXT_VOICE_CMD_CANCEL,     /* 7 取消操作 */
    UI_TXT_VOICE_CMD_SETTINGS,   /* 8 打开设置：跳到设备设置页 */
};

/** @brief 命令词条数 */
#define VOICE_CMD_NUM         ((int)(sizeof(s_commands) / sizeof(s_commands[0])))

/**
 * @brief 命令词对应的拼音（无调号、空格分隔）
 *
 * @note **mn7_cn 必须走拼音添加**（`esp_mn_commands_add(id, 拼音)`）：实测
 *       `check_speech_command()` 对中文 UTF-8 字符串一律返回 0（“打开门禁”
 *       “打开空调”都判为 invalid，日志 `invalid command, please check format`），
 *       只有空格分隔的拼音返回 1。原因是中文 MultiNet7 的解码单元就是拼音；
 *       英文 mn7_en 用 flite_g2p 自动转音素，中文没有内置 g2p，只能自己给拼音。
 *       **不要再调 `esp_mn_commands_phoneme_add()`**：该函数只在英文 mn7 下校验
 *       音素，中文模型走的是 grapheme 分支，校验的还是中文串——照样被拒，
 *       只会白打 9 条 “invalid command” 日志。中文串 s_commands[] 只用于日志
 *       与界面回显（识别结果拿 command_id 反查，不依赖返回的拼音字符串）。
 */
static const char *const s_phonemes[] = {
    "da kai men jin",         /* 0 打开门禁 */
    "da kai men",             /* 1 打开门 */
    "cha xun ji lu",          /* 2 查询记录 */
    "zhu ce ren lian",        /* 3 注册人脸 */
    "wang luo zhuang tai",    /* 4 网络状态 */
    "sheng yin da yi dian",   /* 5 声音大一点 */
    "sheng yin xiao yi dian", /* 6 声音小一点 */
    "qu xiao cao zuo",        /* 7 取消操作 */
    "da kai she zhi",         /* 8 打开设置 */
};

/** @brief 同一命令 ID 的常用说法别名，不增加命令类别 */
static const struct
{
    int command_id;
    const char *phoneme;
} s_command_aliases[] = {
    {2, "cha kan ji lu"},      /* 查看记录 */
    {3, "kai shi zhu ce"},     /* 开始注册 */
    {4, "wang luo qing kuang"},/* 网络情况 */
    {5, "yin liang jia da"},   /* 音量加大 */
    {5, "yin liang jia"},      /* 音量加 */
    {6, "yin liang jian xiao"},/* 音量减小 */
    {6, "yin liang jian"},     /* 音量减 */
    {8, "jin ru she zhi"},     /* 进入设置 */
};

/** @brief 运行状态 */
typedef enum
{
    VOICE_STATE_WAKE = 0, /**< 待唤醒 */
    VOICE_STATE_CMD,      /**< 聆听命令 */
} voice_state_t;

/* ====================== 模型与引擎 ====================== */

/** @brief 模型列表（model 分区，mmap） */
static srmodel_list_t *s_models = NULL;
/** @brief AFE 引擎句柄 */
static const esp_afe_sr_iface_t *s_afe = NULL;
/** @brief AFE 实例 */
static esp_afe_sr_data_t *s_afe_data = NULL;
/** @brief MultiNet 引擎句柄 */
static esp_mn_iface_t *s_mn = NULL;
/** @brief MultiNet 实例 */
static model_iface_data_t *s_mn_data = NULL;

/** @brief 引擎参数（初始化时查询，避免每轮重复调用） */
static int s_feed_chunk = 0;  /**< AFE 每次 feed 的点数 @16 kHz */
static int s_fetch_chunk = 0; /**< AFE 每次 fetch 的点数 @16 kHz */
static int s_mn_chunk = 0;    /**< MultiNet 每次 detect 的点数 @16 kHz */

/* ====================== 工作缓冲（PSRAM） ====================== */

static int16_t *s_rx_buf = NULL;    /**< I2S 原始立体声 VOICE_RX_FRAMES x 2 */
static int16_t *s_feed_buf = NULL;  /**< AFE 输入 s_feed_chunk 点 */
static int16_t *s_fetch_buf = NULL; /**< AFE 输出 s_fetch_chunk 点（单块） */
static int16_t *s_mn_buf = NULL;    /**< MultiNet 输入 s_mn_chunk 点 */
static int16_t *s_mn_queue = NULL;  /**< AFE 输出 -> MultiNet 的拼接队列 VOICE_MN_QUEUE 点 */
static int s_feed_fill = 0;         /**< s_feed_buf 已填点数 */
static int s_mn_q_read = 0;         /**< s_mn_queue 读指针 */
static int s_mn_q_write = 0;        /**< s_mn_queue 写指针 */

/* ====================== 运行状态 ====================== */

static volatile bool s_ready = false;
static TaskHandle_t s_voice_task_handle = NULL;
static volatile voice_state_t s_state = VOICE_STATE_WAKE;
static volatile bool s_paused = false;
static volatile int64_t s_state_since_ms = 0; /**< 进入当前状态的时间 */
static int s_mic_channel = 0;                 /**< 麦克风所在声道：0=左 1=右 */
/** @brief 进入命令聆听后多早开始采信 MultiNet 结果（毫秒，见 VOICE_CMD_ARM_MS） */
static volatile int64_t s_cmd_arm_ms = 0;
/** @brief 上一次派发命令的时间（毫秒，用于 VOICE_CMD_REPEAT_MS 去重） */
static int64_t s_last_dispatch_ms = 0;
/** @brief 上一次派发的命令 ID（-1 = 无） */
static int s_last_dispatch_id = -1;
/** @brief 上一次语音开门的时间（毫秒，用于 VOICE_OPEN_COOLDOWN_MS 限速） */
static int64_t s_last_open_ms = 0;
/**
 * @brief 本轮唤醒后成功派发过的命令条数
 * @note  用于“未听清”提示：只在一次唤醒里**一条命令都没派发出去**时才显示，
 *        否则先说“网络状态”再说一句没识别出来的话，屏幕会把刚才的结果盖成
 *        “未听清”，看起来像什么都没识别到。进入 CMD 状态时清零。
 */
static int s_cmd_dispatched = 0;
/**
 * @brief 输入暂停请求（播报期间置位）
 * @note  由 voice_prompt 在每条播报前后置/清。置位时 voice_task 照常读 RX（不读会让
 *        DMA 溢出）但丢弃数据、不跑 AFE、也不做声道统计——扬声器的声音被麦克风
 *        收回来会让运行期声道校正误判“声道反了”并 reset 回待唤醒。
 *        清除后与音频忙结束一样：drain + reset_engines。
 */
static volatile bool s_input_paused = false;
/* 运行期声道校正的累计量（每 VOICE_CH_WINDOW_MS 结算一次） */
static uint64_t s_ch_sum_l = 0;   /**< 窗口内左声道平方和 */
static uint64_t s_ch_sum_r = 0;   /**< 窗口内右声道平方和 */
static int32_t s_ch_peak_l = 0;   /**< 窗口内左声道峰值 */
static int32_t s_ch_peak_r = 0;   /**< 窗口内右声道峰值 */
static int64_t s_ch_points = 0;   /**< 窗口内累计点数 */
static int64_t s_ch_since_ms = 0; /**< 窗口起点 */
static volatile uint32_t s_level_rms = 0;     /**< 16 kHz 输入 RMS */
static volatile uint32_t s_level_peak = 0;    /**< 16 kHz 输入峰值 */
/** @brief 最近一次命令词（诊断/状态卡读取，最长 63 字节） */
static char s_last_cmd[64] = "";
/** @brief 最近一次命中的拼音串（res->raw_string 优先，空则用 res->string，仅供日志） */
static char s_hit_raw[64] = "";
/** @brief 最近一次命中的置信度（res->prob[0]） */
static float s_hit_prob = 0.0f;

// ---- 识别帧闸门（见 VOICE_MN_GATE_PEAK_MIN 说明）----
/** @brief 当前语音活动窗口的起点（毫秒；每轮窗口取 VOICE_MN_WINDOW_MS） */
static int64_t s_mn_win_since_ms = 0;
/** @brief 本轮窗口内“有声音且 VAD 判为说话”的 AFE 帧数 */
static int s_mn_win_speech = 0;
/** @brief 本轮窗口内“有声音”（峰值过门限）的 AFE 帧数 */
static int s_mn_win_loud = 0;
/** @brief 本轮窗口是否已有过语音活动（决定 AFE 输出是否喂 MultiNet） */
static bool s_mn_win_active = false;
/** @brief 本轮窗口内最后一帧语音的时刻（毫秒）：之后 VOICE_MN_GRACE_MS 内仍可采信 */
static int64_t s_mn_last_speech_ms = 0;
/** @brief 本轮窗口的输入峰值（窗口统计日志用） */
static int s_mn_prev_peak = 0;
/**
 * @brief 自上次 mn_gate_reset() 以来累计的“有声音且 VAD 判说话”帧数
 *
 * @note  窗口计数每 VOICE_MN_WINDOW_MS 清零一次，不能用来判断“这条命令前面到底
 *        有没有人说话”。这里单独累计，命中时要求 ≥ VOICE_MN_MIN_SPEECH_FRAMES，
 *        即命令词之前必须有一段真实语音；纯底噪/提示音残响凑不出这么多帧。
 */
static int s_mn_speech_frames = 0;

/* ====================== 内部辅助 ====================== */

/** @brief 识别闸门重置（定义在后面，供 enter_state 使用） */
static void mn_gate_reset(void);

/**
 * @brief 播放一条提示音并同步主界面状态栏
 * @param text 状态栏文字（UI_TXT_*）
 *
 * @note 提示音只有 150 ms，可以让 voice_task 短暂阻塞；播放期间
 *       SYS_EVENT_AUDIO_BUSY 置位，下一轮循环会看到并暂停识别，不会自触发。
 *       **播报结束后必须清掉 RX DMA**：play_tone 是同步的（置位→播放→清位
 *       全在一次调用内完成），主循环很可能根本看不到 busy 位，也就不会走
 *       “播报恢复后 drain”的分支；而 150 ms 的喇叭声 + 回声远超 RX 的 20 ms
 *       DMA 缓冲，不清掉就会原样灌进 AFE，表现为“唤醒后立刻误识别/听不清”。
 */
static void speak_cue(const char *text)
{
    ui_app_set_status(text, 0x2F6FED);
    (void)audio_service_play_tone(1500, 150, 40);
    (void)i2s_rx_drain(0);
    /* 唤醒后紧接着播提示音。由常驻播报线程播放，本任务不等它；
     * 播报期间 voice_task 看到 s_input_paused 只读不识别，播完自动恢复 */
    (void)voice_prompt_say(VOICE_PROMPT_WOKEN);
}

/**
 * @brief 进入某个状态并记录时间，同时清掉各段半截缓冲
 * @note  打印状态迁移：排查“唤醒后自己开门 / 说别的命令没反应”时，
 *        日志里要能看出每一次 WAKE↔CMD 切换的时机与原因。
 */
static void enter_state(voice_state_t st)
{
    if (s_state != st)
    {
        ESP_LOGI(TAG, "语音状态 %s → %s",
                 s_state == VOICE_STATE_CMD ? "聆听命令" : "待唤醒",
                 st == VOICE_STATE_CMD ? "聆听命令" : "待唤醒");
    }
    s_state = st;
    s_state_since_ms = esp_timer_get_time() / 1000;
    s_feed_fill = 0;
    s_mn_q_read = 0;
    s_mn_q_write = 0;
    mn_gate_reset();
}

/** @brief 重置两个模型的状态机（播报后恢复、唤醒后清上下文） */
static void reset_engines(void)
{
    if (s_afe != NULL && s_afe_data != NULL)
    {
        (void)s_afe->reset_buffer(s_afe_data);
    }
    if (s_mn != NULL && s_mn_data != NULL)
    {
        (void)s_mn->clean(s_mn_data);
    }
    s_feed_fill = 0;
    s_mn_q_read = 0;
    s_mn_q_write = 0;
}

/**
 * @brief 判定麦克风在哪一路声道
 * @return 0 = 左，1 = 右
 *
 * @note 本板 ES7210 只接 1 路模拟麦，用 300 ms 录音的能量比较来判定，
 *       比硬编码寄存器位更可靠（换板/换固件都不用改代码）。失败时默认左声道。
 *       **这只是初值**：安静时两路噪声电平接近（实测 L/R RMS≈63），结果不可靠，
 *       运行期由 channel_monitor() 在有声音时自动校正。
 */
static int probe_mic_channel(void)
{
    const size_t frames = (size_t)I2S_SAMPLE_RATE * VOICE_PROBE_MS / 1000;
    int16_t *raw = (int16_t *)heap_caps_malloc(frames * I2S_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (raw == NULL)
    {
        ESP_LOGW(TAG, "声道探测缓冲分配失败，默认左声道");
        return 0;
    }

    (void)i2s_rx_drain(0);

    size_t got = 0;
    while (got < frames)
    {
        const size_t n = (frames - got > 960) ? 960 : (frames - got);
        size_t done = 0;
        const esp_err_t r = i2s_rx_read(raw + got * 2, n * I2S_FRAME_BYTES, &done,
                                        VOICE_RX_TIMEOUT_MS);
        if (r != ESP_OK || done == 0)
        {
            break;
        }
        got += done / I2S_FRAME_BYTES;
    }

    uint64_t sum_l = 0;
    uint64_t sum_r = 0;
    for (size_t i = 0; i < got; i++)
    {
        const int32_t l = raw[2 * i];
        const int32_t r = raw[2 * i + 1];
        sum_l += (uint64_t)(l * l);
        sum_r += (uint64_t)(r * r);
    }
    heap_caps_free(raw);

    ESP_LOGI(TAG, "声道探测（%u 帧）：L 能量=%llu R 能量=%llu → 使用%s声道", (unsigned)got,
             (unsigned long long)sum_l, (unsigned long long)sum_r, sum_r > sum_l ? "右" : "左");
    return (sum_r > sum_l) ? 1 : 0;
}

/**
 * @brief 运行期声道校正：用真实声音的能量判断麦克风到底在哪一路
 * @param sum_l  本帧左声道平方和
 * @param sum_r  本帧右声道平方和
 * @param peak_l 本帧左声道峰值
 * @param peak_r 本帧右声道峰值
 * @param n      本帧参与统计的点数
 *
 * @note 为什么必须有它：启动探测只有 300 ms，安静时两路只差几个百分点，
 *       选错就永远唤不醒（现场表现为“怎么喊都没反应”，但诊断页 RMS 有数）。
 *       这里累计 1.5 s 再比：**峰值先过门限**（确认真有人在说话），且较响一路
 *       ≥1.6 倍才认为有结论；纯底噪窗口直接丢弃，不会因噪声来回切。
 *       切换时 drain RX、重置 AFE/MultiNet，回到待唤醒状态——半截音频混着
 *       两路的采样，留着只会让下一句识别变差。
 */
static void channel_monitor(uint64_t sum_l, uint64_t sum_r, int32_t peak_l, int32_t peak_r, int n)
{
    const int64_t now = esp_timer_get_time() / 1000;
    if (s_ch_since_ms == 0)
    {
        s_ch_since_ms = now;
    }
    s_ch_sum_l += sum_l;
    s_ch_sum_r += sum_r;
    if (peak_l > s_ch_peak_l)
    {
        s_ch_peak_l = peak_l;
    }
    if (peak_r > s_ch_peak_r)
    {
        s_ch_peak_r = peak_r;
    }
    s_ch_points += n;

    if (s_ch_points <= 0 || now - s_ch_since_ms < VOICE_CH_WINDOW_MS)
    {
        return;
    }

    const double rms_l = sqrt((double)s_ch_sum_l / (double)s_ch_points);
    const double rms_r = sqrt((double)s_ch_sum_r / (double)s_ch_points);
    const int32_t peak_lw = s_ch_peak_l;
    const int32_t peak_rw = s_ch_peak_r;
    s_ch_sum_l = 0;
    s_ch_sum_r = 0;
    s_ch_peak_l = 0;
    s_ch_peak_r = 0;
    s_ch_points = 0;
    s_ch_since_ms = now;

    /* 峰值没过门限：只有底噪，不判断（否则会把噪声较大的那一路选成麦克风） */
    if (peak_lw < VOICE_CH_PEAK_MIN && peak_rw < VOICE_CH_PEAK_MIN)
    {
        return;
    }

    int want = s_mic_channel;
    if (rms_r > rms_l * VOICE_CH_RATIO)
    {
        want = 1;
    }
    else if (rms_l > rms_r * VOICE_CH_RATIO)
    {
        want = 0;
    }
    if (want == s_mic_channel)
    {
        return;
    }

    ESP_LOGW(TAG, "声道校正：L RMS=%.0f R RMS=%.0f → 改用%s声道", rms_l, rms_r,
             want ? "右" : "左");
    s_mic_channel = want;
    (void)i2s_rx_drain(0);
    reset_engines();
    enter_state(VOICE_STATE_WAKE);
}

/**
 * @brief 把 AFE 输出推进拼接队列
 * @return true 溢出（调用方应丢弃本轮）
 */
static bool mn_queue_push(const int16_t *data, int n)
{
    const int used = s_mn_q_write - s_mn_q_read;
    if (used + n > VOICE_MN_QUEUE)
    {
        return false;
    }
    for (int i = 0; i < n; i++)
    {
        s_mn_queue[(s_mn_q_write + i) % VOICE_MN_QUEUE] = data[i];
    }
    s_mn_q_write += n;
    return true;
}

/**
 * @brief 识别帧闸门的前置处理：清零窗口计数
 * @note  在 enter_state() 里调用：每次状态迁移（唤醒、命中、超时、声道校正）都从
 *        干净窗口开始，前一段语音的“有声音”结论不会延续到新窗口里。
 */
static void mn_gate_reset(void)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    s_mn_win_since_ms = now_ms;
    s_mn_win_speech = 0;
    s_mn_win_loud = 0;
    s_mn_win_active = false;
    s_mn_last_speech_ms = 0;
    s_mn_speech_frames = 0;
}

/**
 * @brief 判断本块 AFE 输出是否可以喂给 MultiNet（见 VOICE_MN_GATE_PEAK_MIN 说明）
 * @param res  AFE fetch 结果（取 vad_state）
 * @param peak 本块抽取后 16 kHz 输入峰值
 * @return true 可以喂（当前处在语音期）
 *
 * @note  **每块只调一次，返回值决定本块是否入队**。
 *        语音期 = VAD 判“说话”且峰值过门限（一帧即可开启），最后一帧语音之后
 *        再延续 VOICE_MN_GRACE_MS（命令词收尾的几个字可能落在 VAD 判静音的帧里，
 *        MultiNet 是滑窗解码，还得再往后推几块才出结果）。超过这段就清空队列、
 *        关闭闸门，直到下一次 VAD 说话。
 *        每 VOICE_MN_WINDOW_MS 打印一次窗口统计（VAD 语音帧、过门限帧、峰值），
 *        用来判断“阈值是不是太高”：正常说话每轮应有几十个 VAD 语音帧。
 */
static bool mn_gate_accept(const afe_fetch_result_t *res, int32_t peak)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const bool vad_speech = (res != NULL) && (res->vad_state == VAD_SPEECH);
    const bool loud = peak >= VOICE_MN_GATE_PEAK_MIN;

    if (s_mn_win_since_ms == 0 || now_ms - s_mn_win_since_ms >= VOICE_MN_WINDOW_MS)
    {
        if (s_mn_win_since_ms != 0)
        {
            ESP_LOGI(TAG, "识别闸门：%.1f s 窗口 VAD 语音帧 %d，过门限帧 %d，峰值 %d",
                     (double)(now_ms - s_mn_win_since_ms) / 1000.0, s_mn_win_speech,
                     s_mn_win_loud, s_mn_prev_peak);
        }
        else
        {
            /* 首个窗口：先打印 AFE 给出的角度量，确认 VAD 真的在跑、VAD 门限与被识别音量差多少 */
            ESP_LOGI(TAG, "识别闸门：首个窗口 VAD 语音帧 %d，过门限帧 %d，峰值 %d，音量 %d dB",
                     s_mn_win_speech, s_mn_win_loud, s_mn_prev_peak,
                     (res != NULL) ? (int)res->data_volume : -1);
        }
        s_mn_win_since_ms = now_ms;
        s_mn_win_speech = 0;
        s_mn_win_loud = 0;
        s_mn_prev_peak = 0;
    }
    if (loud)
    {
        s_mn_win_loud++;
        if (peak > s_mn_prev_peak)
        {
            s_mn_prev_peak = peak;
        }
        if (vad_speech)
        {
            s_mn_win_speech++;
        }
    }

    /* 开闸条件：AFE 的 VAD 判“说话”**且**输入峰值过门限。
     * 开闸之后只要有电平（peak 过门限）就续期——命令词中间的自然停顿可能让
     * VAD 短暂判静音，这段时间不能算“语音结束”。 */
    if (loud && vad_speech)
    {
        if (!s_mn_win_active)
        {
            ESP_LOGI(TAG, "识别闸门：检测到语音（峰值 %d），开始采信命令词", (int)peak);
        }
        s_mn_win_active = true;
        s_mn_last_speech_ms = now_ms;
        s_mn_speech_frames++;
    }
    else if (s_mn_win_active && loud)
    {
        s_mn_last_speech_ms = now_ms;
    }

    if (s_mn_win_active && now_ms - s_mn_last_speech_ms > VOICE_MN_GRACE_MS)
    {
        /* 语音结束：再多留一段让 MultiNet 推完最后的窗口，然后关闸。
         * 关闸时顺便 clean()：下一次开口时 MultiNet 从干净状态解码，
         * 不会把两段相隔几秒的语音拼成一条命令。
         * **语音帧计数也在这里清零**：它衡量的是“这一句语音有多长”，
         * 一句说完就归零；否则整段聆听窗口里零星几次噪声会不断累加，
         * 最终把噪声凑成“语音证据足够”，又回到“静音自己开门”。 */
        s_mn_win_active = false;
        s_mn_speech_frames = 0;
        s_mn_q_read = s_mn_q_write = 0;
        if (s_mn != NULL && s_mn_data != NULL)
        {
            (void)s_mn->clean(s_mn_data);
        }
    }
    return s_mn_win_active;
}

/**
 * @brief 丢弃本帧剩余音频（闸门关着、或刚 reset 引擎时使用）
 * @note  闸门关着时 AFE 输出既不能入队也不能读 MultiNet 结果，但 fetch() 必须照常
 *        调用（负责推进 AFE 内部状态并在唤醒后做回填），所以只能把队列清掉。
 */
static void mn_queue_clear(void)
{
    s_mn_q_read = s_mn_q_write = 0;
}

/**
 * @brief 从拼接队列取一块喂给 MultiNet 并处理结果
 * @param busy 播报期间为 true：照常推进模型但不派发命令
 * @return 命中的命令 ID；未命中（含超时、播报期、语音证据不足、重复命中）返回 -1
 * @warning 返回类型是 **int**。曾写成 bool：-1 与 1 都被转成 true，调用方用
 *          `hit >= 0` 判断，于是**任何一次非命中都会被当成“命中 id=1 打开门”**派发出去——
 *          现场表现为“待唤醒状态也每 0.45 s 识别成打开门”（那 0.45 s 正是 MultiNet
 *          512 点块周期），并且只要闸门一开就立刻派发。改回 int 后，只有真正的命中
 *          （0~8）才会走到派发层。
 *
 * @note **命中后不再切回待唤醒**（M6 修正）：原先命中即 enter_state(WAKE)，
 *       结果是“一次唤醒只能执行一条命令”——用户喊完“Hi,乐鑫”再说第二句
 *       （如“网络状态”）完全没有反应，必须先重新喊唤醒词，现场看起来就是
 *       “无法识别其他语音命令”。
 *       现在命中后由调用方 reset_engines() 清掉 AFE/MultiNet 状态与队列，
 *       继续留在 CMD 状态并刷新聆听窗口，可以在同一次唤醒里连续下多条命令。
 *       同一段音频不会被重复识别：reset_engines() 已把环形缓冲与多网络上下文清空。
 *       另外两道保险：
 *         - VOICE_CMD_ARM_MS 内（刚唤醒/刚命中）的结果不采信，避免唤醒词尾音误命中；
 *         - 同一条命令 VOICE_CMD_REPEAT_MS 内不重复派发。
 */
static int mn_step(bool busy)
{
    while (s_mn_q_write - s_mn_q_read >= s_mn_chunk)
    {
        for (int i = 0; i < s_mn_chunk; i++)
        {
            s_mn_buf[i] = s_mn_queue[(s_mn_q_read + i) % VOICE_MN_QUEUE];
        }
        s_mn_q_read += s_mn_chunk;

        const esp_mn_state_t st = s_mn->detect(s_mn_data, s_mn_buf);
        if (busy)
        {
            /* 播报期间只推进状态机：否则 MultiNet 的上下文与真实音频错位，
             * 播报一结束就容易误识别 */
            continue;
        }
        if (st == ESP_MN_STATE_DETECTED)
        {
            esp_mn_results_t *res = s_mn->get_results(s_mn_data);
            const int64_t now_ms = esp_timer_get_time() / 1000;

            /* ---- 采信规则（守卫 v5）：只看“这一句语音”的证据 ----
             * 现场日志（mn7_cn + 9 条拼音命令）：每一次 DETECTED 都是
             * command_id[0]=1、prob[0]=0、string/raw_string 全空，无论人说话还是
             * 纯静音，输出完全一样。也就是说“结果串非空 / prob 可信”这两条判据
             * 在本模型上不成立，曾被用来挡噪声，反而把真人命令也一起挡掉了。
             * 现在改以**输入侧**证据为准：
             *   1) 本轮（自唤醒/上次命中起）累计的 VAD 语音帧达到门限；开门命令
             *      保持更高门限，其他命令降低至 VOICE_MN_MIN_SPEECH_FRAMES；
             *   2) 一句语音只判定一次：一有结果就把语音帧清零，下一句重新累计，
             *      同一段语音不会在滑窗里反复命中；
             *   3) 同一命令 VOICE_CMD_REPEAT_MS 内不重复派发。
             * 静音时只有零星噪声帧，且一句说完即清零，永远凑不够证据，
             * 因此“没说话却自己开门”在这条规则下不会发生。
             * 代价：mn7 把“说了别的词”误映射成打开门时，结果串无法区分（恒空）；
             * 这类误判只能靠 VOICE_MN_GATE_PEAK_MIN（音量门限）与语音帧数挡住，
             * 再加派发层的开门限速（VOICE_OPEN_COOLDOWN_MS）兜底。 */
            if (res == NULL || res->num <= 0 || res->command_id[0] < 0 ||
                res->command_id[0] >= VOICE_CMD_NUM)
            {
                (void)s_mn->clean(s_mn_data);
                continue;
            }

            const int id = res->command_id[0];
            const float prob = res->prob[0];

            /* 这一句的判定到此结束：无论采信与否都清零语音帧，下一句重新累计。
             * 不清零的话，同一段语音在结果之后的滑窗里还会再命中一次
             * （现场 ~0.44 s 一次的重复命中就是这么来的）。 */
            const int speech_frames = s_mn_speech_frames;
            s_mn_speech_frames = 0;

            ESP_LOGI(TAG, "命中评估：id=%d prob=%d/10000 语音帧=%d 候选=%d 短语=%d,%d,%d,%d,%d 串=%.40s 原文=%.40s",
                     id, (int)lroundf(prob * 10000.0f), speech_frames, (int)res->num,
                     (int)res->phrase_id[0], (int)res->phrase_id[1], (int)res->phrase_id[2],
                     (int)res->phrase_id[3], (int)res->phrase_id[4],
                     res->string[0] ? res->string : "(空)",
                     res->raw_string[0] ? res->raw_string : "(空)");

            const int min_speech_frames =
                (id == 0 || id == 1) ? VOICE_MN_OPEN_MIN_SPEECH_FRAMES
                                     : VOICE_MN_MIN_SPEECH_FRAMES;
            if (speech_frames < min_speech_frames)
            {
                ESP_LOGW(TAG, "命令词命中但本轮语音证据不足（id=%d，语音帧 %d < %d），按噪声忽略",
                         id, speech_frames, min_speech_frames);
                (void)s_mn->clean(s_mn_data);
                continue;
            }

            if (id == s_last_dispatch_id && now_ms - s_last_dispatch_ms < VOICE_CMD_REPEAT_MS)
            {
                ESP_LOGI(TAG, "命令词 %.1f s 内重复命中（id=%d），忽略",
                         (double)(now_ms - s_last_dispatch_ms) / 1000.0, id);
                (void)s_mn->clean(s_mn_data);
                continue; /* 本轮 mn_step 继续解剩余采样；continue 不会回主循环 */
            }

            /* 日志/回显：模型给的原始串（mn7_cn 可能为空，只用于日志） */
            snprintf(s_hit_raw, sizeof(s_hit_raw), "%.40s",
                     res->raw_string[0] != 0 ? res->raw_string : res->string);
            s_hit_prob = prob;
            s_last_dispatch_id = id;
            s_last_dispatch_ms = now_ms;
            return id;
        }
        else if (st == ESP_MN_STATE_TIMEOUT && s_state == VOICE_STATE_CMD)
        {
            /* 超时提示只在“这一轮唤醒一条命令都没派发”时出现：派发过命令后
             * 用户再说的那句没识别出来时，状态栏保留上一条命令的结果更有用 */
            ESP_LOGI(TAG, "命令词聆听超时，回到待唤醒（本轮已派发 %d 条）",
                     s_cmd_dispatched);
            if (s_cmd_dispatched == 0)
            {
                ui_app_set_status(UI_TXT_VOICE_NOT_HEARD, 0xC0392B);
            }
            enter_state(VOICE_STATE_WAKE);
            return -1;
        }
    }
    return -1;
}

/* ====================== 命令派发 ====================== */

/**
 * @brief 请求把界面切到指定页面
 * @param page 目标页面
 *
 * @note  所有页面都在 ui_app_start() 时预先创建；此处只切换现有屏幕并获取 LVGL 锁，
 *        不触碰 Flash。直接调用避免内部 RAM 耗尽时临时创建页面任务失败。
 */
static void request_page(ui_page_t page)
{
    const esp_err_t err = ui_app_switch_page(page);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "页面切换失败：page=%d（%s）", (int)page, esp_err_to_name(err));
    }
}

/**
 * @brief 执行一条识别到的命令词
 * @param id 命令 ID（s_commands 下标）
 * @param phrase 识别出的原始字符串（日志/界面回显）
 *
 * @note 所有分支都必须快速返回（不等 face_service 回执），否则 voice_task
 *       停止读 I2S，RX DMA 溢出、唤醒延迟明显变大。
 */
static void dispatch_command(int id, const char *phrase)
{
    /* phrase 是 res->string；raw 是 res->raw_string。两者都可能是空串（mn7_cn 的
     * 结果串不保证填满），所以日志里连 prob 一起打印，用于判断命中是真语音还是噪声 */
    /* MultiNet 返回的是登记的拼音串（res->string），界面/日志统一显示中文，
     * 按 command_id 反查 s_commands[]；phrase 只写进日志辅助排查发音问题 */
    const bool id_ok = (id >= 0 && id < VOICE_CMD_NUM);
    const char *zh = id_ok ? s_commands[id] : UI_TXT_UNKNOWN;
    snprintf(s_last_cmd, sizeof(s_last_cmd), "%.40s", zh);
    /* prob 用万分比整数打印：浮点输出在部分格式化实现/串口日志里会显示 0.00，
     * 无法区分“真 0”与“小值”，排查时会被误导 */
    ESP_LOGI(TAG, "命令词识别：id=%d “%s” prob=%d/10000（拼音 %.40s / 原文 %.40s）", id, zh,
             (int)lroundf(s_hit_prob * 10000.0f), phrase ? phrase : "", s_hit_raw);
    s_cmd_dispatched++;

    switch (id)
    {
    case 0:
    case 1: /* 打开门禁 / 打开门：走人脸识别流水线（语音代替按键） */
        /* 派发层不再检查“识别串非空”：mn7_cn 对本词表恒返回空串（见 mn_step 的
         * 采信规则），能走到这里的命中已经通过了语音证据闸门（VAD 语音帧数）。
         * 这里只保留开门这条路必须有的两道保险：限速 + 识别在途合并。 */
        /* 限速：这是唯一会“开门”的语音命令，必须比别的命令更保守。
         * 现场曾出现 MultiNet 在底噪上每秒命中一次“打开门”，每次触发一轮识别 +
         * 一声提示音，喇叭“嘀嘀嘀”不停。除识别闸门与 VAD 证据之外再加一道硬限速，
         * 保证即使误判重复出现，也不会连续开门与连续响提示音。 */
        if (esp_timer_get_time() / 1000 - s_last_open_ms < VOICE_OPEN_COOLDOWN_MS)
        {
            ESP_LOGW(TAG, "语音开门 %.1f s 内已触发过，本次忽略",
                     (double)(esp_timer_get_time() / 1000 - s_last_open_ms) / 1000.0);
            break;
        }
        /* 识别在途（约 370 ms，且一轮识别结束后才回执）时不叠加新请求：voice_task
         * 是唯一读者，它一旦阻塞等人脸流水线，I2S RX 就会溢出、唤醒延迟变大 */
        if (face_service_recognize_in_flight())
        {
            ESP_LOGI(TAG, "人脸识别进行中，本次语音开门请求已合并（不重复排队）");
            break;
        }
        /* 这里**不置“打开门禁”状态条**：异步识别只有开门/拒绝/陌生人三种结局
         * 才会写状态条（door_controller），而“无人脸/活体检测中”这两种本次
         * 并没有开门，却没有任何东西会把状态条改回来——现场表现就是界面
         * 一直停在“打开门禁”，让人以为一直在开门。 */
        /* 改走 UI 层的入口（不是直接调 face_service_recognize_async）：
         * 它先把主页状态卡写成“识别中”并启动轮询定时器，识别结束后恢复空闲文字。
         * 直接调服务层时屏幕上**什么都不会变**，现场用户以为“说了打开门没反应”。 */
        ui_home_recognize_async();
        /* 注意：这里**不能**播“正在识别”——识别本身只要 ~400 ms，等用户说完话再播报，
         * 播报那 1 s 正好盖住识别结果（音频忙期间 voice_task 也丢弃输入）。
         * 反馈改由状态卡 + 识别结论播报（见 face_task 的 report_prompt）提供。 */
        s_last_open_ms = esp_timer_get_time() / 1000;
        break;

    case 2: /* 查询记录 */
        request_page(UI_PAGE_LOGS);
        ui_app_set_status(UI_TXT_VOICE_QUERY_LOG, 0x2F6FED);
        (void)voice_prompt_say(VOICE_PROMPT_QUERY_LOG);
        break;

    case 3: /* 注册人脸 */
        request_page(UI_PAGE_ENROLL);
        ui_app_set_status(UI_TXT_VOICE_WOKEN, 0x2F6FED);
        (void)voice_prompt_say(VOICE_PROMPT_MENU);
        voice_assistant_rearm_wake();
        break;

    case 4: /* 网络状态 */
    {
        char sum[96] = "";
        net_service_summary(sum, sizeof(sum));
        ESP_LOGI(TAG, "网络状态：%s", sum[0] ? sum : "未知");
        ui_app_set_status(sum[0] ? sum : UI_TXT_UNKNOWN, 0x2F6FED);
        break;
    }

    case 5: /* 声音大一点 */
    case 6: /* 声音小一点 */
    {
        /* 语音命令按“方向”播报（调大/调小），设置页的 +/- 走
         * voice_assistant_set_volume_and_report（按档位播报） */
        int vol = (int)audio_service_get_volume();
        vol += (id == 5) ? VOICE_VOLUME_STEP : -VOICE_VOLUME_STEP;
        (void)voice_assistant_set_volume_and_report(vol);
        ui_app_set_status(id == 5 ? UI_TXT_VOICE_VOL_UP : UI_TXT_VOICE_VOL_DOWN, 0x2F6FED);
        break;
    }

    case 8: /* 打开设置：跳到设备设置页（音量/阈值/活体/网络都在那里） */
        request_page(UI_PAGE_SETTINGS);
        ui_app_set_status(UI_TXT_SET_TITLE, 0x2F6FED);
        (void)voice_prompt_say(VOICE_PROMPT_MENU);
        break;

    case 7:  /* 取消操作 */
    default: /* 未知 ID：只提示 */
        /* 取消/未知命令**不响提示音**：默认分支恰好是误识别最多的地方，一旦
         * 误判连续出现，提示音就会“嘀嘀嘀”不停。只刷新状态卡文字。 */
        ui_app_set_status(UI_TXT_CANCEL, 0xC0392B);
        (void)voice_prompt_say(VOICE_PROMPT_CANCEL);
        break;
    }
}

/* ====================== 主循环 ====================== */

/**
 * @brief 语音任务主循环
 * @param arg 未使用
 *
 * 每轮：读 40 ms I2S → 3 倍抽取填 AFE 输入 → 填满就 feed+fetch →
 *       AFE 输出拼给 MultiNet → 命令/唤醒状态机 → 电平统计。
 */
static void voice_task(void *arg)
{
    (void)arg;
    /* 任务比引擎先创建（见 voice_assistant_init 的顺序说明），先等初始化完成 */
    while (!s_ready)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "语音任务启动：AFE 输入 %d 点 / 输出 %d 点，MultiNet %d 点（%s声道）",
             s_feed_chunk, s_fetch_chunk, s_mn_chunk, s_mic_channel ? "右" : "左");

    int64_t resume_at_ms = 0;
    bool was_busy = false;

    while (true)
    {
        uint32_t notifications = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &notifications, 0) == pdTRUE &&
            (notifications & VOICE_TASK_NOTIFY_REARM_WAKE) != 0)
        {
            reset_engines();
            enter_state(VOICE_STATE_WAKE);
            s_cmd_dispatched = 0;
            ESP_LOGI(TAG, "人脸录入流程结束，语音状态已重置为待唤醒");
        }

        /* ---------- 读 I2S RX（始终读，不读会让 DMA 溢出） ---------- */
        size_t done = 0;
        (void)i2s_rx_read(s_rx_buf, VOICE_RX_FRAMES * I2S_FRAME_BYTES, &done, VOICE_RX_TIMEOUT_MS);
        const int frames = (int)(done / I2S_FRAME_BYTES);
        if (frames <= 0)
        {
            continue;
        }

        /* ---------- 播报期间：只读不识别（防止听见自己的扬声器） ---------- */
        const bool busy = s_input_paused ||
                          ((g_sys_events != NULL) &&
                           (xEventGroupGetBits(g_sys_events) & SYS_EVENT_AUDIO_BUSY) != 0);
        if (busy)
        {
            was_busy = true;
            s_paused = true;
            s_feed_fill = 0;
            s_mn_q_read = 0;
            s_mn_q_write = 0;

            /* 播报也占用了命令词聆听窗口的时间：播报结束回到聆听状态时重新计时，
             * 否则唤醒提示播完后剩余时间变短，用户还以为有完整的 10 s。
             * 只在命令聆听状态下续期，待唤醒状态由唤醒/超时分支自己计时。 */
            if (s_state == VOICE_STATE_CMD)
            {
                s_state_since_ms = esp_timer_get_time() / 1000;
            }
            continue;
        }
        if (was_busy)
        {
            resume_at_ms = esp_timer_get_time() / 1000 + VOICE_RESUME_DELAY_MS;
            was_busy = false;
        }
        if (esp_timer_get_time() / 1000 < resume_at_ms)
        {
            continue;
        }
        if (s_paused)
        {
            s_paused = false;
            /* 播报期间 AFE 被整段丢弃（丢掉的音频不能灌进模型，否则唤醒词/命令词
             * 的上下文与真实语音错位）：恢复时先清 RX、再 reset 两个引擎。
             * 命令聆听状态下顺便把窗口重新计时，让“你好”播完后用户仍有完整的
             * 聆听时间（计时起点见 busy 分支）。 */
            (void)i2s_rx_drain(0);
            reset_engines();
            if (s_state == VOICE_STATE_CMD)
            {
                s_state_since_ms = esp_timer_get_time() / 1000;
            }
        }

        /* ---------- 抽取到 AFE 输入，满一块就 feed + fetch ---------- */
        uint64_t sum_sq = 0;
        uint64_t ch_sq_l = 0; /* 两路各自的平方和/峰值：喂模型之前先判声道，见 channel_monitor */
        uint64_t ch_sq_r = 0;
        int32_t ch_pk_l = 0;
        int32_t ch_pk_r = 0;
        int32_t peak = 0;
        int taken = 0;
        for (int i = 0; i < frames; i += VOICE_DECIMATE)
        {
            const int16_t v = s_rx_buf[2 * i + s_mic_channel];
            sum_sq += (uint64_t)((int32_t)v * (int32_t)v);
            const int32_t l = s_rx_buf[2 * i];
            const int32_t r = s_rx_buf[2 * i + 1];
            ch_sq_l += (uint64_t)(l * l);
            ch_sq_r += (uint64_t)(r * r);
            const int32_t al = l < 0 ? -l : l;
            const int32_t ar = r < 0 ? -r : r;
            if (al > ch_pk_l)
            {
                ch_pk_l = al;
            }
            if (ar > ch_pk_r)
            {
                ch_pk_r = ar;
            }
            taken++;
            const int32_t a = v < 0 ? -v : v;
            if (a > peak)
            {
                peak = a;
            }

            s_feed_buf[s_feed_fill++] = v;
            if (s_feed_fill < s_feed_chunk)
            {
                continue;
            }
            s_feed_fill = 0;
            (void)s_afe->feed(s_afe_data, s_feed_buf);

            afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
            if (res == NULL || res->data == NULL || res->data_size <= 0)
            {
                continue;
            }
            const int n = res->data_size / (int)sizeof(int16_t);

            /* 识别帧闸门：只有 VAD 判“说话”且电平过门限的时段才喂 MultiNet。
             * 静音时既不入队也不跑 detect——现场故障（静音下反复命中“打开门”）
             * 就是 MultiNet 在纯底噪上被置 DETECTED 导致的，见 VOICE_MN_GATE_PEAK_MIN 说明。
             * 本块抽取峰值用本帧读到的 peak（与输入同一帧窗口）。 */
            if (s_state == VOICE_STATE_CMD && !mn_gate_accept(res, peak))
            {
                mn_queue_clear();
            }
            else if (!mn_queue_push(res->data, n))
            {
                ESP_LOGW(TAG, "MultiNet 队列溢出，丢弃 %d 点", n);
                mn_queue_clear();
                continue;
            }

            if (s_state == VOICE_STATE_CMD && esp_timer_get_time() / 1000 < s_cmd_arm_ms)
            {
                /* 刚唤醒/刚执行完命令：这段时间的 AFE 输出不可靠（唤醒词尾音、提示音残响），
                 * 既不给 MultiNet 也不采信结果，直接丢弃队列内容。
                 * 注意入队与这里必须成对：入队分支不判 s_cmd_arm_ms，否则唤醒后
                 * 前 VOICE_CMD_ARM_MS 的所有音频（正是用户紧跟着说命令词的时间）都被丢掉，
                 * 现场表现为“唤醒后紧接着说命令反而识别不了”。 */
                s_mn_q_read = s_mn_q_write = 0;
            }
            else if (s_state == VOICE_STATE_CMD)
            {
                const int hit = mn_step(busy);
                if (hit >= 0)
                {
                    /* mn_step 缓存了拼音串（不要再用 get_results：那是“最近一次”结果，
                     * 重复取会拿到同一句、派发两次） */
                    dispatch_command(hit, s_hit_raw);
                    /* 清掉 AFE 环形缓冲 / MultiNet 上下文 / 拼接队列：同一段音频不能
                     * 再被识别一次；随后**留在聆听状态**并重新计时，让用户能在同一次
                     * 唤醒里继续说下一条命令（见 mn_step 的说明）。
                     * s_cmd_arm_ms 跳过刚 reset 后 fetch 出来的残留块。 */
                    reset_engines();
                    enter_state(VOICE_STATE_CMD);
                    s_cmd_arm_ms = esp_timer_get_time() / 1000 + VOICE_CMD_ARM_MS;
                    /* 本帧剩下的采样是播报前的旧语音，喂给刚 reset 的 AFE 会污染
                     * 下一轮识别；与唤醒分支一样直接跳出本帧 */
                    break;
                }
            }
            else
            {
                /* 待唤醒：MultiNet 不跑，但队列要清空（唤醒后才开始累积） */
                s_mn_q_read = s_mn_q_write = 0;
            }

            if (res->wakeup_state == WAKENET_DETECTED && s_state == VOICE_STATE_WAKE)
            {
                ESP_LOGI(TAG, "唤醒词命中“%s”，进入命令词聆听（%.0f s）", VOICE_WAKE_WORD,
                         VOICE_CMD_WINDOW_MS / 1000.0);
                s_last_cmd[0] = '\0';
                s_cmd_dispatched = 0; // 新的一次唤醒：重新统计本轮派发了几条
                ui_app_set_status(UI_TXT_VOICE_SPEAK, 0x27AE60);
                speak_cue(UI_TXT_VOICE_WOKEN);
                /* speak_cue 已 drain + 清缓冲，但本帧里排在唤醒之后的音频段
                 * 还在局部循环里没喂完，继续喂会把提示音灌给 MultiNet；
                 * 同时 reset AFE/MultiNet，把唤醒词本身从模型上下文里去掉，
                 * 否则下一块输出很容易沿着“Hi,乐鑫”的尾音凑出某个命令词。 */
                reset_engines();
                enter_state(VOICE_STATE_CMD);
                s_cmd_arm_ms = esp_timer_get_time() / 1000 + VOICE_CMD_ARM_MS;
                break;
            }

        }

        /* ---------- 声道校正：必须在电平统计之前，切换后本帧剩下的采样已作废 ---------- */
        const int ch_before = s_mic_channel;
        channel_monitor(ch_sq_l, ch_sq_r, ch_pk_l, ch_pk_r, taken);
        if (s_mic_channel != ch_before)
        {
            continue;
        }

        /* ---------- 电平统计 + 命令窗口兜底超时 ---------- */
        if (taken > 0)
        {
            s_level_rms = (uint32_t)sqrtf((float)sum_sq / (float)taken);
            s_level_peak = (uint32_t)peak;
        }
        if (s_state == VOICE_STATE_CMD &&
            esp_timer_get_time() / 1000 - s_state_since_ms > VOICE_CMD_WINDOW_MS + 500)
        {
            ESP_LOGI(TAG, "命令词窗口结束，回到待唤醒（本轮已派发 %d 条）", s_cmd_dispatched);
            if (s_cmd_dispatched == 0)
            {
                ui_app_set_status(UI_TXT_VOICE_NOT_HEARD, 0xC0392B);
            }
            enter_state(VOICE_STATE_WAKE);
        }

#if VOICE_SELFTEST
        static int64_t last_log_ms = 0;
        const int64_t now = esp_timer_get_time() / 1000;
        if (now - last_log_ms > 2000)
        {
            last_log_ms = now;
            ESP_LOGI(TAG, "[自检] 16 kHz 输入 RMS=%u 峰值=%u", (unsigned)s_level_rms,
                     (unsigned)s_level_peak);
        }
#endif
    }
}

/* ====================== 对外接口 ====================== */

/**
 * @brief 打印语音初始化各阶段的内部 RAM 收支
 * @param step 阶段名
 * @note  排查“初始化期 ESP_ERR_NO_MEM”（esp_timer_create / 合法定时器）时用：
 *        内部 RAM 最大块是碎片敏感指标，空闲够但最大块小同样会失败。
 */
static void voice_mem_log(const char *step)
{
    ESP_LOGI(TAG, "[内存] %-10s 内部空闲=%u 最大块=%u | PSRAM 空闲=%u", step,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

esp_err_t voice_assistant_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(audio_service_is_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "音频服务未就绪（I2S 未初始化）");

    /* ---------- 1. 从 model 分区取模型（mmap，不占堆） ---------- */
    s_models = esp_srmodel_init("model");
    const char *wn_name = (s_models != NULL) ? esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL) : NULL;
    const char *mn_name =
        (s_models != NULL) ? esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE) : NULL;
    if (wn_name == NULL || mn_name == NULL)
    {
        ESP_LOGE(TAG, "model 分区里缺少唤醒模型或中文命令词模型（wn=%s mn=%s）："
                      "请确认 sdkconfig 选了 SR_WN_WN9_HILEXIN + SR_MN_CN_MULTINET7_QUANT，"
                      "并用 idf.py flash 写入 srmodels.bin",
                 wn_name ? wn_name : "无", mn_name ? mn_name : "无");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "模型加载：唤醒=%s，命令词=%s", wn_name, mn_name);

    /* ---------- 2. 先建任务，再建引擎 ----------
     * 顺序不能反：引擎建好之后内部 RAM 已被 AFE/MultiNet 用到只剩 ~15 KB 空闲、
     * 最大连续块 6 KB 左右，再建 8 KB 的 PSRAM 栈任务会连带 TCB（内部 RAM）
     * 一起难申请。任务起来后先等 s_ready 再开始干活。
     * 栈放 PSRAM：内部 RAM 在 WiFi 之后只剩 ~7 KB 最大块（AFE 之后更小），塞不下 8 KB。 */
#if VOICE_SELFTEST
    ESP_LOGI(TAG, "[自检] 建任务前：内部最大块=%u 空闲=%u | PSRAM 空闲=%u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        voice_task, "voice_task", VOICE_TASK_STACK, NULL, VOICE_TASK_PRIO, &s_voice_task_handle, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "voice_task 创建失败");
    voice_mem_log("建任务后");

    /* 顺序说明（内部 RAM 峰值的实测结果，改动前先看 voice_mem_log 的启动输出）：
     * AFE 常驻占内部 RAM 约 16.5 KB，MultiNet 常驻约 4 KB，但 MultiNet 的
     * create（打印 “Build fst from commands.”）会**临时**再吃掉剩余的全部内部
     * RAM——sys_monitor 在这 4 s 窗口里打印过 `内部=19 最大块=0`。如果先建 AFE，
     * 这 4 s 里 WiFi 一旦恰好连接成功（esp_coex 要建合法定时器 ets_timer_setfn →
     * esp_timer_create），就会 ESP_ERR_NO_MEM 直接 abort 复位（现场表现为
     * “有时开机几秒就重启”）。
     * 因此**先建 MultiNet、再建 AFE**：MultiNet 峰值时还有 35 KB 空闲、约 16 KB
     * 余量，WiFi 在这段时间里怎么分配都够；AFE 的 16.5 KB 是一次性小块分配，
     * 放在峰值之后不影响。两者不互相依赖，顺序可以这样排。 */
    /* ---------- 3. 创建 MultiNet 并灌入中文命令词 ---------- */
    s_mn = esp_mn_handle_from_name((char *)mn_name);
    ESP_RETURN_ON_FALSE(s_mn != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "MultiNet 句柄获取失败");
    /* 时长参数：命令词检测窗口（毫秒），与 VOICE_CMD_WINDOW_MS 对齐 */
    s_mn_data = s_mn->create(mn_name, VOICE_CMD_WINDOW_MS);
    ESP_RETURN_ON_FALSE(s_mn_data != NULL, ESP_ERR_NO_MEM, TAG, "MultiNet 创建失败（内存不足）");
    s_mn_chunk = s_mn->get_samp_chunksize(s_mn_data);
    ESP_RETURN_ON_FALSE(s_mn_chunk > 0, ESP_FAIL, TAG, "MultiNet 帧长异常");

    voice_mem_log("建 MultiNet 后");
    ESP_RETURN_ON_ERROR(esp_mn_commands_alloc(s_mn, s_mn_data), TAG, "命令词链表初始化失败");
    for (int i = 0; i < VOICE_CMD_NUM; i++)
    {
        /* 按拼音登记（中文串只用于日志）：见 s_phonemes[] 上方说明。
         * res->string 是拼音，日志里再按 command_id 回查中文 */
        const esp_err_t e = esp_mn_commands_add(i, s_phonemes[i]);
        if (e != ESP_OK)
        {
            ESP_LOGW(TAG, "命令词添加失败：id=%d “%s”（%s）", i, s_commands[i], esp_err_to_name(e));
        }
    }
    for (size_t i = 0; i < sizeof(s_command_aliases) / sizeof(s_command_aliases[0]); i++)
    {
        const int id = s_command_aliases[i].command_id;
        const esp_err_t e = esp_mn_commands_add(id, s_command_aliases[i].phoneme);
        if (e != ESP_OK)
        {
            ESP_LOGW(TAG, "命令词别名添加失败：id=%d “%s”（%s）", id, s_commands[id],
                     esp_err_to_name(e));
        }
    }
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err != NULL)
    {
        ESP_LOGW(TAG, "MultiNet 更新：%d 条命令词无法解析", err->num);
        for (int i = 0; i < err->num; i++)
        {
            ESP_LOGW(TAG, "  无法解析：“%s”", err->phrases[i]->string);
        }
    }
    ESP_LOGI(TAG, "命令词表已加载：%d 类、%u 种说法（守卫 v5：普通命令 VAD 语音帧≥%d，"
                  "开门≥%d，识别串/prob 仅记录不采信，开门限速 %.0f s）",
             VOICE_CMD_NUM,
             (unsigned)(VOICE_CMD_NUM + sizeof(s_command_aliases) / sizeof(s_command_aliases[0])),
             VOICE_MN_MIN_SPEECH_FRAMES, VOICE_MN_OPEN_MIN_SPEECH_FRAMES,
             VOICE_OPEN_COOLDOWN_MS / 1000.0);
    /* 打印命令表与模型侧“活跃短语”的对应关系（只在启动时一次）。
     * 不同 phrase ID 可以映射到同一 command ID（同义说法），但不应出现不同命令的
     * 拼音意外映射到同一 phrase。 */
    esp_mn_commands_print();
    esp_mn_active_commands_print();
    voice_mem_log("命令表更新后");

    /* ---------- 4. 创建 AFE（16 kHz 单麦；SR 模式；内存优先 PSRAM） ----------
     * memory_alloc_mode 是“把工作缓冲尽量放进哪个区”，MORE_PSRAM 是选项里
     * 最省内部 RAM 的一档；实测建完 AFE 内部反而少了 16.5 KB（35.5 → 19 KB），
     * 这一档不能再降（MORE_INTERNAL / BALANCE 只会吃更多内部 RAM）。
     * AFE 在 Core1 实时运行，PSRAM 访问延迟不影响唤醒。 */
    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(afe_cfg != NULL, ESP_ERR_NO_MEM, TAG, "AFE 配置创建失败");
    afe_cfg->wakenet_model_name = (char *)wn_name;
    afe_cfg->wakenet_mode = DET_MODE_90;
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_cfg->afe_perferred_core = 1; /* 与 face_task(0)、LVGL(0) 分核 */
    /* VAD 显式打开并放宽时长：识别闸门依赖 res->vad_state，vad_init=false 时
     * 它恒为 VAD_SILENCE，闸门永远不会开（表现为“怎么都识别不出命令词”）。
     * vad_mode 取默认档；min_speech 32 ms 让短促的“开”也能开闸，
     * min_noise 300 ms 让命令词中间的自然停顿不会被判成“语音结束”。 */
    afe_cfg->vad_init = true;
    afe_cfg->vad_min_speech_ms = 32;
    afe_cfg->vad_min_noise_ms = 300;
    afe_cfg->vad_delay_ms = 128;

    s_afe = esp_afe_handle_from_config(afe_cfg);
    ESP_RETURN_ON_FALSE(s_afe != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "AFE 句柄获取失败");
    s_afe_data = s_afe->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
    ESP_RETURN_ON_FALSE(s_afe_data != NULL, ESP_ERR_NO_MEM, TAG, "AFE 创建失败（内存不足）");
    voice_mem_log("建 AFE 后");

    s_feed_chunk = s_afe->get_feed_chunksize(s_afe_data);
    s_fetch_chunk = s_afe->get_fetch_chunksize(s_afe_data);
    if (s_feed_chunk <= 0 || s_fetch_chunk <= 0)
    {
        ESP_LOGE(TAG, "AFE 帧长异常：feed=%d fetch=%d", s_feed_chunk, s_fetch_chunk);
        return ESP_FAIL;
    }

    /* ---------- 5. 工作缓冲（全部 PSRAM：内部 RAM 不够长驻） ---------- */
    const size_t pcap = MALLOC_CAP_SPIRAM;
    const size_t psz[] = {
        sizeof(int16_t) * 2 * VOICE_RX_FRAMES, /* s_rx_buf */
        sizeof(int16_t) * s_feed_chunk,        /* s_feed_buf */
        sizeof(int16_t) * s_fetch_chunk,       /* s_fetch_buf */
        sizeof(int16_t) * s_mn_chunk,          /* s_mn_buf */
        sizeof(int16_t) * VOICE_MN_QUEUE,      /* s_mn_queue */
    };
    int16_t **const pbufs[] = { &s_rx_buf, &s_feed_buf, &s_fetch_buf, &s_mn_buf, &s_mn_queue };
    for (size_t i = 0; i < sizeof(pbufs) / sizeof(pbufs[0]); i++)
    {
        *pbufs[i] = (int16_t *)heap_caps_malloc(psz[i], pcap);
        ESP_RETURN_ON_FALSE(*pbufs[i] != NULL, ESP_ERR_NO_MEM, TAG,
                            "语音工作缓冲 %u 分配失败（PSRAM 不足，需 %u 字节）", (unsigned)i,
                            (unsigned)psz[i]);
    }

    /* ---------- 6. 声道判定（只在启动时做一次，任务放行前做，避免抢 RX） ----------
     * 先等 600 ms：I2S/ES7210 刚上电有明显瞬态，实测同一块板不同次上电的
     * 探测能量能差 20 倍（58M / 317M / 1.28G），立刻探测等于抛硬币。
     * 探测结果只是初值，真实判定交给运行期的 channel_monitor()。 */
    vTaskDelay(pdMS_TO_TICKS(600));
    s_mic_channel = probe_mic_channel();
    (void)i2s_rx_drain(0);

    /* ---------- 7. 放行任务 ---------- */
    /* 打印一次 AFE 流水线，方便排查“唤醒词不工作” */
    if (s_afe->print_pipeline != NULL)
    {
        s_afe->print_pipeline(s_afe_data);
    }

    enter_state(VOICE_STATE_WAKE);
    s_ready = true;
    ESP_LOGI(TAG, "语音助手就绪：唤醒词“%s”，命令词 %d 条", VOICE_WAKE_WORD, VOICE_CMD_NUM);
    return ESP_OK;
}

bool voice_assistant_is_ready(void)
{
    return s_ready;
}

void voice_assistant_set_input_paused(bool paused)
{
    s_input_paused = paused;
}

void voice_assistant_rearm_wake(void)
{
    if (!s_ready || s_voice_task_handle == NULL)
    {
        return;
    }

    const BaseType_t err =
        xTaskNotify(s_voice_task_handle, VOICE_TASK_NOTIFY_REARM_WAKE, eSetBits);
    if (err != pdPASS)
    {
        ESP_LOGE(TAG, "请求语音回到待唤醒状态失败");
    }
}

/**
 * @brief 设置音量并播报一档音量提示（语音命令与设置页 +/- 共用）
 *
 * @note  播报文案只能按档位选（0/1/2）——素材是固定短句，没有“音量百分之多少”
 *        这种拼接能力（见 voice_prompt.c 文件头）。档位：≤0 调小、≥70 调大、
 *        其余也播“音量已调小”（表示已经降下来），避免出现“调大了却播调小”。
 *        非阻塞：只投递播报，不等播放结束。
 */
esp_err_t voice_assistant_set_volume_and_report(int vol)
{
    if (vol < 0)
    {
        vol = 0;
    }
    if (vol > 100)
    {
        vol = 100;
    }
    const esp_err_t err = audio_service_set_volume((uint8_t)vol);
    (void)voice_prompt_say(vol >= 70 ? VOICE_PROMPT_VOL_UP : VOICE_PROMPT_VOL_DOWN);
    return err;
}

bool voice_assistant_get_level(uint32_t *rms, uint32_t *peak)
{
    if (!s_ready)
    {
        return false;
    }
    if (rms != NULL)
    {
        *rms = s_level_rms;
    }
    if (peak != NULL)
    {
        *peak = s_level_peak;
    }
    return true;
}

/**
 * @brief 语音任务当前状态的中文描述（诊断页“语音”行）
 * @return 静态字符串，调用方不要释放
 * @note  **必须引用 ui_strings.h 的宏**（UI_TXT_VOICE_STATE_*），不要直接写字面量：
 *        gen_font_chars.py 只扫描 ui_strings.h，这里写死中文会让字库缺字形、界面显方块。
 */
const char *voice_assistant_state_text(void)
{
    if (!s_ready)
    {
        return UI_TXT_VOICE_STATE_NOT_RDY;
    }
    if (s_paused)
    {
        return UI_TXT_VOICE_STATE_PAUSED;
    }
    return (s_state == VOICE_STATE_CMD) ? UI_TXT_VOICE_STATE_CMD : UI_TXT_VOICE_STATE_IDLE;
}

const char *voice_assistant_last_command(void)
{
    return s_last_cmd;
}
