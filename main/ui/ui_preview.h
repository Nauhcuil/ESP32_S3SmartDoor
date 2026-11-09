/**
 * @file    ui_preview.h
 * @brief   摄像头预览：帧先拷贝到 PSRAM 缓冲，再由 LVGL 的刷新路径从缓冲里绘制。
 *
 * @note    - ui_preview_create() 在持有 LVGL 锁时调用（主页 / 注册页创建时）：创建外框
 *            与首帧提示文字。所有预览区共用同一套 PSRAM 图像缓冲，尺寸必须一致。
 *          - ui_preview_push_frame() 由 LCD 任务（非 LVGL 线程）调用：把整帧换算成
 *            lv_color_t 写入“后台”图像缓冲，返回后即可归还 camera_fb_t。
 *          - **不在推送时直接改图像控件**：lv_img_set_src + lv_obj_invalidate 在
 *            LVGL 之外调用，会与 LVGL 任务正在进行的刷新交错（下一轮刷新在读取的
 *            仍是旧缓冲时又把旧缓存区重画一遍），表现为屏幕花屏 / 闪屏。
 *            因此改由 ui_preview_service()（LVGL 定时器，每 UI_PREVIEW_SERVICE_MS）
 *            在 LVGL 任务内完成“切换缓冲 + 标脏”，线程安全且只在有新帧时才重绘。
 *          - 只绘制主页与注册页（其他页面没有预览区），省下拷贝与重绘开销。
 */

#ifndef __UI_PREVIEW_H__
#define __UI_PREVIEW_H__

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 预览服务周期（毫秒）
 * @note  只决定“后台缓冲被交给 lv_img 的最长延迟”：LCD 任务推帧时会拒绝覆盖尚未
 *        被取走的缓冲。ui_preview_service() 在 LVGL 任务内立即刷新失效区域，不再等待
 *        下一轮默认刷新周期；20 ms 服务周期减少新帧等待时间。定时器本身空转几乎不耗
 *        CPU，因为它只在有新帧时才切缓冲并刷新。
 */
#define UI_PREVIEW_SERVICE_MS 20

/**
 * @brief 创建预览区
 * @param parent 父对象（页面内容区）
 * @param x/y    相对父对象的位置
 * @param w/h    预览尺寸，应与摄像头输出一致（320x240）
 * @return 外框对象；内存不足/数量超限/尺寸不一致返回 NULL
 * @note  调用方须持有 LVGL 锁。
 */
lv_obj_t *ui_preview_create(lv_obj_t *parent, int x, int y, int w, int h);

/**
 * @brief 提交一帧摄像头数据（在非 LVGL 线程调用）
 * @param rgb565 帧数据（camera_fb_t::buf，RGB565）
 * @param w/h    图像尺寸
 * @return true 已提交；false 预览未创建/尺寸不符/当前页面无预览区/后台缓冲忙
 * @note  返回前整帧已写入后台缓冲，返回后即可归还 camera_fb_t；屏幕更新由
 *        ui_preview_service() 在 LVGL 任务内完成。
 */
bool ui_preview_push_frame(const uint16_t *rgb565, int w, int h);

/**
 * @brief 预览服务：在 LVGL 任务内把新帧交给图像控件
 * @note  由 LVGL 定时器周期调用（不需要也不应再加锁）。
 */
void ui_preview_service(void);

/** @brief 是否已经收到过至少一帧 */
bool ui_preview_has_frame(void);

/**
 * @brief  获取成功绘制的累计帧数（sys_monitor 以此计算实时预览帧率 preview=）
 * @return 累计帧数，控件未创建时返回 0
 */
uint32_t ui_preview_get_push_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_PREVIEW_H__ */
