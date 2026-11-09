/**
 * @file    ui_lvgl_port.h
 * @brief   LVGL 与硬件（ST7796 面板 + FT6X36 触摸）的对接层。
 *
 * @note    - ui_lvgl_init() 必须在 lcd_init() 与 tp_init() 成功之后调用。
 *          - 除 LVGL 自己的任务外，任何任务调用 LVGL API 都必须用
 *            ui_lvgl_lock(timeout_ms) / ui_lvgl_unlock() 包裹。
 *          - 本模块只负责初始化与加锁封装，不创建任何页面。
 */

#ifndef __UI_LVGL_PORT_H__
#define __UI_LVGL_PORT_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 是否已将显示注册到 LVGL（AI 任务据此决定走 LVGL 预览还是直绘回退） */
bool ui_lvgl_is_ready(void);

/** @brief 获取 LVGL 显示对象（未初始化时返回 NULL） */
lv_disp_t *ui_lvgl_get_disp(void);

/**
 * @brief 初始化 LVGL、注册显示与触摸输入
 * @return ESP_OK 成功；其余：面板/触摸未就绪或内存不足
 * @note  面板方向已由 lcd_init() 的 MADCTL 与 tp.c 的坐标变换处理，此处旋转参数全为 0
 */
esp_err_t ui_lvgl_init(void);

/**
 * @brief 获取 LVGL 全局锁
 * @param timeout_ms 0 表示一直等待
 * @return true 成功；false 超时或 LVGL 未初始化
 */
bool ui_lvgl_lock(uint32_t timeout_ms);

/** @brief 释放 LVGL 全局锁 */
void ui_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_LVGL_PORT_H__ */
