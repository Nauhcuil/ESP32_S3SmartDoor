/**
 * @file    ui_diag.h
 * @brief   设备诊断页（M3 起先显示实时系统数据，M6 再补环境传感器与 AI 答疑入口）。
 *
 * @note    - 页面布局与刷新在 ui_diag.c；入口在设置页（主页按钮过密，不再放诊断入口）。
 *          - 数据来自 sys_monitor_get_status()（帧率/耗时/堆内存）、storage_usage()、
 *            snapshot_count()、face_service_get_status() 与 app_config.h 的版本号。
 *          - 刷新定时器每 1 s 一次，只在诊断页可见时读数据；读 SPIFFS 用量较慢，
 *            所以取 LVGL 锁之外先采集，拿锁后只更新文字。
 */

#ifndef __UI_DIAG_H__
#define __UI_DIAG_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建设备诊断页（返回屏幕对象） */
lv_obj_t *ui_diag_create(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_DIAG_H__ */
