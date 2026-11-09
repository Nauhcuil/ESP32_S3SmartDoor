/**
 * @file    ui_enroll.h
 * @brief   人脸注册页的交互接口：绑定控件、开始注册、人脸库列表与删除。
 *
 * @note    - 注册页布局在 ui_pages.c；这里只负责把"开始注册"按钮、状态标签与
 *            人脸库列表接到 face_service 上。
 *          - 注册/删除都会阻塞到 face_task 完成命令（最长 10 s），因此都在
 *            独立工作线程里执行，**不能**在 LVGL 回调里直接调用。
 *          - 定时器只在注册页可见时刷新；离开页面时调用 ui_enroll_leave() 复位。
 */

#ifndef __UI_ENROLL_H__
#define __UI_ENROLL_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 绑定注册页控件并启动状态刷新定时器
 * @param lbl_hint  实时状态文字（人脸/质量/录入结果/删除结果）
 * @param lbl_name  姓名标签（显示下一个自动编号 Person_N）
 * @param btn_start "开始注册"按钮
 * @param list      已录入人脸列表（ui_list_create() 返回的容器，可为 NULL）
 * @note  调用时须持有 LVGL 锁（由 ui_enroll_create() 在锁内调用）；内部锁是递归的。
 */
void ui_enroll_attach(lv_obj_t *lbl_hint, lv_obj_t *lbl_name, lv_obj_t *btn_start, lv_obj_t *list);

/**
 * @brief 绑定"删除人脸"页的人脸库列表
 * @param list ui_list_create() 返回的全宽列表容器
 * @note  在 ui_face_list_create() 中（锁内）调用。注册页与列表页共用同一个刷新
 *        定时器；列表页可见时才拉取并重建列表。绑定后强制下一次刷新重建。
 */
void ui_enroll_bind_face_list(lv_obj_t *list);

/**
 * @brief 离开注册页 / 人脸库页时复位状态（经由 ui_page_set_leave_cb 注册）
 * @note  不会打断正在进行的录入/删除。
 */
void ui_enroll_leave(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_ENROLL_H__ */
