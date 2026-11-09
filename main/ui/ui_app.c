/**
 * @file    ui_app.c
 * @brief   应用界面管理实现：创建各页面、切换、底部状态栏、模拟开门覆盖层。
 *
 * @note    - 页面在 ui_app_start() 时一次性全部创建并常驻 RAM（5 个屏幕对象），
 *            切换时只调用 lv_scr_load_anim()，避免每次进出页面重建控件造成卡顿。
 *          - 状态栏是每个屏幕底部的独立对象；set_status 只改动当前页面的状态栏，
 *            其余页面在切换时通过 s_status_* 全局数据同步文本，保证切页后提示一致。
 *          - 3 秒自动清除与覆盖层超时都由 LVGL 定时器驱动，不使用额外 FreeRTOS 任务。
 *          - 所有对外函数内部加锁，可以从任意任务调用。
 */

#include "ui_app.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ui_lvgl_port.h"
#include "ui_pages.h"
#include "ui_preview.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui_app"; // 日志标签

/** @brief 各页面屏幕对象，索引即 ui_page_t */
static lv_obj_t *s_screens[UI_PAGE_MAX] = {NULL};

/** @brief 各页面底部状态栏标签，索引即 ui_page_t */
static lv_obj_t *s_status_labels[UI_PAGE_MAX] = {NULL};

/** @brief 开门覆盖层的全屏模态遮罩（仅在覆盖层显示时可见，用于拦截触摸） */
static lv_obj_t *s_overlay_mask = NULL;
/** @brief 开门覆盖层 */
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_overlay_title = NULL;
static lv_obj_t *s_overlay_sub = NULL;

/** @brief 当前页面 (volatile：被其他任务读取) */
static volatile ui_page_t s_current_page = UI_PAGE_HOME;

/** @brief 全局状态栏文本与颜色（切页时同步到新页面的状态栏） */
static char s_status_text[64] = "";
static uint32_t s_status_color = UI_COLOR_TEXT; /**< 0xRRGGBB：LVGL 任务外可能被读取，不存 lv_color_t */
static bool s_status_active = false;

/** @brief LVGL 定时器句柄 */
static lv_timer_t *s_status_timer = NULL;
static lv_timer_t *s_overlay_timer = NULL;

/** @brief 最近一次覆盖层所代表的开门来源（供日志/播报读取） */
static volatile ui_overlay_reason_t s_overlay_reason = UI_OVERLAY_REASON_NONE;

/**
 * @brief 触摸诊断开关：置 1 时每 100 ms 在按下期间打印 LVGL 变换后的坐标与命中对象
 * @note  已用它确认横屏映射正确（见 tp.c 的 flags 说明），默认关闭。
 */
#define UI_TOUCH_DEBUG 0

#if UI_TOUCH_DEBUG
/**
 * @brief 调试定时器：按住时打印 LVGL 变换后的触摸点与命中对象
 * @note  每 100 ms 检查一次指点设备状态；只在按下期间打印，用于判断坐标是否落在控件上
 */
static void touch_debug_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev != NULL && lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER)
    {
        indev = lv_indev_get_next(indev);
    }
    if (indev == NULL)
    {
        return;
    }

    // 按下期间 LVGL 会把 indev->proc.types.pointer.act_obj 置为命中对象，松开后清空，
    // 这里用它判断是否正在按压（LVGL 8 没有公开的 indev 状态读取接口）。
    lv_obj_t *act = lv_indev_get_obj_act();
    if (act == NULL)
    {
        return;
    }

    lv_point_t p = {0, 0};
    lv_indev_get_point(indev, &p);
    lv_area_t a = {0, 0, -1, -1};
    if (act != NULL)
    {
        lv_obj_get_coords(act, &a);
    }
    ESP_LOGI(TAG, "LVGL 触摸点 (%d,%d) 命中=%p [%d,%d-%d,%d]", (int)p.x, (int)p.y, (void *)act,
             (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
}
#endif

// =================================================================
// 底部状态栏
// =================================================================

/**
 * @brief 为指定页面创建底部状态栏
 * @param scr 页面屏幕对象
 */
static void create_status_bar(lv_obj_t *scr)
{
    for (ui_page_t p = UI_PAGE_HOME; p < UI_PAGE_MAX; p++)
    {
        if (s_screens[p] != scr)
        {
            continue;
        }

        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_set_pos(bar, 0, UI_SCREEN_H - UI_STATUS_H);
        lv_obj_set_size(bar, UI_SCREEN_W, UI_STATUS_H);
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_HEADER), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *label =
            ui_label_create(bar, s_status_active ? s_status_text : "", UI_FONT_BODY,
                            s_status_active ? s_status_color : UI_COLOR_TEXT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
        s_status_labels[p] = label;
        return;
    }
}

/**
 * @brief 把全局状态栏文本刷到当前页面的状态栏
 */
static void apply_status(void)
{
    lv_obj_t *label = s_status_labels[s_current_page];
    if (label == NULL)
    {
        return;
    }
    lv_label_set_text(label, s_status_active ? s_status_text : "");
    lv_obj_set_style_text_color(label, lv_color_hex(s_status_active ? s_status_color : UI_COLOR_TEXT),
                                0);
}

/**
 * @brief 状态栏 3 秒定时器回调：清除提示
 * @note  只在 LVGL 任务内执行，无需加锁
 */
static void status_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_status_active)
    {
        s_status_active = false;
        s_status_text[0] = '\0';
        apply_status();
    }
}

// =================================================================
// 开门覆盖层
// =================================================================

/**
 * @brief 覆盖层定时器回调：自动隐藏
 */
static void overlay_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_overlay != NULL)
    {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_overlay_mask != NULL)
    {
        lv_obj_add_flag(s_overlay_mask, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 创建（或复用）覆盖层
 */
static void ensure_overlay(void)
{
    if (s_overlay != NULL)
    {
        return;
    }

    // 注意：这里操作的是 lv_layer_top()（全局顶层图层，所有屏幕共用）——
    // 不能给它加 CLICKABLE 标志，也不能在遮挡区域设置“像素级透明”，
    // 否则 LVGL 会认为触摸被顶层图层吞掉，导致下面页面的按钮全部失灵。
    lv_obj_t *scr = lv_layer_top();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    // 全屏模态遮罩：颜色取页面背景色以混出“半透明黑”，透明度由自身 bg_opa 控制。
    s_overlay_mask = lv_obj_create(scr);
    lv_obj_remove_style_all(s_overlay_mask);
    lv_obj_set_pos(s_overlay_mask, 0, 0);
    lv_obj_set_size(s_overlay_mask, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(s_overlay_mask, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay_mask, LV_OPA_70, 0);
    lv_obj_add_flag(s_overlay_mask, LV_OBJ_FLAG_HIDDEN);

    s_overlay = lv_obj_create(scr);
    lv_obj_set_pos(s_overlay, (UI_SCREEN_W - 360) / 2, (UI_SCREEN_H - 180) / 2 - UI_STATUS_H / 2);
    lv_obj_set_size(s_overlay, 360, 180);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 3, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(UI_COLOR_OK), 0);
    lv_obj_set_style_radius(s_overlay, 12, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_overlay_title = ui_label_create(s_overlay, "", UI_FONT_TITLE, UI_COLOR_OK);
    lv_obj_align(s_overlay_title, LV_ALIGN_TOP_MID, 0, 40);

    s_overlay_sub = ui_label_create(s_overlay, "", UI_FONT_BODY, UI_COLOR_TEXT);
    lv_obj_align(s_overlay_sub, LV_ALIGN_TOP_MID, 0, 96);

    // 只创建一个定时器，反复重启即可
    s_overlay_timer = lv_timer_create(overlay_timeout_cb, 3000, NULL);
    lv_timer_pause(s_overlay_timer);
}

// =================================================================
// 页面切换
// =================================================================

static void show_page(ui_page_t page)
{
    if (page >= UI_PAGE_MAX || s_screens[page] == NULL)
    {
        return;
    }

    if (page != s_current_page)
    {
        lv_scr_load_anim(s_screens[page], LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        s_current_page = page;
        apply_status();
        ESP_LOGI(TAG, "切换到页面 %d", (int)page); // 便于确认触摸导航是否生效
    }
    else
    {
        lv_scr_load(s_screens[page]);
    }
}

esp_err_t ui_app_switch_page(ui_page_t page)
{
    if (page >= UI_PAGE_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ui_lvgl_is_ready())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ui_lvgl_lock(100))
    {
        ESP_LOGW(TAG, "切换页面加锁超时");
        return ESP_ERR_TIMEOUT;
    }
    show_page(page);
    ui_lvgl_unlock();
    return ESP_OK;
}

esp_err_t ui_app_goto_home(void)
{
    return ui_app_switch_page(UI_PAGE_HOME);
}

ui_page_t ui_app_current_page(void)
{
    return s_current_page;
}

// =================================================================
// 状态栏与覆盖层对外接口
// =================================================================

void ui_app_set_status(const char *text, uint32_t color_hex)
{
    if (text == NULL || !ui_lvgl_is_ready())
    {
        return;
    }
    if (!ui_lvgl_lock(50))
    {
        return;
    }

    strncpy(s_status_text, text, sizeof(s_status_text) - 1);
    s_status_text[sizeof(s_status_text) - 1] = '\0';
    s_status_color = color_hex;
    s_status_active = true;
    apply_status();

    if (s_status_timer == NULL)
    {
        s_status_timer = lv_timer_create(status_timeout_cb, 3000, NULL);
    }
    else
    {
        lv_timer_reset(s_status_timer);
        lv_timer_resume(s_status_timer);
    }

    ui_lvgl_unlock();
}

void ui_app_clear_status(void)
{
    if (!ui_lvgl_is_ready())
    {
        return;
    }
    if (!ui_lvgl_lock(50))
    {
        return;
    }
    s_status_active = false;
    s_status_text[0] = '\0';
    apply_status();
    ui_lvgl_unlock();
}

void ui_app_show_door_overlay(const char *title, const char *subtitle, bool ok,
                              uint32_t timeout_ms)
{
    if (!ui_lvgl_is_ready())
    {
        return;
    }
    if (!ui_lvgl_lock(100))
    {
        return;
    }

    ensure_overlay();

    const uint32_t color = ok ? UI_COLOR_OK : UI_COLOR_DANGER;
    lv_label_set_text(s_overlay_title, title ? title : "");
    lv_obj_set_style_text_color(s_overlay_title, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(color), 0);

    lv_label_set_text(s_overlay_sub, subtitle ? subtitle : "");
    if (subtitle == NULL)
    {
        lv_obj_add_flag(s_overlay_sub, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(s_overlay_sub, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(s_overlay_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);

    if (timeout_ms == 0)
    {
        timeout_ms = 3000;
    }
    lv_timer_set_period(s_overlay_timer, timeout_ms);
    lv_timer_reset(s_overlay_timer);
    lv_timer_resume(s_overlay_timer);

    // 记录本次覆盖层含义：业务层据此挂接“开门事件”（M3 日志 / M5 播报）
    if (ok && title != NULL && strcmp(title, UI_TXT_DOOR_OPEN) == 0)
    {
        s_overlay_reason = UI_OVERLAY_REASON_FACE;
    }
    else if (ok && title != NULL && strcmp(title, UI_TXT_DOOR_REMOTE_OK) == 0)
    {
        s_overlay_reason = UI_OVERLAY_REASON_REMOTE;
    }
    else if (ok && title != NULL && strcmp(title, UI_TXT_DOOR_VOICE_OK) == 0)
    {
        s_overlay_reason = UI_OVERLAY_REASON_VOICE;
    }
    else
    {
        s_overlay_reason = UI_OVERLAY_REASON_NONE;
    }

    ui_lvgl_unlock();
}

ui_overlay_reason_t ui_app_get_overlay_reason(void)
{
    return s_overlay_reason;
}

void ui_app_hide_door_overlay(void)
{
    if (s_overlay == NULL || !ui_lvgl_is_ready())
    {
        return;
    }
    if (!ui_lvgl_lock(50))
    {
        return;
    }
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay_mask != NULL)
    {
        lv_obj_add_flag(s_overlay_mask, LV_OBJ_FLAG_HIDDEN);
    }
    ui_lvgl_unlock();
}

// =================================================================
// 初始化
// =================================================================

/**
 * @brief 创建全部页面（必须在锁内调用）
 */
static esp_err_t create_all_pages(void)
{
    s_screens[UI_PAGE_HOME] = ui_home_create();
    s_screens[UI_PAGE_ENROLL] = ui_enroll_create();
    s_screens[UI_PAGE_LOGS] = ui_logs_create();
    s_screens[UI_PAGE_SETTINGS] = ui_settings_create();
    s_screens[UI_PAGE_DIAG] = ui_diag_create();
    s_screens[UI_PAGE_FACE_LIST] = ui_face_list_create();

    for (ui_page_t p = UI_PAGE_HOME; p < UI_PAGE_MAX; p++)
    {
        if (s_screens[p] == NULL)
        {
            ESP_LOGE(TAG, "页面 %d 创建失败", (int)p);
            return ESP_ERR_NO_MEM;
        }
        create_status_bar(s_screens[p]);
    }
    return ESP_OK;
}

esp_err_t ui_app_start(void)
{
    if (!ui_lvgl_is_ready())
    {
        ESP_LOGE(TAG, "LVGL 未初始化，无法创建界面");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ui_lvgl_lock(0))
    {
        ESP_LOGE(TAG, "获取 LVGL 锁失败");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = create_all_pages();
    if (ret == ESP_OK)
    {
        // 主页创建时已创建预览控件；这里显式保证顺序（预览依赖主页内容区）
        lv_scr_load(s_screens[UI_PAGE_HOME]);
        s_current_page = UI_PAGE_HOME;
        apply_status();
#if UI_TOUCH_DEBUG
        lv_timer_create(touch_debug_timer_cb, 100, NULL);
#endif
    }

    ui_lvgl_unlock();

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "界面已启动（%d 个页面）", (int)UI_PAGE_MAX);
    }
    return ret;
}
