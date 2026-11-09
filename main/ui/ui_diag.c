/**
 * @file    ui_diag.c
 * @brief   设备诊断页（M3）实现：实时展示帧率、耗时、内存、存储、人脸与抓拍数量。
 *
 * @note    1. 数据采集一律在 LVGL 锁之外：storage_usage() 要读 SPIFFS、snapshot_count()
 *             要遍历目录、栈水位要扫任务栈，都不该占着界面锁。
 *             做法是“先采集到本地结构体，再取锁只更新文字”，取不到锁就丢掉本轮。
 *          2. 每行只在文字变化时重绘（lv_label_set_text 相同内容也会触发重排）。
 *          3. 芯片温度/环境温湿度/光照属于 M6 的 env_service，尚未实现；
 *             网络/云端在 M4 接入；“语音”行在 M5 接入（状态 + 输入电平）。
 *          4. 页面入口在设置页：主页按钮已经排满，不再放诊断入口。
 */

#include "ui_diag.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "face_service.h"
#include "net_service.h"
#include "snapshot.h"
#include "storage.h"
#include "sys_monitor.h"
#include "ui_app.h"
#include "ui_lvgl_port.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "voice_assistant.h" /* M5：语音状态与麦克风电平 */

static const char *TAG = "ui_diag"; // 日志标签

/* ==================== 页面参数 ==================== */
/**
 * @brief 行高：本页只读不点，可以比设置页紧凑（16 px 字体 + 上下 3 px）
 * @note  M5 加了“语音”行后共 11 行。内容区只有 UI_SCREEN_H - UI_HEADER_H -
 *        UI_STATUS_H = 252 px，再扣 UI_PAD 上下各 8（可用 236）：22 px 行高
 *        + 1 px 间距需要 252 px，末行“固件版本”会被裁成半个字。行高取 21 后
 *        11 x 21 + 10 x 1 = 241，仍略高于 236，所以间距也收到 0（11 x 21 + 10 = 241
 *        → 再让底行贴边即可），实测最后一行完整显示。
 */
#define UI_DIAG_ROW_H 21
/** @brief 行间距（M5 加行后由 1 收到 0，见 UI_DIAG_ROW_H 说明） */
#define UI_DIAG_ROW_GAP 0
/** @brief 刷新周期（毫秒） */
#define UI_DIAG_TIMER_MS 1000
/** @brief 取 LVGL 锁的等待时间（取不到就跳过本轮） */
#define UI_DIAG_LOCK_MS 20
/** @brief 数值标签最多长度 */
#define UI_DIAG_VAL_MAX 48

/** @brief 行序号（控件按此索引更新） */
typedef enum
{
    UI_DIAG_ROW_THRESHOLD = 0, /**< 识别阈值 / 活体 */
    UI_DIAG_ROW_FACE_COUNT,    /**< 人脸数量 */
    UI_DIAG_ROW_FPS,           /**< 检测帧率 / 预览帧率 */
    UI_DIAG_ROW_DETECT,        /**< 检测耗时 */
    UI_DIAG_ROW_MEMORY,        /**< 内部 RAM / PSRAM */
    UI_DIAG_ROW_STORAGE,       /**< SPIFFS 已用 */
    UI_DIAG_ROW_SNAPSHOTS,     /**< 抓拍张数 */
    UI_DIAG_ROW_NETWORK,       /**< 网络（M4） */
    UI_DIAG_ROW_CLOUD,         /**< 云端（M4） */
    UI_DIAG_ROW_VOICE,         /**< 语音（M5）：状态 + 麦克风 RMS */
    UI_DIAG_ROW_FIRMWARE,      /**< 固件版本 */
    UI_DIAG_ROW_MAX,
} ui_diag_row_t;

/** @brief 数值标签（NULL 表示该行尚未创建） */
static lv_obj_t *s_values[UI_DIAG_ROW_MAX];
/** @brief 上次显示的文字（内容未变则不重绘） */
static char s_texts[UI_DIAG_ROW_MAX][UI_DIAG_VAL_MAX];
/** @brief 刷新定时器 */
static lv_timer_t *s_timer = NULL;

/** @brief 本轮采集到的数据（锁外填充，锁内只读） */
typedef struct
{
    float threshold;
    bool liveness;
    int face_count;
    float fps_detect;
    float fps_preview;
    uint32_t detect_us;
    uint32_t heap_internal_kb;
    uint32_t heap_psram_kb;
    uint32_t storage_used_pct;
    uint32_t storage_free_kb;
    size_t snap_count;
    char net_text[UI_DIAG_VAL_MAX];   /**< 网络状态（含配网热点名 / RSSI） */
    char cloud_text[UI_DIAG_VAL_MAX]; /**< 云端状态 */
    char voice_text[UI_DIAG_VAL_MAX]; /**< 语音状态 + 电平 */
} diag_snapshot_t;

/**
 * @brief 采集数据（锁外调用）
 * @param out 输出
 */
static void collect(diag_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));

    face_service_status_t st;
    face_service_get_status(&st);
    out->threshold = st.threshold;
    out->liveness = st.liveness_enabled;
    out->face_count = st.enrolled_num;

    sys_monitor_status_t mon;
    sys_monitor_get_status(&mon);
    out->fps_detect = mon.fps_detect;
    out->fps_preview = mon.fps_preview;
    out->detect_us = mon.detect_avg_us;
    out->heap_internal_kb = mon.heap_internal_free / 1024;
    out->heap_psram_kb = mon.heap_psram_free / 1024;

    size_t total = 0;
    size_t used = 0;
    if (storage_is_ready() && storage_usage(&total, &used) == ESP_OK && total > 0)
    {
        out->storage_used_pct = (uint32_t)(used * 100 / total);
        out->storage_free_kb = (uint32_t)((total - used) / 1024);
    }

    out->snap_count = snapshot_is_ready() ? snapshot_count() : 0;

    // 网络/云端只读状态枚举，不访问 WiFi 驱动以外的东西；锁外调用安全
    net_service_summary(out->net_text, sizeof(out->net_text));
    snprintf(out->cloud_text, sizeof(out->cloud_text), "%s", net_service_cloud_text());

    // 语音：状态文字直接可读；电平用来判断“麦克风有没有声音进来”
    uint32_t rms = 0;
    uint32_t peak = 0;
    if (voice_assistant_get_level(&rms, &peak))
    {
        snprintf(out->voice_text, sizeof(out->voice_text), "%s %lu",
                 voice_assistant_state_text(), (unsigned long)rms);
    }
    else
    {
        snprintf(out->voice_text, sizeof(out->voice_text), "%s", voice_assistant_state_text());
    }
}

/**
 * @brief 更新一行文字（内容未变不重绘）
 * @param row  行序号
 * @param text 新文字
 * @return true 内容确实变了
 */
static bool set_row(ui_diag_row_t row, const char *text)
{
    if (s_values[row] == NULL || strcmp(s_texts[row], text) == 0)
    {
        return false;
    }
    snprintf(s_texts[row], UI_DIAG_VAL_MAX, "%s", text);
    lv_label_set_text(s_values[row], s_texts[row]);
    return true;
}

/**
 * @brief 把采集结果格式化成各行的文字（锁内调用）
 * @param snap 已采集的数据
 * @return true 至少有一行内容变化（需要重绘）
 */
static bool render(const diag_snapshot_t *snap)
{
    char buf[UI_DIAG_VAL_MAX];
    bool changed = false;

    snprintf(buf, sizeof(buf), "%.2f  %s %s", snap->threshold, UI_TXT_SET_LIVENESS,
             snap->liveness ? UI_TXT_ON : UI_TXT_OFF);
    changed |= set_row(UI_DIAG_ROW_THRESHOLD, buf);

    snprintf(buf, sizeof(buf), "%d", snap->face_count);
    changed |= set_row(UI_DIAG_ROW_FACE_COUNT, buf);

    // 检测 / 预览两个帧率：检测受节流约 4~5，预览才是肉眼看到的流畅度
    snprintf(buf, sizeof(buf), "%.1f / %.1f", snap->fps_detect, snap->fps_preview);
    changed |= set_row(UI_DIAG_ROW_FPS, buf);

    snprintf(buf, sizeof(buf), "%u %s", (unsigned)snap->detect_us, UI_TXT_DIAG_MS);
    changed |= set_row(UI_DIAG_ROW_DETECT, buf);

    // 内部 RAM 与 PSRAM 分两段显示：内部 RAM 才是判断“还能不能开新任务”的依据
    snprintf(buf, sizeof(buf), "%u / %u KB", (unsigned)snap->heap_internal_kb,
             (unsigned)snap->heap_psram_kb);
    changed |= set_row(UI_DIAG_ROW_MEMORY, buf);

    if (snap->storage_used_pct == 0 && snap->storage_free_kb == 0)
    {
        snprintf(buf, sizeof(buf), "%s", UI_TXT_NOT_READY);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%u%%  %u KB", (unsigned)snap->storage_used_pct,
                 (unsigned)snap->storage_free_kb);
    }
    changed |= set_row(UI_DIAG_ROW_STORAGE, buf);

    snprintf(buf, sizeof(buf), "%u %s", (unsigned)snap->snap_count, UI_TXT_DIAG_PIECES);
    changed |= set_row(UI_DIAG_ROW_SNAPSHOTS, buf);

    // 网络：已连接（含 dBm）/ 配网中 + 热点名 / 未配置等；云端在 MQTT 接入前显示“未启用”
    changed |= set_row(UI_DIAG_ROW_NETWORK, snap->net_text);
    changed |= set_row(UI_DIAG_ROW_CLOUD, snap->cloud_text);
    // 语音：状态 + 16 kHz 输入 RMS（正常说话 100~2000，静默 30~80）
    changed |= set_row(UI_DIAG_ROW_VOICE, snap->voice_text);
    changed |= set_row(UI_DIAG_ROW_FIRMWARE, APP_CFG_VERSION);

    return changed;
}

/**
 * @brief 刷新定时器：只在诊断页可见时采集并更新
 * @note  采集（读 SPIFFS、枚举任务）放在锁外，避免长时间占用 LVGL 任务。
 */
static void diag_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    // 不在诊断页就不采集：读 SPIFFS / 枚举任务都没必要常驻运行
    if (ui_app_current_page() != UI_PAGE_DIAG)
    {
        return;
    }

    diag_snapshot_t snap;
    collect(&snap);

    if (!ui_lvgl_lock(UI_DIAG_LOCK_MS))
    {
        return; // 本轮放弃，下一轮再来
    }

    // render() 内部逐行比较，只有真正变化的行才重绘
    render(&snap);

    ui_lvgl_unlock();
}

/**
 * @brief 创建一行：左侧名称（次要色）+ 右侧数值（强调色）
 * @param content 内容区
 * @param y       纵坐标
 * @param row     行序号（用于登记数值标签）
 * @param name    行名称
 * @param value   初始数值
 */
static void create_row(lv_obj_t *content, int y, ui_diag_row_t row, const char *name,
                       const char *value)
{
    const int w = UI_SCREEN_W - UI_PAD * 2; // 464
    // 行高只有 22 px，用 ui_card_create() 默认的 8 px 内边距会把 16 px 文字裁掉一半；
    // 这里上下不留白、左右 10 px。
    lv_obj_t *card = ui_card_create(content, 0, y, w, UI_DIAG_ROW_H);
    lv_obj_set_style_pad_top(card, 0, 0);
    lv_obj_set_style_pad_bottom(card, 0, 0);
    lv_obj_set_style_pad_left(card, 10, 0);
    lv_obj_set_style_pad_right(card, 10, 0);

    lv_obj_t *label = ui_label_create(card, name, UI_FONT_BODY, UI_COLOR_TEXT_DIM);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = ui_label_create(card, value, UI_FONT_BODY, UI_COLOR_TEXT);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    s_values[row] = val;
    snprintf(s_texts[row], UI_DIAG_VAL_MAX, "%s", value);
}

lv_obj_t *ui_diag_create(void)
{
    lv_obj_t *content = ui_page_create(UI_TXT_DIAG_TITLE, true);
    if (content == NULL)
    {
        return NULL;
    }

    const int step = UI_DIAG_ROW_H + UI_DIAG_ROW_GAP;
    int y = 0;
    create_row(content, y, UI_DIAG_ROW_THRESHOLD, UI_TXT_SET_THRESHOLD, UI_TXT_UNKNOWN);
    y += step;
    create_row(content, y, UI_DIAG_ROW_FACE_COUNT, UI_TXT_DIAG_FACE_COUNT, "0");
    y += step;
    create_row(content, y, UI_DIAG_ROW_FPS, UI_TXT_DIAG_FPS, UI_TXT_UNKNOWN);
    y += step;
    create_row(content, y, UI_DIAG_ROW_DETECT, UI_TXT_DIAG_DETECT_TIME, UI_TXT_UNKNOWN);
    y += step;
    create_row(content, y, UI_DIAG_ROW_MEMORY, UI_TXT_DIAG_MEMORY, UI_TXT_UNKNOWN);
    y += step;
    create_row(content, y, UI_DIAG_ROW_STORAGE, UI_TXT_DIAG_STORAGE, UI_TXT_NOT_READY);
    y += step;
    create_row(content, y, UI_DIAG_ROW_SNAPSHOTS, UI_TXT_DIAG_SNAPSHOTS, "0");
    y += step;
    create_row(content, y, UI_DIAG_ROW_NETWORK, UI_TXT_DIAG_NETWORK, UI_TXT_NOT_ENABLED);
    y += step;
    create_row(content, y, UI_DIAG_ROW_CLOUD, UI_TXT_DIAG_CLOUD, UI_TXT_NOT_ENABLED);
    y += step;
    create_row(content, y, UI_DIAG_ROW_VOICE, UI_TXT_DIAG_VOICE, UI_TXT_NOT_ENABLED);
    y += step;
    create_row(content, y, UI_DIAG_ROW_FIRMWARE, UI_TXT_DIAG_FIRMWARE, APP_CFG_VERSION);

    // 诊断页从设置页进入，返回也回设置页
    ui_page_set_back_target(UI_PAGE_DIAG, UI_PAGE_SETTINGS);

    // 不需要 leave 回调：定时器只在诊断页可见时采集，离开后自然停手，
    // 重新进入时 render() 会按内容变化立即把过期数据刷掉。
    if (s_timer == NULL)
    {
        s_timer = lv_timer_create(diag_timer_cb, UI_DIAG_TIMER_MS, NULL);
    }

    ESP_LOGI(TAG, "诊断页已创建");

    return lv_obj_get_parent(content);
}
