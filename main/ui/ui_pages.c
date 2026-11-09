/**
 * @file    ui_pages.c
 * @brief   主页 / 人脸注册 / 人脸库（删除人脸）三页的布局实现。
 *
 * @note    - 注册页的交互（开始注册按钮、实时状态刷新、人脸库列表与删除确认）在
 *            ui_enroll.c，这里只做布局并把控件交给 ui_enroll_attach()。
 *          - 门禁记录、设备设置、设备诊断三页在 M3 拆成独立模块：
 *            ui_logs.c / ui_settings.c / ui_diag.c（声明在各自头文件，ui_pages.h 统一
 *            转发给 ui_app.c）。
 *          - 布局按 480x320 横屏：标题栏 40 px、内容区 252 px、状态栏 28 px。
 *          - 文字一律取自 ui_strings.h，便于字库子集化。
 */

#include "ui_pages.h"

#include <stdint.h>

#include "esp_log.h"
#include "ui_app.h"
#include "ui_enroll.h"
#include "ui_home.h"
#include "ui_home_api.h"
#include "ui_preview.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui_pages"; // 日志标签

// =================================================================
// 主页
// =================================================================

/**
 * @brief 主页按钮回调：切换到对应页面
 * @note  user_data 存放 ui_page_t 的整数（转换为指针）
 */
static void home_nav_cb(lv_event_t *e)
{
    ui_page_t page = (ui_page_t)(uintptr_t)lv_event_get_user_data(e);
    ui_app_switch_page(page);
}

/**
 * @brief 主页“开始识别”按钮回调
 * @note  只做一次非阻塞请求：真正的识别在 ui_home.c 创建的 worker 任务里执行，
 *        LVGL 任务不会被阻塞，预览与按钮响应保持流畅。
 */
static void home_recognize_cb(lv_event_t *e)
{
    (void)e;
    ui_home_recognize_async();
}

lv_obj_t *ui_home_create(void)
{
    lv_obj_t *content = ui_page_create(UI_TXT_APP_NAME, false);
    if (content == NULL)
    {
        return NULL;
    }

    // ---- 左侧摄像头预览区（由 ui_preview 在 LVGL 锁内直绘到面板）----
    if (ui_preview_create(content, 0, 0, UI_PREVIEW_W, UI_PREVIEW_H) == NULL)
    {
        ESP_LOGW(TAG, "预览区创建失败，界面继续运行但不显示摄像头画面");
        lv_obj_t *hint =
            ui_label_create(content, UI_TXT_HOME_FACE_HINT, UI_FONT_TITLE, UI_COLOR_TEXT_DIM);
        lv_obj_set_pos(hint, 8, 8);
    }

    // ---- 右侧识别状态卡片 ----
    const int right_x = UI_PREVIEW_W + UI_PAD;        // 328
    const int right_w = UI_SCREEN_W - UI_PAD - right_x; // 144
    lv_obj_t *status_card = ui_card_create(content, right_x, 0, right_w, 84);

    lv_obj_t *waiting =
        ui_label_create(status_card, UI_TXT_HOME_WAITING, UI_FONT_TITLE, UI_COLOR_TEXT);
    lv_obj_set_pos(waiting, 0, 0);

    lv_obj_t *enrolled =
        ui_label_create(status_card, UI_TXT_HOME_ENROLLED " 0", UI_FONT_BODY, UI_COLOR_TEXT_DIM);
    lv_obj_set_pos(enrolled, 0, 34);

    lv_obj_t *today = ui_label_create(status_card,
                                      UI_TXT_HOME_TODAY_PASS " 0",
                                      UI_FONT_BODY,
                                      UI_COLOR_TEXT_DIM); // 今日通行次数（M3 日志接入后更新）
    lv_obj_set_pos(today, 0, 58);

    // ---- 右侧按钮：识别（整行，最常用）+ 三个功能入口 ----
    // 内容区高 252：84（状态卡）+ 8 + 52（识别）+ 8 + 44 + 8 + 44 = 248，正好放得下。
    // 设备诊断页保留在 ui_app 的页面枚举里（后续 M6 接入环境数据），只是主页不再给
    // 入口按钮——按钮太多会让右侧一排挤成两行 4 个，触控目标过小。
    const int btn_w = (right_w - 8) / 2; // 68
    const int btn_recognize_y = 92;
    const int btn_recognize_h = 52;
    const int btn_y1 = btn_recognize_y + btn_recognize_h + 8; // 152
    const int btn_h2 = 44;
    const int btn_y2 = btn_y1 + btn_h2 + 8; // 204

    ui_button_create(content, UI_TXT_HOME_MANUAL_RECOGNIZE, right_x, btn_recognize_y, right_w,
                     btn_recognize_h, home_recognize_cb, NULL);

    struct
    {
        const char *text;
        ui_page_t page;
        int x;
        int y;
        int w;
    } buttons[] = {
        {UI_TXT_HOME_ENROLL, UI_PAGE_ENROLL, right_x, btn_y1, btn_w},
        {UI_TXT_HOME_LOGS, UI_PAGE_LOGS, right_x + btn_w + 8, btn_y1, btn_w},
        {UI_TXT_HOME_SETTINGS, UI_PAGE_SETTINGS, right_x, btn_y2, right_w},
    };

    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
    {
        ui_button_create(content,
                         buttons[i].text,
                         buttons[i].x,
                         buttons[i].y,
                         buttons[i].w,
                         btn_h2,
                         home_nav_cb,
                         (void *)(uintptr_t)buttons[i].page);
    }

    // 登记状态卡控件：M2 起由 face_task 更新“识别状态/已录入/今日通行”
    ui_home_register_widgets(waiting, enrolled, today);

    // 标题栏最右侧的实时时钟（时间来源见 sys/clock_service）
    ui_home_attach_clock(lv_obj_get_parent(content));

    return lv_obj_get_parent(content);
}

// =================================================================
// 人脸注册
// =================================================================

lv_obj_t *ui_enroll_create(void)
{
    lv_obj_t *content = ui_page_create(UI_TXT_ENROLL_TITLE, true);
    if (content == NULL)
    {
        return NULL;
    }

    // ---- 左侧：摄像头预览（320x240，与主页同一套 ui_preview 实现）----
    if (ui_preview_create(content, 0, 0, UI_PREVIEW_W, UI_PREVIEW_H) == NULL)
    {
        ESP_LOGW(TAG, "注册页预览区创建失败，界面继续运行但看不到画面");
    }

    // ---- 右侧：姓名 + 实时状态 + 两个操作 ----
    // 右侧只有 135 px 宽，放不下人脸库列表（行内既有名字又有删除提示，还要留够
    // 触摸间距）。所以列表移到独立的“删除人脸”页（UI_PAGE_FACE_LIST），
    // 注册页这里只留一个入口按钮。
    const int right_x = UI_PREVIEW_W + UI_PAD; // 328
    const int right_w = 464 - right_x - 1;     // 135

    lv_obj_t *name =
        ui_label_create(content, UI_TXT_ENROLL_NAME ": Person_0", UI_FONT_BODY, UI_COLOR_TEXT_DIM);
    lv_obj_set_pos(name, right_x, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(name, right_w);

    lv_obj_t *hint = ui_label_create(content, UI_TXT_ENROLL_FACE, UI_FONT_BODY, UI_COLOR_TEXT_DIM);
    lv_obj_set_pos(hint, right_x, 24);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, right_w);

    // 开始注册按钮：稍上移并加高，手指更容易点中（右侧列只有 135 px 宽）
    const int btn_y_start = 86;
    const int btn_h = 42;
    lv_obj_t *btn =
        ui_button_create(content, UI_TXT_ENROLL_START, right_x, btn_y_start, right_w, btn_h,
                         NULL, NULL);

    // 删除人脸入口：整行按钮，跳到独立的列表页（那里宽度 464 px，行高 40 px）
    const int btn_y_delete = btn_y_start + btn_h + 10; // 138
    ui_button_create(content, UI_TXT_ENROLL_DELETE, right_x, btn_y_delete, right_w, btn_h,
                     home_nav_cb, (void *)(uintptr_t)UI_PAGE_FACE_LIST);

    // 交互（开始注册 / 实时状态）由 ui_enroll 模块接管；人脸库列表绑定在
    // ui_face_list_create() 里（同一个模块负责两页的数据刷新与删除确认）。
    ui_enroll_attach(hint, name, btn, NULL);
    ui_page_set_leave_cb(UI_PAGE_ENROLL, ui_enroll_leave);

    return lv_obj_get_parent(content);
}

// =================================================================
// 人脸库（删除人脸）
// =================================================================

lv_obj_t *ui_face_list_create(void)
{
    lv_obj_t *content = ui_page_create(UI_TXT_ENROLL_DELETE, true);
    if (content == NULL)
    {
        return NULL;
    }

    // 内容区 464x236：一行提示 + 全宽列表（行高 40 px，可放 4 行，更多上下滑动）
    lv_obj_t *hint = ui_label_create(content, UI_TXT_FACE_LIST_HINT, UI_FONT_BODY,
                                     UI_COLOR_TEXT_DIM);
    lv_obj_set_pos(hint, 0, 0);

    lv_obj_t *list = ui_list_create(content, 0, 24, 464, 212);

    ui_enroll_bind_face_list(list);
    ui_page_set_leave_cb(UI_PAGE_FACE_LIST, ui_enroll_leave);
    // 删除人脸是从注册页进来的，返回也应该回到注册页（原来直接回主页，用户要重走一遍）
    ui_page_set_back_target(UI_PAGE_FACE_LIST, UI_PAGE_ENROLL);

    return lv_obj_get_parent(content);
}

