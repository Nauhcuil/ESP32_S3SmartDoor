/**
 * @file    ui_app.h
 * @brief   应用界面管理：页面切换、底部状态栏、模拟开门覆盖层。
 *
 * @note    - ui_app_start() 必须在 ui_lvgl_init() 成功之后调用；
 *            内部自己获取 LVGL 锁，调用者不要再持锁。
 *          - 除 ui_app_start() 内部外，本模块的对外接口都线程安全（内部加锁），
 *            任何任务都可以直接调用。
 *          - 所有中文文案来自 ui_strings.h。
 */

#ifndef __UI_APP_H__
#define __UI_APP_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 页面标识 */
typedef enum
{
    UI_PAGE_HOME = 0, /**< 主页：预览 + 识别状态 + 功能入口 */
    UI_PAGE_ENROLL,   /**< 人脸注册 */
    UI_PAGE_LOGS,     /**< 门禁记录 */
    UI_PAGE_SETTINGS, /**< 设备设置 */
    UI_PAGE_DIAG,     /**< 设备诊断 */
    UI_PAGE_FACE_LIST, /**< 人脸库（删除人脸）：全宽列表，从注册页进入 */
    UI_PAGE_MAX,
} ui_page_t;

/**
 * @brief 创建界面并加载主页
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示 LVGL 未初始化或锁超时
 */
esp_err_t ui_app_start(void);

/**
 * @brief 切换到指定页面
 * @param page 目标页面
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 页面越界；ESP_ERR_TIMEOUT 锁超时
 */
esp_err_t ui_app_switch_page(ui_page_t page);

/** @brief 返回主页 */
esp_err_t ui_app_goto_home(void);

/** @brief 获取当前页面 */
ui_page_t ui_app_current_page(void);

/**
 * @brief 在底部状态栏显示一条提示（3 秒后自动清除）
 * @param text 提示文本（UTF-8），内部复制，最长 63 字节
 * @param color_hex 文字颜色（0xRRGGBB），如 UI_COLOR_OK / UI_COLOR_DANGER
 * @note  仅更新文本内容，不会切换页面；文本未变化时不重绘，避免闪烁
 */
void ui_app_set_status(const char *text, uint32_t color_hex);

/** @brief 立即清除底部状态栏 */
void ui_app_clear_status(void);

/**
 * @brief 显示模拟开门覆盖层
 * @param title 覆盖层主标题（如 UI_TXT_DOOR_WELCOME）
 * @param subtitle 副标题（如姓名或原因），可为 NULL
 * @param ok true=绿色成功样式，false=红色拒绝样式
 * @param timeout_ms 显示时长，到时自动隐藏
 */
void ui_app_show_door_overlay(const char *title, const char *subtitle, bool ok,
                              uint32_t timeout_ms);

/** @brief 立即隐藏开门覆盖层 */
void ui_app_hide_door_overlay(void);

/** @brief 开门覆盖层的来源（M3 日志 / M5 播报用） */
typedef enum
{
    UI_OVERLAY_REASON_NONE = 0, /**< 普通提示（拒绝/失败等） */
    UI_OVERLAY_REASON_FACE,     /**< 人脸识别开门 */
    UI_OVERLAY_REASON_REMOTE,   /**< 远程开门 */
    UI_OVERLAY_REASON_VOICE,    /**< 语音开门 */
} ui_overlay_reason_t;

/**
 * @brief 读取最近一次覆盖层的开门来源
 * @return 来源枚举；未开门时为 UI_OVERLAY_REASON_NONE
 */
ui_overlay_reason_t ui_app_get_overlay_reason(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_APP_H__ */
