/**
 * @file    ui_widgets.h
 * @brief   页面通用控件构造函数：标题栏、内容区、卡片、文字、按钮。
 *
 * @note    所有函数都必须在已持有 LVGL 锁的情况下调用（页面创建函数由
 *          ui_app_start() 在锁内统一调用）。
 */

#ifndef __UI_WIDGETS_H__
#define __UI_WIDGETS_H__

#include <stdbool.h>

#include "lvgl.h"
#include "ui_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 页面离开回调（切回主页前调用，内部已持有 LVGL 锁） */
typedef void (*lv_page_leave_cb_t)(void);

/**
 * @brief 注册"离开页面"回调
 * @param page 页面编号
 * @param cb   回调（可为 NULL 表示清除）
 * @note  只有标题栏的"返回"按钮会触发；页面内有定时器或后台状态时可借此复位。
 */
void ui_page_set_leave_cb(ui_page_t page, lv_page_leave_cb_t cb);

/**
 * @brief 设置标题栏"返回"按钮的目的页（默认回主页）
 * @param page  当前页面编号
 * @param target 目标页面（如删除人脸页 → 人脸注册页）
 * @note  须在页面创建后、锁内调用。未设置的页面仍返回主页。
 */
void ui_page_set_back_target(ui_page_t page, ui_page_t target);

/**
 * @brief 创建一个标准页面：深色背景 + 顶部标题栏 + 内容区
 * @param title     标题文字（UTF-8）
 * @param with_back 是否显示"返回"按钮（点击后回到该页设置的目标页，默认主页）
 * @return 内容区对象（位置 0,UI_HEADER_H，尺寸 480x(320-40-28)），失败返回 NULL
 * @note   顶部标题栏高 40 px，底部 28 px 留给全局状态栏
 */
lv_obj_t *ui_page_create(const char *title, bool with_back);

/** @brief 创建卡片容器（圆角、深色、无边框） */
lv_obj_t *ui_card_create(lv_obj_t *parent, int x, int y, int w, int h);

/**
 * @brief 创建文字标签
 * @param parent    父对象
 * @param text      文本（UTF-8）
 * @param font      字体（NULL 表示 UI_FONT_BODY）
 * @param color_hex 0xRRGGBB 颜色
 */
lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text, const lv_font_t *font,
                          uint32_t color_hex);

/**
 * @brief 创建按钮（蓝色圆角，居中文案）
 * @param cb 点击回调，可为 NULL
 * @param user_data 传给回调的用户数据
 */
lv_obj_t *ui_button_create(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                           lv_event_cb_t cb, void *user_data);

/**
 * @brief 创建可滑动的文字列表（每一项是独立可点标签，触摸拖动可滚动）
 * @param parent   父对象
 * @param x/y/w/h  位置与尺寸（高度超出部分靠上下滑动查看）
 * @param cb       点击某一项的回调；传给回调的 user_data 由 ui_list_add_item() 指定
 * @return 列表容器；失败返回 NULL
 * @note  不用 lv_roller：它的滚动条与文字样式使用 24 px 字体，而字库子集里
 *        未必包含所有需要的字符，这里统一用 UI_FONT_BODY。
 */
lv_obj_t *ui_list_create(lv_obj_t *parent, int x, int y, int w, int h);

/**
 * @brief 清空列表中的所有项
 * @param list ui_list_create() 返回的容器
 */
void ui_list_clear(lv_obj_t *list);

/**
 * @brief 向列表追加一项文字
 * @param list     列表容器
 * @param text     项文字（UTF-8，可含换行）
 * @param color_hex 文字颜色（0xRRGGBB）
 * @param cb       点击回调（可为 NULL）
 * @param user_data 传给回调的用户数据
 * @return 该项的标签对象（作为 user_data 传入回调）；失败返回 NULL
 */
lv_obj_t *ui_list_add_item(lv_obj_t *list, const char *text, uint32_t color_hex, lv_event_cb_t cb,
                           void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* __UI_WIDGETS_H__ */
