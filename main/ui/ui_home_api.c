/**
 * @file    ui_home_api.c
 * @brief   主页状态卡更新接口实现（M2）：文字与计数都通过 LVGL 锁修改。
 *
 * @note    - 主页由 ui_pages.c 创建；本文件在创建时收到控件句柄，之后由 face_task
 *            以任意频率调用更新函数。
 *            - 持锁等待 20 ms：face_task 处在识别路径上，不应为等 LVGL 长时间阻塞；
 *            等不到锁就丢弃本次文字更新（下一帧会再次调用）。
 *            - 计数传负数表示“保持不变”，便于只更新其中一项。
 */

#include "ui_home_api.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "ui_lvgl_port.h"
#include "ui_strings.h"

static const char *TAG = "ui_home";

/** @brief 主页状态卡控件（LVGL 对象，首次创建后被记录） */
static lv_obj_t *s_state_label = NULL;
static lv_obj_t *s_enrolled_label = NULL;
static lv_obj_t *s_today_label = NULL;

/** @brief 最近一次状态文字与颜色（LVGL 未就绪时暂存，创建后回放） */
static char s_pending_text[96] = UI_TXT_HOME_WAITING;
static uint32_t s_pending_color = 0xE8EDF2; // UI_COLOR_TEXT
static int s_enrolled = -1;
static int s_today = -1;

/** @brief 已经写到控件上的文字/颜色，用于跳过重复刷新（避免每帧触发重绘） */
static char s_applied_text[96] = "";
static uint32_t s_applied_color = 0;

/**
 * @brief 刷新已注册/今日通行文字
 * @note  调用方必须已持有 LVGL 锁
 */
static void refresh_counts_locked(void)
{
    char text[48];
    if (s_enrolled_label != NULL && s_enrolled >= 0)
    {
        snprintf(text, sizeof(text), "%s %d", UI_TXT_HOME_ENROLLED, s_enrolled);
        lv_label_set_text(s_enrolled_label, text);
    }
    if (s_today_label != NULL && s_today >= 0)
    {
        snprintf(text, sizeof(text), "%s %d", UI_TXT_HOME_TODAY_PASS, s_today);
        lv_label_set_text(s_today_label, text);
    }
}

/**
 * @brief 主页创建时登记状态卡控件，并回放最近一次暂存的文字
 * @param state_label    主状态文字控件（“待识别”）
 * @param enrolled_label “已录入 N”控件
 * @param today_label    “今日通行 N”控件
 * @note  由 ui_pages.c 在 ui_home_create() 末尾调用，调用时已持有 LVGL 锁。
 */
void ui_home_register_widgets(lv_obj_t *state_label, lv_obj_t *enrolled_label,
                              lv_obj_t *today_label)
{
    s_state_label = state_label;
    s_enrolled_label = enrolled_label;
    s_today_label = today_label;

    s_applied_text[0] = '\0';
    s_applied_color = 0;
    if (s_state_label != NULL)
    {
        lv_label_set_text(s_state_label, s_pending_text);
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(s_pending_color), 0);
        snprintf(s_applied_text, sizeof(s_applied_text), "%s", s_pending_text);
        s_applied_color = s_pending_color;
    }
    refresh_counts_locked();
    ESP_LOGI(TAG, "主页状态卡已登记");
}

void ui_home_set_state(const char *text, uint32_t color)
{
    if (text == NULL)
    {
        return;
    }
    // 先暂存：LVGL 未就绪或持锁失败时，最新一次内容不丢失
    snprintf(s_pending_text, sizeof(s_pending_text), "%s", text);
    s_pending_color = color;

    // 内容未变化：直接返回，避免每帧重绘状态标签
    if (s_state_label != NULL && s_applied_color == s_pending_color &&
        strcmp(s_applied_text, s_pending_text) == 0)
    {
        return;
    }

    if (s_state_label == NULL || !ui_lvgl_is_ready() || !ui_lvgl_lock(20))
    {
        return;
    }
    lv_label_set_text(s_state_label, s_pending_text);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(s_pending_color), 0);
    ui_lvgl_unlock();
    snprintf(s_applied_text, sizeof(s_applied_text), "%s", s_pending_text);
    s_applied_color = s_pending_color;
}

void ui_home_set_counts(int enrolled, int today)
{
    if (enrolled >= 0)
    {
        s_enrolled = enrolled;
    }
    if (today >= 0)
    {
        s_today = today;
    }

    if (s_enrolled_label == NULL || !ui_lvgl_is_ready() || !ui_lvgl_lock(20))
    {
        return;
    }
    refresh_counts_locked();
    ui_lvgl_unlock();
    ESP_LOGI(TAG, "主页计数已更新：已录入 %d，今日通行 %d", s_enrolled, s_today);
}
