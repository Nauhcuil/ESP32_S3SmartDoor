/**
 * @file    ui_home.h
 * @brief   主页交互接口（手动人脸识别）。
 *
 * @note    主页按钮回调只能做瞬时动作；本模块把识别请求转交给一个一次性
 *          worker 任务去阻塞等待 face_service 回执，避免卡住 LVGL 任务。
 */

#ifndef __UI_HOME_H__
#define __UI_HOME_H__

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 请求一次手动人脸识别（非阻塞）
 * @note  可在任意任务（包括 LVGL 回调）中调用。已经有一次识别在途时直接忽略
 *        本次点击并打印日志。
 */
void ui_home_recognize_async(void);

/**
 * @brief 是否有手动识别正在等待回执
 * @return true 表示“开始识别”已被点击、结果尚未返回
 * @note  face_task 用它保护状态卡：识别在途时不要把“识别中”改写成“请点开始识别”。
 */
bool ui_home_recognize_busy(void);

/**
 * @brief 在主页标题栏最右侧创建实时时钟（HH:MM，每秒刷新）
 * @param screen 主页屏幕对象（ui_page_create() 返回的 content 的父对象）
 * @note  由 ui_home_create() 在锁内调用；时间来源见 sys/clock_service。
 */
void ui_home_attach_clock(lv_obj_t *screen);

#ifdef __cplusplus
}
#endif

#endif /* __UI_HOME_H__ */
