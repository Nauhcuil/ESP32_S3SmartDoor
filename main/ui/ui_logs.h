/**
 * @file    ui_logs.h
 * @brief   门禁记录页（M3）：分页展示 access_log 中的事件，支持清空。
 *
 * @note    - 页面布局与交互都在 ui_logs.c；ui_app_start() 在锁内调用 ui_logs_create()。
 *          - 读日志要访问 SPIFFS（写/读都会短暂关闭 Flash cache），**不能**在持有
 *            LVGL 锁时做，所以定时器先在锁外读、拿到锁后只做控件更新。
 *          - 分页不保存“页号→序号”整表，而是记住每页的 seq 上界：日志是只追加的，
 *            用 seq 上界翻页即使期间新增记录也不会重复/漏读同一批（新增记录只出现在
 *            第 0 页顶部，旧页内容保持不变）。
 */

#ifndef __UI_LOGS_H__
#define __UI_LOGS_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建门禁记录页（返回屏幕对象） */
lv_obj_t *ui_logs_create(void);

/**
 * @brief 离开记录页（由 ui_page_set_leave_cb 注册）
 * @note  只复位“下次进入要强制刷新”的标记，不改变当前页号——重新进来仍在原页。
 */
void ui_logs_leave(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_LOGS_H__ */
