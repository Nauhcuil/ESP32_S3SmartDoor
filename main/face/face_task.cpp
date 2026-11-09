/**
 * @file    face_task.cpp
 * @brief   人脸推理任务（C++）：唯一持有 MSR01 / MNP01 / FaceRecognition112V1S8 模型，
 *          负责取帧、检测、选脸、跟踪、质量门限、识别、活体、命令执行与结果发布。
 *
 * @note    - 帧归属：本任务从摄像头帧队列取帧，处理完后**先**送入显示队列，
 *            由 LCD 任务负责 esp_camera_fb_return()。LCD 与 Flash 共用 SPI 总线，
 *            先写 Flash 会黑屏，所以所有写 Flash 的动作都发生在帧已送出之后。
 *            - 选脸策略：面积最大且质量合格（M2 起），避免远处路人抢占识别目标。
 *            - 模型对象只在本任务栈内创建/使用，其他任务通过 face_service 命令访问。
 *            - 识别策略：
 *                AUTO   —— 稳定轨迹上每 FACE_AUTO_INTERVAL_MS 自动识别一次；
 *                MANUAL —— 仅在收到 FACE_CMD_RECOGNIZE 时识别（按键/语音/触摸）。
 *            - 活体：每帧喂入统计（纹理/运动/特征一致），新轨迹时重置窗口。
 *            - 开门：M2 只弹出覆盖层（模拟），预留 APP_CFG_DOOR_RELAY_GPIO。
 */

#include <stdio.h>
#include <string.h>
#include <vector>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "access_log.h"
#include "app_config.h"
#include "app_nvs.h"
#include "door_controller.h"
#include "face_internal.h"
#include "face_liveness.h"
#include "face_quality.h"
#include "face_tracker.h"
#include "face_recognition_112_v1_s8.hpp"
#include "face_selftest.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "human_face_detect_mnp01.hpp"
#include "human_face_detect_msr01.hpp"
#include "snapshot.h"
#include "sys_monitor.h"
#include "ui_app.h"
#include "ui_home.h"
#include "ui_home_api.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "voice_assistant.h"
#include "voice_prompt.h"
#include "who_ai_utils.hpp"

static const char *TAG = "face_task";

/* ==================== 任务参数 ==================== */
/* 栈需容纳：检测/识别模型的前后处理临时张量、std::list/vector、人脸库读取（约 1.7 KB/条）。
 * 原 8 KB 在识别 + 活体同时工作时余量不足，扩到 16 KB；M4 接入 WiFi 后内部 RAM 紧张，
 * 实测运行期水位仍剩 13.8 KB（见 sys_monitor 的栈水位行），故回收到 12 KB。
 * 若以后日志里 face_task 剩余栈 <2 KB，必须调回去。
 * M4 第二次回收：识别/注册时剩 ~8.7 KB，按实测再降到 10 KB 给 WiFi 让内部 RAM；
 * 触发时若仍低于 2 KB 必须调回 12 KB。栈与 TCB 改为静态分配（见 face_task_start）。 */
#define FACE_TASK_STACK         (10 * 1024)
#define FACE_TASK_PRIO          5
#define FACE_TASK_CORE          0           /**< 与 LVGL 同核：推理时 LVGL 让出 CPU 给 IDLE */
#define FACE_FRAME_TIMEOUT_MS   100         /**< 取帧等待（兼顾命令响应） */
#define FACE_AUTO_INTERVAL_MS   1000        /**< 自动识别间隔 */
#define FACE_OVERLAY_COOLDOWN_MS 6000       /**< 自动开门覆盖层最小间隔 */
#define FACE_LIVENESS_INTERVAL_MS 100       /**< 活体统计最低更新间隔（≤10 Hz） */
#define FACE_LIVENESS_GATE_FRAMES 5         /**< 活体持续“样本不足”时，累计的识别帧数阈值 */
#define FACE_LIVENESS_GATE_MS   4000        /**< 且累计观察时间不少于该值才判拒绝（防误杀） */
#define FACE_MAX_DRAW_FACES     4           /**< 预览最多绘制的人脸框数 */
#define FACE_SELECT_MIN_SIDE_PX 24          /**< 选脸时忽略短边小于此值的框（远处路人/误检） */
#define FACE_RESULT_TEXT_HOLD_MS 6000       /**< 手动识别结果文字在状态卡上的保护期 */

/**
 * @brief 非识别帧（只做跟踪/质量/预览）之间的最小间隔（毫秒）
 * @note  检测一次约 20~70 ms，而摄像头出帧 ~12 fps（每帧 80 ms 左右）。如果每个
 *        到达的帧都跑检测，face_task 会长时间占住 Core0：LVGL 任务（Core0、优先级 0）
 *        抢不到 CPU，界面与预览表现为“一帧一顿”，严重时 face_task 饿死 IDLE0
 *        触发任务看门狗（现场日志：detect_avg 73 ms、preview 0.1 fps、IDLE0 WDT）。
 *        按 200 ms 节流后：检测只占约 1/3 的 CPU 时间，其余帧直接送预览，
 *        状态卡与人脸框每 200 ms 刷新一次（5 Hz），人眼几乎无感。
 *        命令帧（手动识别/注册）不受影响，收到命令的那一帧一定做检测。
 */
#define FACE_TRACK_INTERVAL_MS 200

/**
 * @brief MSR01 的输入缩放系数（0.3 = 把 320x240 帧缩成 96x72 再送网络）
 * @note  这是“一直显示未检测到人脸”的根因所在，不能随便调小：
 *        MSR01 是单阶段检测器，输入被缩小后小人脸就只剩十几个像素而彻底消失。
 *        用模型自检（face_selftest.cpp 路径 4）在 QVGA 画布上实测：
 *          - 站近（脸高约 58 px）：scale 0.3 → 检不出；0.5 → 检出 0.61；1.0 → 检出 0.71
 *          - 站远（脸高约 36 px）：scale 0.3/0.5 → 检不出；1.0 → 检出 0.58
 *        以及同一帧上的实测耗时（320x240 输入）：0.3 → 约 18 ms；0.5 → 约 50 ms；
 *        1.0 → 约 208 ms（此时整条流水线掉到 3 fps、预览停摆，不能接受）。
 *        综合取 0.5：能覆盖门口正常距离（人脸 ≥100 px）的情况，单帧 50 ms 也仍在
 *        12 fps 流水线的预算内。真人脸更小（>1.5 m）时仍可能漏检，那是硬件的
 *        分辨率上限；若确有必要可临时调到 1.0 验证（此时应同时降低帧率预期）。
 *        若后续换更大分辨率的摄像头（如 VGA），必须先重跑路径 4 再定这个值。
 */
#define FACE_DETECT_RESIZE_SCALE 0.5F

/** @brief MSR01 候选分数门限（0.3 与 esp-dl 例程一致；越小越容易出候选、误检也越多） */
#define FACE_DETECT_SCORE_THR    0.3F

/**
 * @brief 检测链路调试开关：置 1 时每 FACE_DETECT_LOG_MS 打印一次检测计数
 * @note  长期没有识别日志时用它区分“画面里确实没人脸”与“有人脸但检测不出”：
 *        打印 MSR01 候选数、MNP01 结果数、选中框坐标、质量明细与逐级门限。
 *        正常固件保持 0（只多一行 printf，开销可忽略，但会刷串口）。
 */
#ifndef FACE_DETECT_DEBUG
#define FACE_DETECT_DEBUG 0
#endif
#define FACE_DETECT_LOG_MS      2000        /**< 检测统计打印间隔 */

/**
 * @brief 重量级调试输出开关（缩略图、字节序交换复检、抓拍 JPEG 的 Base64 串口回传）
 * @note  这些输出能直接看到帧缓冲内容，但代价很高：
 *        - ASCII 缩略图每 2 s 60 行日志 ≈ 3 KB，115200 串口要 0.3 s；
 *        - 交换字节序复检每 20 s 多跑一次完整推理并申请 150 KB PSRAM；
 *        - 抓拍 JPEG 的 Base64 回传约 23 KB，串口阻塞 3 s 以上——这段时间面任务是
 *          停摆的，预览会明显卡顿（容易被误认成“闪屏”）。
 *        因此默认关闭，只在需要确认“帧数据是否损坏/字节序是否对”时临时置 1。
 */
#ifndef FACE_DETECT_HEAVY_DEBUG
#define FACE_DETECT_HEAVY_DEBUG 0
#endif

/** @brief 队列句柄（face_task_start 设置） */
static QueueHandle_t s_camera_queue = NULL;
static QueueHandle_t s_display_queue = NULL;
static QueueHandle_t s_cmd_queue = NULL;

/** @brief 自动开门的覆盖层冷却（只在本任务内访问） */
static int64_t s_last_overlay_ms = 0;

/** @brief 当前轨迹最近一次的识别结论（供后续帧展示活体/相似度） */
static face_action_t s_track_action = FACE_ACTION_NONE;
/** @brief 当前轨迹是否已记录过通行/拒绝/陌生人（同一轨迹只记一次） */
static bool s_track_access_done = false;
static bool s_track_deny_done = false;
static bool s_track_stranger_done = false;
/** @brief 已匹配人员、但活体仍在“样本不足”状态下累计的帧数与起始时刻（兜底拒绝用） */
static uint32_t s_liveness_pending_frames = 0;
static int64_t s_liveness_pending_since_ms = 0;

/** @brief 同一轨迹连续“未注册”的识别帧数（满 APP_CFG_STRANGER_CONFIRM_FRAMES 确认陌生人） */
static uint32_t s_stranger_frames = 0;
/** @brief 当前轨迹最近一次陌生人告警时间与轨迹号（冷却用） */
static int64_t s_stranger_alert_ms = 0;
static int s_stranger_alert_track = -1;
/** @brief 最近一次向主页状态卡写“识别结果类文字”的时刻（结果保护期用） */
static int64_t s_result_text_ms = 0;

/** @brief 上一次真正送模型检测的时刻（跟踪节流用） */
static int64_t s_last_track_ms = 0;
/**
 * @brief 最近一次检测出的框（节流跳过的帧用它补画，避免预览里的框闪一下没一下）
 * @note  只在 face_task 内读写；容量与预览绘制上限一致。
 */
static std::list<dl::detect::result_t> s_last_draw;

#if FACE_DETECT_DEBUG
/** @brief 上一次检测统计打印时间 */
static int64_t s_detect_log_ms = 0;
/** @brief 模型自检是否已运行（首帧到达后跑一次） */
static bool s_model_selftest_done = false;
#endif

#if FACE_DETECT_HEAVY_DEBUG
/**
 * @brief 把整帧按 40x15 个采样块打印成亮度缩略图（每行一条日志）
 * @note  只用于离线判断摄像头数据本身是否有效：若缩略图是规律的人形/场景轮廓，
 *        说明帧缓冲正常，问题在显示路径；若是纯噪点、整块同值或明显错位，
 *        说明帧数据损坏（DMA/缓存）或字节序/格式不对。
 *        在缩略图之后另起一行打印采样像素的十六进制，用来判断字节序：
 *        GC0308 RGB565 是“高字节在前”（首字节 = RRRRRGGG），ESP-DL 与
 *        img_converters 默认按“低字节在前”（首字节 = GGGBBBBB）解释。
 */
static void dump_frame_ascii(const camera_fb_t *frame)
{
    static const char ramp[] = " .:-=+*#%@";
    const int cols = 40;
    const int rows = 15;
    const int step_x = (int)frame->width / cols;
    const int step_y = (int)frame->height / rows;
    char line[cols + 1];
    ESP_LOGI(TAG, "[缩略图] %ux%d %u 字节，PSRAM 地址 %p，整帧亮度 %.0f", frame->width,
             frame->height, (unsigned)frame->len, frame->buf,
             face_frame_brightness((const uint16_t *)frame->buf, frame->width, frame->height));
    for (int r = 0; r < rows; r++)
    {
        for (int c = 0; c < cols; c++)
        {
            // 每块取中心像素。摄像头是“高字节在前”（首字节 = RRRRRGGG），
            // 而小端 CPU 按 uint16 读会把两字节交换，所以这里按字节显式拼成
            // “位域正确”的值来算亮度，缩略图才与实际画面一致。
            const int x = c * step_x + step_x / 2;
            const int y = r * step_y + step_y / 2;
            const uint8_t *p = frame->buf + ((size_t)y * frame->width + x) * 2;
            const uint16_t v = (uint16_t)((p[0] << 8) | p[1]);
            const int r5 = (v >> 11) & 0x1F;
            const int g6 = (v >> 5) & 0x3F;
            const int b5 = v & 0x1F;
            const int luma = (r5 * 77 * 8 + g6 * 150 * 4 + b5 * 29 * 8) / 256;
            line[c] = ramp[luma * 9 / 255];
        }
        line[cols] = '\0';
        ESP_LOGI(TAG, "[缩略图] |%s|", line);
    }

    // 采样 10 个点（间隔 40 行，取整行宽的四分之三处），打印原始字节 + 两种解释
    char hex[128];
    int n = 0;
    for (int i = 0; i < 10 && n < 100; i++)
    {
        const int y = i * 40 + 20;
        const int x = frame->width * 3 / 4;
        if (y >= (int)frame->height)
        {
            break;
        }
        const uint8_t *p = frame->buf + ((size_t)y * frame->width + x) * 2;
        // 高字节在前解释（GC0308 实际字节序）
        const unsigned be = ((unsigned)p[0] << 8) | p[1];
        const int be_r = (be >> 11) & 0x1F, be_g = (be >> 5) & 0x3F, be_b = be & 0x1F;
        // 低字节在前解释（ESP-DL convert_pixel_rgb565_to_rgb888 的假设）
        const unsigned le = ((unsigned)p[1] << 8) | p[0];
        const int le_r = (le >> 11) & 0x1F, le_g = (le >> 5) & 0x3F, le_b = le & 0x1F;
        n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02x%02x=BE(%2d,%2d,%2d)/LE(%2d,%2d,%2d) ",
                      p[0], p[1], be_r, be_g, be_b, le_r, le_g, le_b);
    }
    ESP_LOGI(TAG, "[字节序] %s", hex);
}

/**
 * @brief 整帧色彩统计：判断摄像头送进来的到底是 RGB565 还是别的格式（如 YUV422）
 * @param frame 摄像头帧
 * @note  自然画面的 RGB565（通道扩到 8 位后）通常三通道接近，平均饱和度较低；
 *        若传感器实际输出 YUV422 却被当成 RGB565 解释，奇/偶像素分别由 Y/U 与
 *        Y/V 组成，两个通道会被推到 0/255 附近，平均饱和度飙升且亮暗成对出现。
 *        因此同时按“高字节在前（摄像头实际）”与“低字节在前”解释统计，
 *        平均饱和度高得离谱的那种解释就说明格式或字节序不对。
 */
static void dump_frame_colour_stats(const camera_fb_t *frame)
{
    const int step = 4; // 每 4 像素采样一次
    double be_sat = 0.0, le_sat = 0.0, be_lum = 0.0;
    uint32_t n = 0;
    for (uint32_t y = 0; y < frame->height; y += step)
    {
        const uint8_t *row = frame->buf + (size_t)y * frame->width * 2;
        for (uint32_t x = 0; x < frame->width; x += step)
        {
            const uint8_t *p = row + (size_t)x * 2;
            for (int mode = 0; mode < 2; mode++)
            {
                const uint16_t v = mode == 0 ? (uint16_t)((p[0] << 8) | p[1])
                                             : (uint16_t)((p[1] << 8) | p[0]);
                const int r5 = (v >> 11) & 0x1F;
                const int g6 = (v >> 5) & 0x3F;
                const int b5 = v & 0x1F;
                const int r8 = (r5 << 3) | (r5 >> 2);
                const int g8 = (g6 << 2) | (g6 >> 4);
                const int b8 = (b5 << 3) | (b5 >> 2);
                const int hi = r8 > g8 ? (r8 > b8 ? r8 : b8) : (g8 > b8 ? g8 : b8);
                const int lo = r8 < g8 ? (r8 < b8 ? r8 : b8) : (g8 < b8 ? g8 : b8);
                if (mode == 0)
                {
                    be_sat += hi - lo;
                    be_lum += 0.299 * r8 + 0.587 * g8 + 0.114 * b8;
                }
                else
                {
                    le_sat += hi - lo;
                }
            }
            n++;
        }
    }
    ESP_LOGI(TAG, "[色彩] format=%d len=%u：高字节在前 饱和度 %.1f 亮度 %.0f；低字节在前 饱和度 %.1f",
             (int)frame->format, (unsigned)frame->len, be_sat / (double)n, be_lum / (double)n,
             le_sat / (double)n);
    ESP_LOGI(TAG, "[色彩] 样本：%02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x",
             frame->buf[0], frame->buf[1], frame->buf[2], frame->buf[3], frame->buf[4],
             frame->buf[5], frame->buf[6], frame->buf[7], frame->buf[8], frame->buf[9],
             frame->buf[10], frame->buf[11]);
}

/** @brief 抓拍拷贝（PSRAM，一帧 RGB565）与下一次抓拍的时刻 */
static uint16_t *s_shot_copy = NULL;
static int s_shot_w = 0;
static int s_shot_h = 0;
static int64_t s_shot_next_ms = 10000; /* 上电 10 s 后第一次抓拍 */
static bool s_shot_done = false;       /* 已抓过一张（调试只抓一次） */

/**
 * @brief 把当前帧拷到 PSRAM 拷贝缓冲（必须在帧送显示队列之前调用）
 * @param frame 摄像头帧
 * @return true 已拷贝，可以排队写盘
 * @note  不能等到帧送出之后再编码：LCD 任务会立刻 esp_camera_fb_return()，
 *        帧缓冲可能已被摄像头复用，编码出来是撕裂画面。
 */
static bool shot_copy_frame(const camera_fb_t *frame)
{
    const size_t bytes = (size_t)frame->width * frame->height * sizeof(uint16_t);
    if (s_shot_copy == NULL)
    {
        s_shot_copy = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_shot_copy == NULL)
        {
            ESP_LOGE(TAG, "[抓拍] PSRAM 拷贝缓冲分配失败（%u 字节）", (unsigned)bytes);
            return false;
        }
    }
    memcpy(s_shot_copy, frame->buf, bytes);
    s_shot_w = (int)frame->width;
    s_shot_h = (int)frame->height;
    return true;
}

/**
 * @brief 把二进制数据按 Base64 分块打印到串口（每行带序号，便于主机侧拼接）
 * @param tag  行首标记
 * @param data 数据
 * @param len  长度
 * @note  诊断用：串口日志每行由 ESP_LOG 加锁输出，不会与其他任务的日志交错，
 *        主机侧只需收集以 tag 开头的行再解码即可还原 JPEG。
 */
static void dump_base64(const char *tag, const uint8_t *data, size_t len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[200];
    size_t off = 0;
    unsigned idx = 0;
    while (off < len)
    {
        int n = snprintf(line, sizeof(line), "%s %04u ", tag, idx++);
        for (int k = 0; k < 120 && off < len; k += 3)
        {
            const size_t rem = len - off;
            const unsigned b0 = data[off];
            const unsigned b1 = rem > 1 ? data[off + 1] : 0;
            const unsigned b2 = rem > 2 ? data[off + 2] : 0;
            const unsigned v = (b0 << 16) | (b1 << 8) | b2;
            line[n++] = tbl[(v >> 18) & 0x3F];
            line[n++] = tbl[(v >> 12) & 0x3F];
            line[n++] = rem > 1 ? tbl[(v >> 6) & 0x3F] : '=';
            line[n++] = rem > 2 ? tbl[v & 0x3F] : '=';
            off += rem > 2 ? 3 : rem;
        }
        line[n] = '\0';
        ESP_LOGI(TAG, "%s", line);
    }
}

/**
 * @brief 把抓拍拷贝编码成 JPEG 写到 /spiffs/snap/cam.jpg（帧已送出后才调用）
 * @note  fmt2jpg 的 RGB565 输入默认按“高字节在前”解释，与 GC0308 一致，
 *        所以画面颜色应与预览一致；若 JPEG 颜色明显异常，说明字节序假设不对。
 */
static void shot_write_jpeg(void)
{
    if (s_shot_copy == NULL)
    {
        return; // 只抓一张，避免每次 JPEG 编码（会关闭 Flash cache）拖慢流水线
    }
    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    if (fmt2jpg((uint8_t *)s_shot_copy, (size_t)s_shot_w * s_shot_h * 2, (uint16_t)s_shot_w,
                (uint16_t)s_shot_h, PIXFORMAT_RGB565, 80, &jpg, &jpg_len) &&
        jpg != NULL)
    {
        FILE *f = fopen("/spiffs/snap/cam.jpg", "wb");
        if (f != NULL)
        {
            const bool ok = fwrite(jpg, 1, jpg_len, f) == jpg_len;
            fclose(f);
            ESP_LOGI(TAG, "[抓拍] cam.jpg 写入%s（%u 字节）", ok ? "成功" : "失败",
                     (unsigned)jpg_len);
        }
        else
        {
            ESP_LOGE(TAG, "[抓拍] 打开 /spiffs/snap/cam.jpg 失败");
        }
        // 同时把 JPEG 以 Base64 打到串口，主机侧解码即可查看画面（调试用）
        ESP_LOGI(TAG, "[抓拍] JPEG Base64 开始：%u 字节，%u 行", (unsigned)jpg_len,
                 (unsigned)((jpg_len + 119) / 120));
        dump_base64("JPG64", jpg, jpg_len);
        ESP_LOGI(TAG, "[抓拍] JPEG Base64 结束");
        free(jpg);
        s_shot_done = true;
        heap_caps_free(s_shot_copy); // 拷贝缓冲只用于这一次抓拍
        s_shot_copy = NULL;
    }
    else
    {
        ESP_LOGE(TAG, "[抓拍] JPEG 编码失败");
    }
}

/** @brief 字节交换缓冲（验证 ESP-DL 的 16 位字序假设用） */
static uint16_t *s_swap_buf = NULL;

/**
 * @brief 把整帧按 16 位字互换字节后再跑一次 MSR01，返回候选数
 * @param detector 检测器
 * @param frame    当前帧
 * @return 交换字节序后的候选数
 * @note  诊断用：若原字节序候选数为 0、交换后能检出人脸，说明 ESP-DL 的 RGB565
 *        解释与摄像头字节序不一致，需要在前处理里统一交换。
 */
static size_t detect_swapped_bytes(HumanFaceDetectMSR01 &detector, const camera_fb_t *frame)
{
    const size_t n = (size_t)frame->width * frame->height;
    if (s_swap_buf == NULL)
    {
        s_swap_buf = (uint16_t *)heap_caps_malloc(n * sizeof(uint16_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_swap_buf == NULL)
        {
            ESP_LOGE(TAG, "[字节序] 交换缓冲分配失败");
            return 0;
        }
    }
    const uint16_t *src = (const uint16_t *)frame->buf;
    for (size_t i = 0; i < n; i++)
    {
        const uint16_t v = src[i];
        s_swap_buf[i] = (uint16_t)((v >> 8) | (v << 8));
    }
    std::list<dl::detect::result_t> &r =
        detector.infer(s_swap_buf, {(int)frame->height, (int)frame->width, 3});
    return r.size();
}
#endif /* FACE_DETECT_HEAVY_DEBUG */
/** @brief 全局抓拍节流 */
static int64_t s_last_snap_ms = 0;
static int64_t s_snap_hour_start_ms = 0;
static uint32_t s_snap_hour_count = 0;
static char s_track_name[32] = "";
static float s_track_similarity = -1.0f;

/** @brief 当前帧上下文 */
struct face_frame_ctx_t
{
    camera_fb_t *frame;
    int64_t now_ms;
    bool present;
    int32_t x, y, w, h;
    std::vector<int> landmarks;
    face_quality_t quality;
    face_track_t track;
    face_frame_ctx_t() : frame(NULL), now_ms(0), present(false), x(0), y(0), w(0), h(0)
    {
        memset(&quality, 0, sizeof(quality));
        memset(&track, 0, sizeof(track));
        track.track_id = -1;
    }
};

/**
 * @brief 选取面积最大的人脸
 * @return 选中结果的指针；无人脸返回 NULL
 * @note  短边小于 FACE_SELECT_MIN_SIDE_PX 的框直接忽略：这类框要么是远处路人
 *        （既不可能通过质量门限，也不该抢占识别目标），要么是纹理误检。
 *        若全部框都被忽略，本帧按“无人脸”处理，状态卡显示“未检测到人脸”，
 *        等待访客走近。
 */
static dl::detect::result_t *select_largest(std::list<dl::detect::result_t> &results)
{
    dl::detect::result_t *best = NULL;
    int64_t best_area = 0;
    for (auto &r : results)
    {
        if (r.box.size() < 4)
        {
            continue;
        }
        const int32_t bw = r.box[2] - r.box[0];
        const int32_t bh = r.box[3] - r.box[1];
        if (bw < FACE_SELECT_MIN_SIDE_PX || bh < FACE_SELECT_MIN_SIDE_PX)
        {
            continue;
        }
        const int64_t area = (int64_t)bw * (int64_t)bh;
        if (best == NULL || area > best_area)
        {
            best = &r;
            best_area = area;
        }
    }
    return best;
}

/**
 * @brief 更新上下文：选脸、质量、跟踪、绘制、主页状态卡
 * @param ctx        帧上下文
 * @param results    MNP01 检测结果
 * @param enrolled   已注册人数
 */
static void analyze_frame(face_frame_ctx_t &ctx, std::list<dl::detect::result_t> &results,
                          int enrolled)
{
    dl::detect::result_t *sel = select_largest(results);

    ctx.present = false;
    ctx.landmarks.clear();

    if (sel != NULL)
    {
        const int32_t x = sel->box[0];
        const int32_t y = sel->box[1];
        const int32_t w = sel->box[2] - sel->box[0];
        const int32_t h = sel->box[3] - sel->box[1];

        std::vector<int32_t> kp32;
        const int32_t *kpts = NULL;
        if (sel->keypoint.size() == 10)
        {
            kp32.assign(sel->keypoint.begin(), sel->keypoint.end());
            kpts = kp32.data();
            ctx.landmarks.assign(sel->keypoint.begin(), sel->keypoint.end());
        }

        ctx.present = face_quality_eval((const uint16_t *)ctx.frame->buf, ctx.frame->width,
                                        ctx.frame->height, x, y, w, h, kpts, &ctx.quality);
        if (ctx.present)
        {
            ctx.x = ctx.quality.x;
            ctx.y = ctx.quality.y;
            ctx.w = ctx.quality.w;
            ctx.h = ctx.quality.h;
        }
    }

    face_tracker_update(ctx.present, ctx.x, ctx.y, ctx.w, ctx.h, ctx.now_ms, &ctx.track);
    // 新轨迹：清空上一轨迹的识别结论，避免把“上一位访客”的结果显示给新访客
    if (ctx.track.is_new)
    {
        s_track_action = FACE_ACTION_NONE;
        s_track_name[0] = '\0';
        s_track_similarity = -1.0f;
        s_liveness_pending_frames = 0;
        s_stranger_frames = 0;
        s_track_access_done = false;
        s_track_deny_done = false;
        s_track_stranger_done = false;
    }

    // ---- 在帧上绘制候选框（供预览观察） ----
    if (!results.empty())
    {
        std::list<dl::detect::result_t> draw;
        int n = 0;
        for (auto &r : results)
        {
            draw.push_back(r);
            if (++n >= FACE_MAX_DRAW_FACES)
            {
                break;
            }
        }
        draw_detection_result((uint16_t *)ctx.frame->buf, ctx.frame->height, ctx.frame->width, draw);
        // 留一份给节流跳过的帧补画（见主循环的“跟踪节流”分支）
        s_last_draw = draw;
    }
    else
    {
        // 本帧确实没有检测结果：清掉旧框，否则跳帧时会一直画着上一个人的框
        s_last_draw.clear();
    }

    // ---- 更新当前人脸状态快照与主页状态卡 ----
    face_face_state_t st;
    memset(&st, 0, sizeof(st));
    st.face_present = ctx.present;
    st.quality_pass = ctx.present && ctx.quality.pass;
    st.track_id = ctx.track.track_id;
    st.action = ctx.present ? s_track_action : FACE_ACTION_NONE;
    strncpy(st.name, s_track_name, sizeof(st.name) - 1);
    st.similarity = s_track_similarity;
    st.enrolled_num = enrolled;
    face_service_update_face_state(&st);

    if (!ctx.present)
    {
#if FACE_DETECT_DEBUG
        // 调试：若检测器有输出但被质量门限判为“无人脸”，状态卡会一直显示“未检测到人脸”，
        // 极易与“检测不到”混淆。这里明确区分两种情况（正式固件不打印）。
        if (sel != NULL)
        {
            static int64_t s_qfail_log_ms = 0;
            if (ctx.now_ms - s_qfail_log_ms >= FACE_DETECT_LOG_MS)
            {
                s_qfail_log_ms = ctx.now_ms;
                ESP_LOGW(TAG, "[质量] 检出人脸但质量评估失败（框(%d,%d,%d,%d)，原始框 %d,%d,%d,%d），"
                              "界面会显示“未检测到人脸”，易与真正的漏检混淆",
                         (int)ctx.x, (int)ctx.y, (int)ctx.w, (int)ctx.h, (int)sel->box[0],
                         (int)sel->box[1], (int)sel->box[2], (int)sel->box[3]);
            }
        }
#endif
        ui_home_set_state(UI_TXT_FACE_NO_FACE, UI_COLOR_TEXT_DIM);
    }
    else if (!ctx.quality.size_ok)
    {
        ui_home_set_state(UI_TXT_FACE_TOO_FAR, UI_COLOR_WARN);
    }
    else if (!ctx.quality.pass)
    {
        ui_home_set_state(UI_TXT_FACE_LOW_QUALITY, UI_COLOR_WARN);
    }
    else if (s_track_action == FACE_ACTION_NONE && face_task_strategy() == FACE_STRATEGY_MANUAL)
    {
        // 手动识别模式：人脸质量合格但还没点“开始识别”（或结果已过期）。
        // 只显示一句提示，不进入“识别中”——主页平时只显示摄像头画面。
        //
        // 但识别中/刚出结果的那几秒不能被覆盖：点按钮后到 face_task 回复之间，
        // 每帧的 analyze_frame 都会跑到这里；若无条件写“请点开始识别”，
        // 紧接着 apply_result 写的“识别成功 xx%”会立刻被下一帧盖掉，用户看不到结论。
        // 这里按时间窗保护：识别请求在途（ui_home_recognize_busy）或 6 s 内写过结果时，
        // 不覆盖状态卡。
        const int64_t now_ms = ctx.now_ms;
        if (!ui_home_recognize_busy() && (now_ms - s_result_text_ms) >= FACE_RESULT_TEXT_HOLD_MS)
        {
            // 人脸库为空时提示先注册，否则提示点按钮识别
            ui_home_set_state(enrolled > 0 ? UI_TXT_HOME_FACE_READY : UI_TXT_HOME_NO_ENROLLED,
                              UI_COLOR_TEXT_DIM);
        }
    }
    else
    {
        switch (s_track_action)
        {
        case FACE_ACTION_OPEN:
        {
            char text[64];
            snprintf(text, sizeof(text), UI_FMT_RECOGNIZE, s_track_name, s_track_similarity * 100.0f);
            ui_home_set_state(text, UI_COLOR_OK);
            break;
        }
        case FACE_ACTION_DENY_LIVENESS:
            ui_home_set_state(UI_TXT_FACE_LIVENESS_DENY, UI_COLOR_DANGER);
            break;
        case FACE_ACTION_UNKNOWN:
            ui_home_set_state(UI_TXT_FACE_UNKNOWN, UI_COLOR_WARN);
            break;
        default:
            // 自动模式：稳定轨迹正在等下一轮自动识别 → 显示“识别中”
            // 手动模式：本条轨迹已识别过、结论是“活体样本继续累积（NONE）”→ 同上
            ui_home_set_state(face_task_strategy() == FACE_STRATEGY_AUTO ? UI_TXT_HOME_RECOGNIZING
                                                                       : UI_TXT_FACE_LIVENESS_RUN,
                              UI_COLOR_ACCENT);
            break;
        }
        // 结果类文字（开门/拒绝/未识别/活体检测中）都记时间，供上面的“请点开始识别”
        // 判定是否需要让位，避免刚出的结论被同一秒内的下一帧覆盖。
        s_result_text_ms = ctx.now_ms;
    }
}

/**
 * @brief 活体决策：只有“确认活体通过”才开门，“样本不足”不判死
 * @param matched 是否识别为已注册人员
 * @param live    活体结论
 * @return 动作
 *
 * @note   - 样本不足（LIVENESS_RESULT_UNKNOWN）时返回 FACE_ACTION_NONE：
 *            界面显示“活体检测中”，既不开门也不拒绝。自动识别每秒复核一次，
 *            活体采样达到 5 帧（约 0.5 s）后便会给出 LIVE/SPOOF 的确定结论，
 *            因此真人最多多等一个识别周期，而照片/屏幕不会靠一两帧蒙混过关。
 *          - 只有 SPOOF 才返回拒绝；拒绝覆盖层因此不会在真人刚走到镜头前误弹。
 */
static face_action_t decide_liveness_action(bool matched, liveness_result_t live)
{
    if (matched && !face_liveness_is_enabled())
    {
        return FACE_ACTION_OPEN;
    }
    if (!matched)
    {
        return FACE_ACTION_UNKNOWN;
    }
    switch (live)
    {
    case LIVENESS_RESULT_LIVE:
        return FACE_ACTION_OPEN;
    case LIVENESS_RESULT_SPOOF:
        return FACE_ACTION_DENY_LIVENESS;
    default:
        return FACE_ACTION_NONE;
    }
}

/**
 * @brief 由识别结果 + 活体报告生成对外结果
 */
static face_result_t build_result(const face_frame_ctx_t &ctx, const face_info_t &info,
                                  bool matched)
{
    face_result_t res;
    memset(&res, 0, sizeof(res));
    res.id = matched ? info.id : -1;
    if (matched)
    {
        strncpy(res.name, info.name.c_str(), sizeof(res.name) - 1);
        res.similarity = info.similarity;
    }
    else
    {
        res.similarity = -1.0f;
    }
    res.track_id = ctx.track.track_id;
    res.ts_ms = (uint64_t)ctx.now_ms;
    res.x = ctx.x;
    res.y = ctx.y;
    res.w = ctx.w;
    res.h = ctx.h;
    res.quality_pass = ctx.quality.pass;

    liveness_report_t rep;
    face_liveness_get_report(&rep);
    const liveness_result_t live = rep.result;
    res.liveness = face_liveness_is_enabled() ? rep.total : 100.0f;

    res.action = matched ? decide_liveness_action(true, live) : FACE_ACTION_UNKNOWN;
    return res;
}

/**
 * @brief 应用结果：状态快照、主页文字、开门覆盖层
 * @param ctx       帧上下文
 * @param res       识别结果
 * @param enrolled  已注册人数
 * @param manual    true = 手动触发（覆盖层不受冷却限制）
 */
static void apply_result(face_frame_ctx_t &ctx, const face_result_t &res, int enrolled, bool manual)
{
    s_track_action = res.action;
    strncpy(s_track_name, res.name, sizeof(s_track_name) - 1);
    s_track_name[sizeof(s_track_name) - 1] = '\0';
    s_track_similarity = res.similarity;

    face_face_state_t st;
    memset(&st, 0, sizeof(st));
    st.face_present = true;
    st.quality_pass = true;
    st.track_id = ctx.track.track_id;
    st.action = res.action;
    strncpy(st.name, res.name, sizeof(st.name) - 1);
    st.similarity = res.similarity;
    st.enrolled_num = enrolled;
    face_service_update_face_state(&st);

    switch (res.action)
    {
    case FACE_ACTION_OPEN:
        ui_home_set_state(UI_TXT_FACE_RECOGNIZED, UI_COLOR_OK);
        break;
    case FACE_ACTION_DENY_LIVENESS:
        ui_home_set_state(UI_TXT_FACE_LIVENESS_DENY, UI_COLOR_DANGER);
        break;
    case FACE_ACTION_NONE:
        // 已匹配到人、但活体样本还在累积：显示“活体检测中”，不开门也不拒绝
        ui_home_set_state(res.name[0] ? UI_TXT_FACE_LIVENESS_RUN : UI_TXT_HOME_RECOGNIZING,
                          UI_COLOR_ACCENT);
        break;
    default:
        ui_home_set_state(UI_TXT_FACE_UNKNOWN, UI_COLOR_WARN);
        break;
    }
    // 结果文字保护期起点：apply_result 在识别帧上写结论，随后 analyze_frame 每帧都会
    // 经过“请点开始识别”分支，必须让它知道刚写过结果、不要立刻覆盖。
    s_result_text_ms = ctx.now_ms;

    // ---- 门禁事件：覆盖层立即弹出（画面与识别同步），日志/计数由 door_controller 异步落盘 ----
    const bool allow_overlay = (ctx.now_ms - s_last_overlay_ms) >= FACE_OVERLAY_COOLDOWN_MS;
    if (res.action == FACE_ACTION_OPEN && !s_track_access_done && res.name[0] != '\0' &&
        (manual || allow_overlay))
    {
        s_track_access_done = true;
        s_last_overlay_ms = ctx.now_ms;

        char sub[48];
        snprintf(sub, sizeof(sub), UI_FMT_RECOGNIZE, res.name, res.similarity * 100.0f);
        ui_app_show_door_overlay(UI_TXT_DOOR_OPEN, sub, true, APP_CFG_DOOR_OPEN_SIM_MS);

        const door_open_info_t info = {
            .source = DOOR_SOURCE_FACE,
            .name = res.name[0] ? res.name : NULL,
            .similarity = res.similarity,
            .liveness = res.liveness,
            .extra = NULL,
            .track_id = res.track_id,
        };
        (void)door_controller_open(&info);
        ESP_LOGI(TAG, "模拟开门：%s（相似度 %.2f，活体 %.0f）", res.name, res.similarity,
                 res.liveness);
        /* 语音播报只在**真正开门**时响一次：同一条轨迹的后续识别帧不再重复播报。
         * 播报本身是非阻塞投递（voice_prompt 的常驻线程负责播放），face_task 不会
         * 被喇叭拖慢帧率；播放期间 audio_service 置 AUDIO_BUSY，语音任务自动暂停。 */
        (void)voice_prompt_say(VOICE_PROMPT_OPEN);
    }
    else if (res.action == FACE_ACTION_DENY_LIVENESS && !s_track_deny_done &&
             (manual || allow_overlay))
    {
        s_track_deny_done = true;
        s_last_overlay_ms = ctx.now_ms;
        ui_app_show_door_overlay(UI_TXT_DOOR_DENY, UI_TXT_DOOR_LIVENESS_FAIL, false,
                                 APP_CFG_DOOR_OPEN_SIM_MS);

        const door_deny_info_t deny = {
            .name = res.name[0] ? res.name : NULL,
            .similarity = res.similarity,
            .liveness = res.liveness,
            .liveness_fail = true,
            .snap = NULL,
            .track_id = res.track_id,
        };
        (void)door_controller_deny(&deny);
        (void)voice_prompt_say(VOICE_PROMPT_LIVENESS);
    }
}

/**
 * @brief 陌生人判定与抓拍（M3）
 * @param ctx 帧上下文（调用方保证 present 且质量合格）
 * @param res 本帧识别结果（未匹配）
 *
 * @note   - 同一轨迹连续 APP_CFG_STRANGER_CONFIRM_FRAMES 个识别帧都未注册 → 确认陌生人；
 *          - 同一轨迹冷却 APP_CFG_STRANGER_COOLDOWN_MS，全局抓拍间隔 APP_CFG_SNAP_GLOBAL_GAP_MS，
 *            每小时上限 APP_CFG_SNAP_HOURLY_MAX 张（避免门口有人长时间停留时刷屏）；
 *          - 抓拍在**帧送出显示队列之前**完成（JPEG 编码需要读帧缓冲，返回后帧即被回收）；
 *            编码约 30~80 ms，只发生在确认陌生人那一帧；
 *          - 抓拍开关来自 NVS（app_nvs_get_snapshot_enabled），关闭时仍记录陌生人日志。
 */
static void handle_stranger(face_frame_ctx_t &ctx, const face_result_t &res)
{
    if (s_track_stranger_done)
    {
        return;
    }
    if (++s_stranger_frames < APP_CFG_STRANGER_CONFIRM_FRAMES)
    {
        return;
    }

    // 冷却：同一轨迹 60 s 内只告警一次（轨迹号变化视为新访客）
    const bool same_track = (s_stranger_alert_track == ctx.track.track_id);
    if (same_track && (ctx.now_ms - s_stranger_alert_ms) < APP_CFG_STRANGER_COOLDOWN_MS)
    {
        return;
    }
    s_stranger_alert_track = ctx.track.track_id;
    s_stranger_alert_ms = ctx.now_ms;
    s_track_stranger_done = true;

    char snap_name[32] = "";

    // 全局抓拍节流
    if (ctx.now_ms - s_snap_hour_start_ms >= 3600000)
    {
        s_snap_hour_start_ms = ctx.now_ms;
        s_snap_hour_count = 0;
    }
    const bool throttle_ok =
        (s_last_snap_ms == 0 || (ctx.now_ms - s_last_snap_ms) >= APP_CFG_SNAP_GLOBAL_GAP_MS) &&
        s_snap_hour_count < APP_CFG_SNAP_HOURLY_MAX;

    if (throttle_ok && snapshot_is_ready() && app_nvs_get_snapshot_enabled())
    {
        uint8_t *jpg = NULL;
        size_t jpg_len = 0;
        const int64_t t0 = esp_timer_get_time();
        if (frame2jpg(ctx.frame, 75, &jpg, &jpg_len) && jpg != NULL)
        {
            if (snapshot_make_name(snap_name, sizeof(snap_name), "S") == ESP_OK)
            {
                if (snapshot_submit(jpg, jpg_len, snap_name) == ESP_OK)
                {
                    s_last_snap_ms = ctx.now_ms;
                    s_snap_hour_count++;
                    ESP_LOGI(TAG, "陌生人抓拍：%s（%u 字节，编码 %d ms）", snap_name,
                             (unsigned)jpg_len, (int)((esp_timer_get_time() - t0) / 1000));
                }
                else
                {
                    snap_name[0] = '\0'; // snapshot_submit 失败时会释放 jpg
                }
            }
            else
            {
                free(jpg);
                snap_name[0] = '\0';
            }
        }
        else
        {
            ESP_LOGW(TAG, "陌生人抓拍编码失败");
        }
    }

    (void)door_controller_stranger(snap_name[0] ? snap_name : NULL, ctx.track.track_id);
    /* 确认陌生人（同一轨迹连续 APP_CFG_STRANGER_CONFIRM_FRAMES 帧未匹配）才播报，
     * 不是每一帧；同一轨迹之后由 s_track_stranger_done 拦住，不会连说 */
    (void)voice_prompt_say(VOICE_PROMPT_DENIED);
}

/**
 * @brief 活体窗口维护：新轨迹重置，按 ≥100 ms 节奏喂纹理/运动
 * @param ctx      帧上下文（要求 present 且质量合格）
 * @param emb      本帧特征（可为 NULL）
 * @param emb_len  特征长度
 * @param force    true = 识别帧，跳过 10 Hz 节流以保证一致性样本及时更新
 * @param last_ms  上次喂入时间（毫秒），跨帧保存
 */
static void liveness_tick(face_frame_ctx_t &ctx, const float *emb, int emb_len, bool force,
                          int64_t &last_ms)
{
    if (!ctx.present || !ctx.quality.pass)
    {
        return;
    }
    if (ctx.track.is_new)
    {
        face_liveness_reset();
        last_ms = 0;
    }
    if (!force && (ctx.now_ms - last_ms) < FACE_LIVENESS_INTERVAL_MS)
    {
        return;
    }
    last_ms = ctx.now_ms;
    (void)face_liveness_update((const uint16_t *)ctx.frame->buf, ctx.frame->width,
                               ctx.frame->height, &ctx.quality, emb, emb_len);
}

/**
 * @brief 识别一次当前人脸（调用方保证 ctx.present 且质量合格）
 * @param recognizer  识别器
 * @param ctx         帧上下文
 * @param manual      true = 手动触发
 * @param enrolled    已注册人数
 * @param last_live_ms 活体喂入时间戳（跨帧保存）
 * @return 识别结果
 */
static face_result_t recognize_once(FaceRecognition112V1S8 &recognizer, face_frame_ctx_t &ctx,
                                    bool manual, int enrolled, int64_t &last_live_ms)
{
    const int64_t t0 = esp_timer_get_time();
    face_info_t info = recognizer.recognize((uint16_t *)ctx.frame->buf,
                                            {(int)ctx.frame->height, (int)ctx.frame->width, 3},
                                            ctx.landmarks);
    sys_monitor_report_recognize((uint32_t)(esp_timer_get_time() - t0));

    // 识别帧强制喂入活体（附带特征用于“一致性”指标）。
    // 注意：get_face_emb(id) 对匹配成功返回的是“已注册模板”，跨帧恒定；
    // 一致性需要本帧特征，因此始终取 id=-1（最近一次输入的 embedding）。
    std::vector<float> emb;
    dl::Tensor<float> &t = recognizer.get_face_emb(-1);
    if (t.get_size() > 0 && t.element != NULL)
    {
        emb.assign(t.element, t.element + t.get_size());
    }
    liveness_tick(ctx, emb.empty() ? NULL : emb.data(), (int)emb.size(), true, last_live_ms);

    face_result_t res = build_result(ctx, info, info.id >= 0);
    liveness_report_t rep_before;
    face_liveness_get_report(&rep_before);

    // 兜底：已匹配人员、活体长期停在“样本不足”（既非 LIVE 也非 SPOOF）时，
    // 累计足够帧数与时长后按“活体检测失败”拒绝，避免照片/屏幕长时间蒙混。
    if (res.action == FACE_ACTION_NONE)
    {
        if (s_liveness_pending_frames == 0)
        {
            s_liveness_pending_since_ms = ctx.now_ms;
        }
        s_liveness_pending_frames++;
    }
    else
    {
        s_liveness_pending_frames = 0;
    }
    if (s_liveness_pending_frames >= FACE_LIVENESS_GATE_FRAMES &&
        (ctx.now_ms - s_liveness_pending_since_ms) >= FACE_LIVENESS_GATE_MS)
    {
        ESP_LOGW(TAG, "活体长期样本不足（%u 帧，纹理 %.0f 运动 %.0f）→ 拒绝", s_liveness_pending_frames,
                 rep_before.texture_score, rep_before.motion_score);
        res.action = FACE_ACTION_DENY_LIVENESS;
        s_liveness_pending_frames = 0;
    }

    apply_result(ctx, res, enrolled, manual);
    face_service_publish_result(&res);

    if (res.id < 0)
    {
        handle_stranger(ctx, res);
    }
    else
    {
        s_stranger_frames = 0;
    }

    liveness_report_t rep;
    face_liveness_get_report(&rep);
    liveness_status_t lv_st;
    face_liveness_get_status(&lv_st);
    // 把三项原始指标一起打印：误判“活体检测失败”时，凭 纹理(比值)/运动(灰度差)/一致(余弦)
    // 就能判断是哪个门限卡住的，不必反复改阈值试错。样本数不足 5 时结论固定为 UNKNOWN。
    ESP_LOGI(TAG,
             "识别：轨迹 %d → id=%d name=%s sim=%.2f live=%.0f(%s) [纹理%.0f 比值%.2f 运动%.0f "
             "(%.2f) 一致%.0f 样本%d 综合%.0f] → %s",
             res.track_id, res.id, res.name, res.similarity, res.liveness,
             face_liveness_result_text(rep.result), rep.texture_score, lv_st.texture_ratio,
             rep.motion_score, lv_st.motion, rep.consistency_score, lv_st.sample_count, rep.total,
             face_action_text(res.action));
    return res;
}

/**
 * @brief 无帧命令（清空/删除/列表/阈值）与有帧命令共用的简单命令执行
 * @return true 已处理并回复；false 表示命令需要相机帧
 */
static bool execute_simple_cmd(FaceRecognition112V1S8 &recognizer, const face_cmd_t &cmd)
{
    switch (cmd.type)
    {
    case FACE_CMD_DELETE:
    {
        std::vector<face_info_t> ids = recognizer.get_enrolled_ids();
        bool found = false;
        for (auto &info : ids)
        {
            if (info.id == cmd.id)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            face_reply_err(ESP_ERR_NOT_FOUND);
            break;
        }
        // delete_id 的返回值语义按“剩余条数”定义，删到空时可能为 -1，
        // 因此以“目标 ID 是否仍在库中”作为成功判据。
        (void)recognizer.delete_id(cmd.id, false);
        std::vector<face_info_t> remain = recognizer.get_enrolled_ids();
        bool still_there = false;
        for (auto &info : remain)
        {
            if (info.id == cmd.id)
            {
                still_there = true;
                break;
            }
        }
        if (still_there)
        {
            face_reply_err(ESP_FAIL);
        }
        else
        {
            const int written = recognizer.write_ids_to_flash();
            ESP_LOGI(TAG, "已删除人脸 id=%d（写入 %d 条）", cmd.id, written);
            ui_home_set_counts(recognizer.get_enrolled_id_num(), -1);
            face_reply_err(written >= 0 ? ESP_OK : ESP_FAIL);
        }
        break;
    }

    case FACE_CMD_CLEAR:
    {
        std::vector<face_info_t> ids = recognizer.get_enrolled_ids();
        for (auto &info : ids)
        {
            recognizer.delete_id(info.id, false);
        }
        recognizer.write_ids_to_flash();
        ESP_LOGI(TAG, "已清空 %d 个人脸数据", (int)ids.size());
        ui_home_set_counts(0, -1);
        face_reply_err(ESP_OK);
        break;
    }

    case FACE_CMD_LIST:
    {
        std::vector<face_info_t> ids = recognizer.get_enrolled_ids();
        static face_record_t list[FACE_REPLY_LIST_MAX];
        size_t n = 0;
        for (auto &info : ids)
        {
            if (n >= FACE_REPLY_LIST_MAX)
            {
                break;
            }
            list[n].id = info.id;
            strncpy(list[n].name, info.name.c_str(), sizeof(list[n].name) - 1);
            list[n].name[sizeof(list[n].name) - 1] = '\0';
            n++;
        }
        face_reply_list(ESP_OK, list, n);
        break;
    }

    case FACE_CMD_SET_THRESHOLD:
    {
        recognizer.set_thresh(cmd.value);
        ESP_LOGI(TAG, "识别阈值已生效：%.2f", cmd.value);
        face_reply_err(ESP_OK);
        break;
    }

    default:
        return false;
    }

    face_reply_done();
    return true;
}

/** @brief 延迟执行的 Flash 写入（ENROLL：先送显示帧，再写 Flash） */
struct pending_enroll_t
{
    bool active;
    int id;
    char name[32];
} s_pending_enroll = {false, -1, ""};

/**
 * @brief 完成录入的 Flash 写入与回复
 * @note  必须在帧已送入显示队列后调用，避免 Flash 写与 LCD 刷新争用 SPI 导致黑屏。
 */
static void finish_pending_enroll(FaceRecognition112V1S8 &recognizer)
{
    if (!s_pending_enroll.active)
    {
        return;
    }
    s_pending_enroll.active = false;

    const int written = recognizer.write_ids_to_flash();
    if (written < 0)
    {
        ESP_LOGE(TAG, "人脸库写入 Flash 失败：%d", written);
        ui_home_set_state(UI_TXT_ENROLL_FLASH_FAIL, UI_COLOR_DANGER);
        (void)voice_prompt_say(VOICE_PROMPT_ENROLL_ERR);
        face_reply_result(ESP_ERR_INVALID_STATE, NULL, -1);
    }
    else
    {
        ESP_LOGI(TAG, "录入成功：id=%d name=%s（人脸库 %d 个）", s_pending_enroll.id,
                 s_pending_enroll.name, recognizer.get_enrolled_id_num());
        ui_home_set_state(UI_TXT_FACE_ENROLL_OK, UI_COLOR_OK);
        ui_home_set_counts(recognizer.get_enrolled_id_num(), -1);
        (void)voice_prompt_say(VOICE_PROMPT_ENROLL_OK);
        face_reply_result(ESP_OK, NULL, s_pending_enroll.id);
    }
    voice_assistant_rearm_wake();
    face_reply_done();
}

/**
 * @brief 处理需要相机帧的录入命令（特征提取在送显示帧之前，Flash 写入在之后）
 * @return true 命令已接受（等待 finish_pending_enroll）；false 已直接回复错误
 */
static bool begin_enroll(FaceRecognition112V1S8 &recognizer, face_frame_ctx_t &ctx,
                         const face_cmd_t &cmd)
{
    if (ctx.frame == NULL || !ctx.present)
    {
        ESP_LOGW(TAG, "录入失败：未检测到人脸");
        ui_home_set_state(UI_TXT_ENROLL_NO_FACE, UI_COLOR_WARN);
        (void)voice_prompt_say(VOICE_PROMPT_ENROLL_ERR);
        face_reply_result(ESP_ERR_NOT_FOUND, NULL, -1);
        face_reply_done();
        return false;
    }
    if (!ctx.quality.pass || ctx.landmarks.size() != 10)
    {
        ESP_LOGW(TAG, "录入失败：质量不达标（尺寸%d 亮度%d 清晰%d 正脸%d）", ctx.quality.size_ok,
                 ctx.quality.brightness_ok, ctx.quality.sharpness_ok, ctx.quality.frontal_ok);
        ui_home_set_state(UI_TXT_ENROLL_BAD_QUALITY, UI_COLOR_WARN);
        (void)voice_prompt_say(VOICE_PROMPT_ENROLL_ERR);
        face_reply_result(ESP_ERR_INVALID_ARG, NULL, -1);
        face_reply_done();
        return false;
    }

    // ---- 先判断是否已注册：同一个人重复注册会得到重复 ID ----
    // 阈值取当前识别阈值；相似度更高说明就是库里那个人。
    {
        face_info_t probe = recognizer.recognize((uint16_t *)ctx.frame->buf,
                                                 {(int)ctx.frame->height, (int)ctx.frame->width, 3},
                                                 ctx.landmarks);
        if (probe.id >= 0)
        {
            ESP_LOGW(TAG, "录入失败：该人脸已注册为 %s（相似度 %.2f）", probe.name.c_str(),
                     probe.similarity);
            ui_home_set_state(UI_TXT_ENROLL_EXIST, UI_COLOR_WARN);
            (void)voice_prompt_say(VOICE_PROMPT_ENROLL_ERR);
            face_reply_result(ESP_ERR_INVALID_STATE, NULL, -1);
            face_reply_done();
            return false;
        }
    }

    char name[32];
    if (cmd.name[0] != '\0')
    {
        strncpy(name, cmd.name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
    else
    {
        snprintf(name, sizeof(name), UI_FMT_PERSON, recognizer.get_enrolled_id_num());
    }

    ui_home_set_state(UI_TXT_FACE_ENROLLING, UI_COLOR_ACCENT);
    // 首次推理会让识别器按真实尺寸分配内部缓冲（对齐脸 + 特征张量），这部分内存
    // 之后长期占用——M4 之后内部 RAM 本就只剩几 KB，这里把前后余额打出来，
    // 排查“注册成功之后点识别/删除没反应”（xTaskCreate 拿不到连续块）时看这行。
    const unsigned heap_before =
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const int id = recognizer.enroll_id((uint16_t *)ctx.frame->buf,
                                        {(int)ctx.frame->height, (int)ctx.frame->width, 3},
                                        ctx.landmarks, name, false);
    ESP_LOGI(TAG, "录入推理完成：内部 RAM %u → %u（最大块 %u）", heap_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (id < 0)
    {
        ESP_LOGE(TAG, "录入失败（特征提取失败）");
        ui_home_set_state(UI_TXT_ENROLL_FAIL, UI_COLOR_DANGER);
        (void)voice_prompt_say(VOICE_PROMPT_ENROLL_ERR);
        // 用 ESP_FAIL 与"该人脸已注册"（ESP_ERR_INVALID_STATE）区分，界面据此给不同提示
        face_reply_result(ESP_FAIL, NULL, -1);
        face_reply_done();
        return false;
    }

    s_pending_enroll.active = true;
    s_pending_enroll.id = id;
    strncpy(s_pending_enroll.name, name, sizeof(s_pending_enroll.name) - 1);
    return true;
}

/**
 * @brief 命令队列取命令（非阻塞）
 * @note  若上一帧的录入回执（write_ids_to_flash 的结果）尚未发出，先补发回执；正常
 *        路径下上一帧送显示后已经 finish_pending_enroll，这里只作兜底。
 */
static bool poll_cmd(FaceRecognition112V1S8 &recognizer, face_cmd_t *cmd)
{
    finish_pending_enroll(recognizer);
    return s_cmd_queue != NULL && xQueueReceive(s_cmd_queue, cmd, 0) == pdTRUE;
}

/**
 * @brief 人脸推理主任务
 */
static void face_task(void *arg)
{
    (void)arg;

    // ---- 模型实例（本任务独占） ----
    HumanFaceDetectMSR01 detector(FACE_DETECT_SCORE_THR, 0.3F, 10, FACE_DETECT_RESIZE_SCALE);
    // MNP01 的 score 门限原为 0.4：它只精修 MSR01 给出的候选，候选中包含分数
    // 0.4~0.6 的“够不上高置信度”人脸，0.4 会把它们全部丢掉，表现为
    // “MSR01 候选 1，MNP01 结果 0”（现场有人却始终无人脸）。降到 0.3 与 MSR01
    // 一致；误检由质量门限 + 轨迹稳定判定兜底。
    HumanFaceDetectMNP01 detector2(0.3F, 0.3F, 10);
    FaceRecognition112V1S8 recognizer;
    // set_partition() 返回 1 = 成功。-1 表示分区不存在（分区表被改过但没全擦重烧）。
    const int part_ok =
        recognizer.set_partition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    recognizer.set_thresh(face_task_threshold());
    if (part_ok != 1)
    {
        ESP_LOGW(TAG, "storage 分区设置失败（set_partition=%d），人脸库将无法持久化", part_ok);
    }
    // ---- 模型版本迁移：换模型后旧库的特征向量作废，清掉再读 ----
    // 此时还没开始取帧，Flash 写不会和 LCD 争 SPI；清库后 set_ids_from_flash 返回 -1（空库），
    // 下面日志会打印“人脸库加载完成：0 个”。用户重新录入即可。
    if (face_task_clear_db_on_start())
    {
        recognizer.clear_id(true);
        ESP_LOGW(TAG, "模型版本变化：已清空旧人脸库（storage 分区），请重新录入");
        ui_home_set_state(UI_TXT_ENROLL_FLASH_FAIL, UI_COLOR_WARN);
    }

    // set_ids_from_flash() 在没有记录时返回 -1，统一按 0 处理
    const int loaded_raw = recognizer.set_ids_from_flash();
    int enrolled = loaded_raw;
    if (enrolled < 0)
    {
        enrolled = 0;
    }
    // 原始返回值单独打印：它同时反映“分区里的记录数”（0 表示一条也没读出来），
    // 排查“注册成功但重启后库里是空的”时看这一行最快。
    ESP_LOGI(TAG, "人脸库加载完成：%d 个（set_ids_from_flash=%d），阈值 %.2f", enrolled,
             loaded_raw, face_task_threshold());

    face_tracker_reset();
    s_result_text_ms = 0;
    s_liveness_pending_frames = 0;
    face_liveness_init();
    face_liveness_set_enabled(face_task_liveness_enabled());
    ui_home_set_counts(enrolled, (int)access_log_today_open_count());

    face_frame_ctx_t ctx;
    int64_t last_auto_ms = 0;
    int64_t last_live_ms = 0;

    while (true)
    {
        camera_fb_t *frame = NULL;
        const bool got_frame =
            xQueueReceive(s_camera_queue, &frame, pdMS_TO_TICKS(FACE_FRAME_TIMEOUT_MS)) == pdTRUE;

#if FACE_DETECT_DEBUG
        // 自检放在首帧到达之后：检测器与识别器在首次推理后才按真实尺寸分配内部缓冲，
        // 过早运行会与实时检测争抢内存，模型链路检查结果也不可靠。
        if (!s_model_selftest_done && got_frame)
        {
            s_model_selftest_done = true;
            face_selftest_detect(detector);
        }
#endif

        // ---- 1. 无帧：只处理不需要相机的命令 ----
        if (!got_frame)
        {
            face_cmd_t cmd;
            while (poll_cmd(recognizer, &cmd))
            {
                if (!execute_simple_cmd(recognizer, cmd))
                {
                    // RECOGNIZE / ENROLL 需要相机帧：摄像头停摆时立即失败，避免调用方空等
                    face_reply_result(ESP_ERR_NOT_FOUND, NULL, -1);
                    face_reply_done();
                }
            }
            continue;
        }

        ctx.frame = frame;
        ctx.now_ms = esp_timer_get_time() / 1000;

        // ---- 1.5 跟踪节流：距上一帧推理不足 FACE_TRACK_INTERVAL_MS 时本帧不送模型 ----
        // 只补画上一帧的框、把帧送预览。命令（识别/注册）最多因此延后一个节流周期，
        // 仍在手动识别的超时范围内；而 face_task 不再独占 Core0，LVGL 与 IDLE0 才有时间跑。
        if ((ctx.now_ms - s_last_track_ms) < FACE_TRACK_INTERVAL_MS)
        {
            if (!s_last_draw.empty())
            {
                draw_detection_result((uint16_t *)frame->buf, frame->height, frame->width,
                                      s_last_draw);
            }
            bool queued = false;
            if (s_display_queue != NULL &&
                xQueueSend(s_display_queue, &frame, pdMS_TO_TICKS(1000)) == pdTRUE)
            {
                queued = true;
                sys_monitor_count_display(); // 仅在帧率统计窗口打开时计数（调试用）
            }
            if (!queued)
            {
                esp_camera_fb_return(frame);
            }
            ctx.frame = NULL;
            continue;
        }
        s_last_track_ms = ctx.now_ms;

        // ---- 2. 两级检测 ----
        const int64_t detect_start = esp_timer_get_time();
        std::list<dl::detect::result_t> &cands =
            detector.infer((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3});
#if FACE_DETECT_DEBUG
        const size_t n_cands = cands.size();
#endif
        std::list<dl::detect::result_t> &results = detector2.infer(
            (uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3}, cands);
        analyze_frame(ctx, results, recognizer.get_enrolled_id_num());
        sys_monitor_report_frame((uint32_t)(esp_timer_get_time() - detect_start));

#if FACE_DETECT_DEBUG
        if (ctx.now_ms - s_detect_log_ms >= FACE_DETECT_LOG_MS)
        {
            s_detect_log_ms = ctx.now_ms;
            const int n_ok = ctx.present ? 1 : 0;
            ESP_LOGI(TAG, "[检测] MSR01 候选 %u，MNP01 结果 %u，选中 %d 框(%d,%d,%d,%d) 质量=%d "
                          "尺寸%d 亮度%d(%.0f) 清晰%d(%.0f) 正脸%d(yaw%.2f)",
                     (unsigned)n_cands, (unsigned)results.size(), n_ok, (int)ctx.x, (int)ctx.y,
                     (int)ctx.w, (int)ctx.h, ctx.present ? 1 : 0, ctx.quality.size_ok ? 1 : 0,
                     ctx.quality.brightness_ok ? 1 : 0, ctx.quality.brightness,
                     ctx.quality.sharpness_ok ? 1 : 0, ctx.quality.sharpness,
                     ctx.quality.frontal_ok ? 1 : 0, ctx.quality.yaw);
#if FACE_DETECT_HEAVY_DEBUG
            dump_frame_ascii(frame);
            dump_frame_colour_stats(frame);
            // 每 60 s 用交换字节序的帧再检测一次，验证 ESP-DL 的 16 位字序假设
            static int64_t s_swap_log_ms = 0;
            if (ctx.now_ms - s_swap_log_ms >= 20000)
            {
                s_swap_log_ms = ctx.now_ms;
                ESP_LOGI(TAG, "[字节序] 交换后 MSR01 候选 %u",
                         (unsigned)detect_swapped_bytes(detector, frame));
            }
#endif
        }
#endif

        // ---- 2.5 调试抓拍：在帧送出之前拷贝（送出后缓冲可能已被摄像头复用）----
#if FACE_DETECT_HEAVY_DEBUG
        bool shot_ready = false;
        if (!s_shot_done && ctx.now_ms >= s_shot_next_ms && shot_copy_frame(frame))
        {
            s_shot_next_ms = ctx.now_ms + 30000; // 每 30 s 采一张
            shot_ready = true;
        }
#endif

        // ---- 3. 命令判定（仅取一条，避免单帧内做多次重活） ----
        face_cmd_t cmd;
        const bool has_cmd = poll_cmd(recognizer, &cmd);
        bool cmd_is_recognize = false;
        bool cmd_is_enroll = false;

        if (has_cmd)
        {
            if (cmd.type == FACE_CMD_RECOGNIZE)
            {
                cmd_is_recognize = true;
                if (ctx.present && ctx.quality.pass)
                {
                    // 手动识别是用户的显式动作：同一轨迹再次点“开始识别”时也要重新弹
                    // 覆盖层、重新写一条门禁日志，否则第二三次点击看起来毫无反应
                    //（s_track_access_done / s_track_deny_done 在首次结果后已置位）。
                    // 陌生人的抓拍冷却（时间 + 每小时张数）不受影响，仍在 handle_stranger 里兜底。
                    s_track_access_done = false;
                    s_track_deny_done = false;

                    const face_result_t res = recognize_once(recognizer, ctx, true,
                                                             recognizer.get_enrolled_id_num(),
                                                             last_live_ms);
                    face_reply_result(ESP_OK, &res, -1);
                    face_reply_done();
                }
                else
                {
                    /* 语音/按键“开始识别”但画面里没有人脸或质量不合格：没有识别结论，
                     * 只有状态卡文字，用户容易以为“没反应”。播报具体原因（素材约 1 s）。 */
                    (void)voice_prompt_say(ctx.present ? (ctx.quality.size_ok ? VOICE_PROMPT_QUALITY
                                                                             : VOICE_PROMPT_TOO_FAR)
                                                       : VOICE_PROMPT_NO_FACE);
                    face_reply_result(ctx.present ? ESP_ERR_INVALID_ARG : ESP_ERR_NOT_FOUND, NULL,
                                      -1);
                    face_reply_done();
                }
            }
            else if (cmd.type == FACE_CMD_ENROLL)
            {
                cmd_is_enroll = begin_enroll(recognizer, ctx, cmd);
                if (!cmd_is_enroll)
                {
                    voice_assistant_rearm_wake();
                }
            }
            // 其余（删除/清空/列表/阈值）写 Flash，必须放到帧送出之后执行
        }
        else if (face_task_strategy() == FACE_STRATEGY_AUTO && ctx.present && ctx.quality.pass &&
                 ctx.track.stable && (ctx.now_ms - last_auto_ms) >= FACE_AUTO_INTERVAL_MS)
        {
            // ---- 4. 自动识别：稳定轨迹上按间隔识别 ----
            last_auto_ms = ctx.now_ms;
            (void)recognize_once(recognizer, ctx, false, recognizer.get_enrolled_id_num(),
                                 last_live_ms);
        }
        else
        {
            // 非识别帧也喂活体统计（纹理/运动），否则样本永远不足
            liveness_tick(ctx, NULL, 0, false, last_live_ms);
        }


        // ---- 5. 先送显示队列（LCD 任务负责归还帧） ----
        bool frame_queued = false;
        if (s_display_queue != NULL &&
            xQueueSend(s_display_queue, &frame, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            frame_queued = true;
            sys_monitor_count_display(); // 仅在帧率统计窗口打开时计数（调试用）
        }
        if (!frame_queued)
        {
            esp_camera_fb_return(frame);
        }
        ctx.frame = NULL;

        // ---- 6. 帧已送出：现在可以安全写 Flash ----
        finish_pending_enroll(recognizer);

#if FACE_DETECT_HEAVY_DEBUG
        if (shot_ready)
        {
            shot_write_jpeg();
        }
#endif

        if (has_cmd && !cmd_is_recognize && !cmd_is_enroll)
        {
            if (!execute_simple_cmd(recognizer, cmd))
            {
                face_reply_result(ESP_ERR_NOT_FOUND, NULL, -1);
                face_reply_done();
            }
        }
    }
}

QueueHandle_t face_task_cmd_queue(void)
{
    return s_cmd_queue;
}

esp_err_t face_task_start(QueueHandle_t camera_queue, QueueHandle_t display_queue,
                          QueueHandle_t cmd_queue)
{
    if (camera_queue == NULL || cmd_queue == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_camera_queue = camera_queue;
    s_display_queue = display_queue;
    s_cmd_queue = cmd_queue;

    // 用静态 TCB + 静态栈：xTaskCreate 会额外从内部堆切 1 KB 连续块做 TCB，
    // 而 WiFi 起来后内部 RAM 的最大连续块只有 1.4~7.6 KB，这个块时有时无——实测
    // D-cache 刷写/配置变化后 face_task 创建失败，人脸完全不工作（日志 "face_task
    // 创建失败"）。静态分配在启动时就按 .bss 预留，不依赖运行时堆碎片。
    static StaticTask_t s_face_tcb;
    static StackType_t s_face_stack[FACE_TASK_STACK / sizeof(StackType_t)];
    const TaskHandle_t h = xTaskCreateStaticPinnedToCore(face_task, "face_task", FACE_TASK_STACK,
                                                         NULL, FACE_TASK_PRIO, s_face_stack,
                                                         &s_face_tcb, FACE_TASK_CORE);
    return h != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}
