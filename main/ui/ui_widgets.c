/**
 * @file    ui_widgets.c
 * @brief   页面通用控件实现：标题栏、内容区、卡片、文字、按钮。
 *
 * @note    统一在这里设置样式，页面代码只关心布局与文案，便于后续整体换肤。
 */

#include "ui_widgets.h"

#include "ui_app.h"
#include "ui_strings.h"
#include "ui_theme.h"

/**
 * @brief  离开页面钩子（按页面注册，切回主页时调用一次）
 * @note   页面内的定时器/后台状态需要在离开时复位，又不能把 lv_obj 回调写到
 *         ui_app.c 里，所以在这里按页面编号挂一个回调；回调内已持有 LVGL 锁。
 */
static lv_page_leave_cb_t s_leave_cbs[UI_PAGE_MAX] = {NULL};

/** @brief 各页面"返回"按钮的目标页（默认主页） */
static ui_page_t s_back_targets[UI_PAGE_MAX] = {UI_PAGE_HOME};

/**
 * @brief "返回"按钮回调：先通知当前页面做离开清理，再切到该页的目标页
 */
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    const ui_page_t cur = ui_app_current_page();
    if (cur < UI_PAGE_MAX && s_leave_cbs[cur] != NULL)
    {
        s_leave_cbs[cur]();
    }
    const ui_page_t target = cur < UI_PAGE_MAX ? s_back_targets[cur] : UI_PAGE_HOME;
    ui_app_switch_page(target);
}

void ui_page_set_back_target(ui_page_t page, ui_page_t target)
{
    if (page < UI_PAGE_MAX && target < UI_PAGE_MAX)
    {
        s_back_targets[page] = target;
    }
}

void ui_page_set_leave_cb(ui_page_t page, lv_page_leave_cb_t cb)
{
    if (page < UI_PAGE_MAX)
    {
        s_leave_cbs[page] = cb;
    }
}

/**
 * @brief 创建顶部标题栏（480x40）
 */
static void create_header(lv_obj_t *scr, const char *title, bool with_back)
{
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, UI_SCREEN_W, UI_HEADER_H);
    lv_obj_set_style_bg_color(header, lv_color_hex(UI_COLOR_HEADER), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = ui_label_create(header, title, UI_FONT_TITLE, UI_COLOR_TEXT);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 12, 0);

    if (with_back)
    {
        lv_obj_t *back = ui_button_create(header, UI_TXT_RETURN, UI_SCREEN_W - 12 - 72,
                                          (UI_HEADER_H - 30) / 2, 72, 30, back_btn_cb, NULL);
        (void)back;
    }
}

lv_obj_t *ui_page_create(const char *title, bool with_back)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    if (scr == NULL)
    {
        return NULL;
    }

    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_header(scr, title, with_back);

    // 内容区：标题栏与底部状态栏之间
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_pos(content, 0, UI_HEADER_H);
    lv_obj_set_size(content, UI_SCREEN_W, UI_SCREEN_H - UI_HEADER_H - UI_STATUS_H);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, UI_PAD, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    return content;
}

lv_obj_t *ui_card_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text, const lv_font_t *font,
                          uint32_t color_hex)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, font ? font : UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color_hex), 0);
    return label;
}

lv_obj_t *ui_button_create(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                           lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t *label = ui_label_create(btn, text, UI_FONT_BODY, UI_COLOR_TEXT);
    lv_obj_center(label);

    if (cb != NULL)
    {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

lv_obj_t *ui_list_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_pos(list, x, y);
    lv_obj_set_size(list, w, h);
    lv_obj_set_style_bg_color(list, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_40, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_border_color(list, lv_color_hex(UI_COLOR_HEADER), 0);
    lv_obj_set_style_radius(list, 6, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    // **必须用 flex 纵向排列**：lv_obj_create() 默认 layout=LV_LAYOUT_NONE（实机打印
    // 确认过），子项都停在 (0,0)，多个条目会完全重叠在同一位置——表现为“人名全挤在
    // 一起”，点一下只删掉最上面那个，连续点会一次删好几个。flex 列布局让每项依次
    // 向下排（实测第 1 项 y=0、第 2 项 y=36）。
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, 0, 0);
    // 竖向滚动：一屏放不下时上下滑动查看；横向不滚动
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
    return list;
}

void ui_list_clear(lv_obj_t *list)
{
    if (list != NULL)
    {
        lv_obj_clean(list); // 删除全部子项；容器自身保留
    }
}

lv_obj_t *ui_list_add_item(lv_obj_t *list, const char *text, uint32_t color_hex, lv_event_cb_t cb,
                           void *user_data)
{
    if (list == NULL || text == NULL)
    {
        return NULL;
    }

    // 透明背景的行：本身即触摸目标，点击/拖动都由 LVGL 处理
    lv_obj_t *item = lv_obj_create(list);
    lv_obj_set_size(item, lv_obj_get_content_width(list), UI_LIST_ITEM_H);
    lv_obj_set_style_bg_color(item, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_50, 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(UI_COLOR_HEADER), 0);
    lv_obj_set_style_radius(item, 4, 0);
    lv_obj_set_style_pad_left(item, 8, 0);
    lv_obj_set_style_pad_right(item, 8, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = ui_label_create(item, text, UI_FONT_BODY, color_hex);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    if (cb != NULL)
    {
        lv_obj_add_event_cb(item, cb, LV_EVENT_CLICKED, user_data);
    }
    return label;
}
