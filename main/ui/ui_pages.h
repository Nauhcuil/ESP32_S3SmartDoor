/**
 * @file    ui_pages.h
 * @brief   各页面的创建入口（每个页面返回其屏幕对象 lv_obj_t*）。
 *
 * @note    所有函数都必须在持有 LVGL 锁时调用，由 ui_app_start() 统一创建。
 *          M3 起门禁记录/设备设置/设备诊断三页各自独立成模块（ui_logs / ui_settings /
 *          ui_diag），主页、注册页与人脸库页仍在本头文件声明。
 */

#ifndef __UI_PAGES_H__
#define __UI_PAGES_H__

#include "lvgl.h"

/* M3 独立页面模块的创建函数（各自的头文件也声明了同样的函数，这里统一转发一次，
 * 方便 ui_app.c 只包含 ui_pages.h） */
#include "ui_diag.h"
#include "ui_logs.h"
#include "ui_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_home_create(void);      /**< 主页 */
lv_obj_t *ui_enroll_create(void);    /**< 人脸注册 */
lv_obj_t *ui_face_list_create(void); /**< 人脸库（删除人脸），从注册页进入 */

#ifdef __cplusplus
}
#endif

#endif /* __UI_PAGES_H__ */
