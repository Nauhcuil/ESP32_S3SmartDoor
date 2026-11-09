/**
 * @file    ui_settings.h
 * @brief   设备设置页（M3）：识别阈值、活体开关、陌生人抓拍、密码、恢复出厂。
 *
 * @note    - 布局与交互都在 ui_settings.c。所有改动立即生效并写入 NVS（M2 的
 *            face_service / M3 的 app_nvs 已提供接口）；修改密码与恢复出厂需要先
 *            通过密码校验，密码只存在 NVS 里（出厂默认值见 app_config.h），不硬编码。
 *          - 会阻塞（写 Flash、清人脸库、清抓拍）的操作一律放在一次性工作线程里，
 *            LVGL 回调里绝不做阻塞调用。
 *          - 网络配置在 M4 接入；诊断页在 M6 接入，本页给出入口按钮但当前提示“未启用”。
 */

#ifndef __UI_SETTINGS_H__
#define __UI_SETTINGS_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建设备设置页（返回屏幕对象） */
lv_obj_t *ui_settings_create(void);

/**
 * @brief 离开设置页（由 ui_page_set_leave_cb 注册）
 * @note  关闭可能还开着的确认框/输入框，避免它们留在其他页面之上。
 */
void ui_settings_leave(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_SETTINGS_H__ */
