/**
 * @file    ui_home_api.h
 * @brief   主页状态卡更新接口（供 face_task 调用，不暴露 LVGL 对象）。
 *
 * @note    - 主页在 ui_pages.c 内部创建，face_task 只通过本接口更新文字，
 *            避免业务层直接操作 LVGL 控件。
 *            - 所有函数内部加 LVGL 锁，可从任意任务调用；LVGL 未就绪时安全返回。
 */

#ifndef __UI_HOME_API_H__
#define __UI_HOME_API_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 更新主页“识别状态”卡片
 * @param text  主状态文字（UTF-8 中文，如“识别成功”“请正对摄像头”）
 * @param color 文字颜色（0xRRGGBB）
 */
void ui_home_set_state(const char *text, uint32_t color);

/**
 * @brief 更新主页人数与今日通行次数
 * @param enrolled 已注册人数（负数表示保持不变）
 * @param today    今日通行次数（负数表示保持不变）
 */
void ui_home_set_counts(int enrolled, int today);

/**
 * @brief 主页创建时登记状态卡控件（由 ui_pages.c 调用，调用时须持有 LVGL 锁）
 * @param state_label    主状态文字控件
 * @param enrolled_label “已录入”计数控件
 * @param today_label    “今日通行”计数控件
 * @note 登记后会把创建前暂存的最新状态回放到控件上，避免启动阶段丢字。
 */
struct _lv_obj_t;
void ui_home_register_widgets(struct _lv_obj_t *state_label, struct _lv_obj_t *enrolled_label,
                              struct _lv_obj_t *today_label);

#ifdef __cplusplus
}
#endif

#endif /* __UI_HOME_API_H__ */
