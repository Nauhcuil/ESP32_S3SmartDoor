/**
 * @file    ui_theme.h
 * @brief   LVGL 界面配色与尺寸常量（深色安防风格，480x320 横屏）。
 *
 * @note    1. 颜色一律用 lv_color_hex() 书写，保证 0xRRGGBB 中 R=0xFF 时
 *             与 R=0x00 显示一致（直接给 lv_color_t 赋 u16 会因 LV_COLOR_16_SWAP
 *             与 RGB565 的字段互换而让红色变蓝色）。
 *          2. 尺寸按 480x320 设计：顶部标题栏 40 px，底部状态栏 28 px。
 *          3. 新增页面优先复用 UI_COLOR_* 与 UI_FONT_*，避免各页面各写一套。
 */

#ifndef __UI_THEME_H__
#define __UI_THEME_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 尺寸 ==================== */
#define UI_SCREEN_W         (480)
#define UI_SCREEN_H         (320)
#define UI_HEADER_H         (40)  /**< 顶部标题栏高度 */
#define UI_STATUS_H         (28)  /**< 底部状态栏高度 */
#define UI_PREVIEW_W        (320) /**< 摄像头预览宽度（摄像头 QVGA） */
#define UI_PREVIEW_H        (240) /**< 摄像头预览高度 */
#define UI_PAD              (8)   /**< 通用内边距 */
#define UI_LIST_ITEM_H      (28)  /**< ui_list 每一项的高度 */

/* ==================== 颜色 ==================== */
#define UI_COLOR_BG         0x101418 /**< 页面背景（近黑蓝） */
#define UI_COLOR_HEADER     0x1B2230 /**< 标题栏 */
#define UI_COLOR_CARD       0x1E2633 /**< 卡片 / 次级面板 */
#define UI_COLOR_TEXT       0xE8EDF2 /**< 主文字（浅灰白） */
#define UI_COLOR_TEXT_DIM   0x8E9AA8 /**< 次要文字 */
#define UI_COLOR_ACCENT     0x2E7DFF /**< 强调色（蓝） */
#define UI_COLOR_OK         0x2ECC71 /**< 成功（绿） */
#define UI_COLOR_WARN       0xF5A623 /**< 警告（橙） */
#define UI_COLOR_DANGER     0xE74C3C /**< 危险 / 拒绝（红） */

/* ==================== 字体 ==================== */
LV_FONT_DECLARE(font_cn_16); /**< 16px 中文字库（正文） */
LV_FONT_DECLARE(font_cn_24); /**< 24px 中文字库（标题） */
LV_FONT_DECLARE(lv_font_montserrat_16); /**< 内置 ASCII 回退字体 */
LV_FONT_DECLARE(lv_font_montserrat_24);

#define UI_FONT_BODY        (&font_cn_16)
#define UI_FONT_TITLE       (&font_cn_24)

#ifdef __cplusplus
}
#endif

#endif /* __UI_THEME_H__ */
