/**
 * @file    ui_preview.c
 * @brief   摄像头预览：把帧写进 PSRAM 图像缓冲，由 LVGL 在刷新路径里绘制。
 *
 * @note    为什么是“后台缓冲 + LVGL 任务内切换”而不是“推送时直接改控件”：
 *          - 推送发生在 LCD 任务里。若在推送时调用 lv_img_set_src() +
 *            lv_obj_invalidate()，与 LVGL 任务自己的刷新是并发的：LVGL 可能刚
 *            标脏准备重画，推送又把 src 换成了另一块缓冲，同一区域就会被反复
 *            重画 / 半帧数据被读走，实测表现为花屏、闪屏。
 *          - 现在推送只往 PSRAM 的后台缓冲写数据并置“有新帧”标记；真正的
 *            切换 src + invalidate 由 ui_preview_service()（LVGL 定时器）在
 *            LVGL 任务内完成，并立即刷新失效区域，避免再等待 LVGL 的下一轮周期刷新。
 *          - 像素换算：摄像头输出的是“高字节在前”的 RGB565 字节流（首字节 =
 *            RRRRRGGG），LV_COLOR_16_SWAP 下 lv_color_t 的内存布局也是
 *            [RRRRRGGG][GGGBBBBB]。所以两者逐字节相同，**原样 memcpy** 即可，
 *            不要按位域重排（重排会得到 R/B 互换的画面）。
 *          - 帧缓冲在 PSRAM，LVGL 读它由 CPU 拷进内部 RAM 的 20 行绘制缓冲，
 *            不经过 SPI DMA，因此不会出现当初直绘整帧时的 “DMA TX underflow”。
 *          - 三缓冲（UI_PREVIEW_BUFS）：交给 lv_img 的新帧与面板上仍在显示的
 *            旧帧都不能被 LCD 任务覆盖；两块缓冲时写目标必然是其中之一，而 LCD
 *            任务优先级（5）高于 LVGL 任务（0），切换瞬间会抢先把正在显示的
 *            缓冲写花——现场看到的就是“画面一帧一帧平移 + 彩色花屏”。
 *          - 支持多个预览区（主页 + 注册页）：共用同一组图像缓冲，每帧只更新
 *            “当前可见页面”上的控件。
 *          - 帧限速在 LCD 任务里（45 ms，约 22 fps，见 who_human_face_recognition.cpp
 *            的 task_process_lcd 注释），这里不再限速。
 */

#include "ui_preview.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui_app.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui_preview"; // 日志标签

/** @brief 最多同时存在的预览区（主页 + 注册页） */
#define UI_PREVIEW_MAX 2

/**
 * @brief 图像缓冲数量（三缓冲）
 * @note  两块不够用：ui_preview_service() 把新缓冲交给 lv_img 后，LVGL 真正重画
 *        面板还要等下一次 lv_timer_handler（最长 task_max_sleep_ms = 30 ms）。
 *        这两块缓冲（“刚交给 lv_img 的”和“面板上仍在显示的旧帧”）都不能被 LCD
 *        任务覆盖，因此写目标必须是第三块。两缓冲时 LCD 任务（优先级 5）会在
 *        切换瞬间抢占 LVGL 任务（优先级 0），把正在显示的缓冲写花——现场表现
 *        就是“画面一帧一帧平移 + 彩色花屏”。三缓冲彻底消除这个窗口。
 */
#define UI_PREVIEW_BUFS 3

/** @brief 一个预览区：外框 + 图像 + 首帧前的提示文字 */
typedef struct
{
    lv_obj_t *box;  /**< 外框（卡片底色，圆角） */
    lv_obj_t *img;  /**< 图像控件 */
    lv_obj_t *hint; /**< 首帧前的提示文字，收到第一帧后隐藏 */
} preview_widget_t;

/** @brief 已创建的预览区 */
static preview_widget_t s_widgets[UI_PREVIEW_MAX];
static int s_widget_count = 0;

/** @brief 图像描述符（每个对应一块缓冲） */
static lv_img_dsc_t s_dsc[UI_PREVIEW_BUFS];
/** @brief PSRAM 图像缓冲（RGB565，原样存放摄像头帧） */
static uint8_t *s_buf[UI_PREVIEW_BUFS] = {NULL, NULL, NULL};
/** @brief 图像尺寸（所有预览区必须一致） */
static int s_w = 0;
static int s_h = 0;
/** @brief 下一帧要写入的缓冲索引（LCD 任务写） */
static int s_back = 0;
/** @brief 已经交给 lv_img 的缓冲索引（LVGL 正在/将要绘制它） */
static int s_front = 0;
/** @brief 上一块交给 lv_img 的缓冲索引（面板上可能仍是它的内容，尚未重画完） */
static int s_prev_front = -1;
/** @brief 已拷贝好、等待 ui_preview_service() 显示的缓冲索引（-1 = 无新帧） */
static volatile int s_pending_idx = -1;
/** @brief 是否已收到过帧 */
static bool s_has_frame = false;
/** @brief 成功绘制的帧计数（供 sys_monitor 统计预览帧率） */
static volatile uint32_t s_push_count = 0;

/**
 * @brief 选择一个可以安全写入的缓冲索引
 * @return 既不是 s_front（即将显示）也不是 s_prev_front（面板上可能还在显示）的索引
 * @note  三块缓冲下满足条件的索引唯一；s_prev_front 为 -1（首帧）时任选一块非 front。
 *        调用方必须已持有 LVGL 锁（本函数只在 ui_preview_service() 内调用）。
 */
static int pick_back_buffer(void)
{
    for (int i = 0; i < UI_PREVIEW_BUFS; i++)
    {
        if (i != s_front && i != s_prev_front)
        {
            return i;
        }
    }
    return (s_front + 1) % UI_PREVIEW_BUFS; // 理论不可达（三块缓冲必有空闲）
}

/**
 * @brief 分配图像缓冲并初始化描述符（只在第一个预览区创建时执行一次）
 * @param w/h 图像尺寸
 * @return true 成功
 */
static bool ensure_buffers(int w, int h)
{
    if (s_buf[0] != NULL)
    {
        return true;
    }

    const size_t bytes = (size_t)w * (size_t)h * sizeof(lv_color_t);
    for (int i = 0; i < UI_PREVIEW_BUFS; i++)
    {
        s_buf[i] = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_buf[i] == NULL)
        {
            ESP_LOGE(TAG, "图像缓冲分配失败（%u 字节 x%d，PSRAM）", (unsigned)bytes,
                     UI_PREVIEW_BUFS);
            for (int k = 0; k < i; k++)
            {
                heap_caps_free(s_buf[k]);
                s_buf[k] = NULL;
            }
            return false;
        }
        memset(s_buf[i], 0, bytes); // 首帧前显示纯黑，避免看到随机数据
    }

    for (int i = 0; i < UI_PREVIEW_BUFS; i++)
    {
        s_dsc[i].header.always_zero = 0;
        s_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        s_dsc[i].header.w = (uint32_t)w;
        s_dsc[i].header.h = (uint32_t)h;
        s_dsc[i].data_size = (uint32_t)bytes;
        s_dsc[i].data = s_buf[i];
    }

    s_w = w;
    s_h = h;
    s_front = 0;
    s_prev_front = -1;
    s_back = 1;
    s_pending_idx = -1;
    return true;
}

lv_obj_t *ui_preview_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    if (parent == NULL || w <= 0 || h <= 0)
    {
        return NULL;
    }
    if (s_widget_count >= UI_PREVIEW_MAX)
    {
        ESP_LOGW(TAG, "预览区数量已达上限（%d），忽略本次创建", UI_PREVIEW_MAX);
        return NULL;
    }
    if (s_w != 0 && (w != s_w || h != s_h))
    {
        // 所有预览区共用同一对缓冲，尺寸必须一致（摄像头固定 QVGA）
        ESP_LOGE(TAG, "预览尺寸不一致：已有 %dx%d，请求 %dx%d", s_w, s_h, w, h);
        return NULL;
    }
    if (!ensure_buffers(w, h))
    {
        return NULL;
    }

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_HEADER), 0);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *img = lv_img_create(box);
    lv_img_set_src(img, &s_dsc[s_front]);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_size(img, w, h);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hint = ui_label_create(box, UI_TXT_HOME_FACE_HINT, UI_FONT_TITLE, UI_COLOR_TEXT_DIM);
    lv_obj_center(hint);

    preview_widget_t *it = &s_widgets[s_widget_count++];
    it->box = box;
    it->img = img;
    it->hint = hint;

    ESP_LOGI(TAG, "预览区已创建（第 %d 个）：%dx%d，%d 块图像缓冲在 PSRAM", s_widget_count, w, h,
             UI_PREVIEW_BUFS);
    return box;
}

bool ui_preview_push_frame(const uint16_t *rgb565, int w, int h)
{
    if (rgb565 == NULL || s_buf[0] == NULL || s_widget_count == 0)
    {
        return false;
    }
    if (w != s_w || h != s_h)
    {
        return false; // 尺寸不符（摄像头配置被改过）
    }

    // 只有主页与注册页有预览区；其他页面直接丢弃，省下拷贝与重绘
    const ui_page_t page = ui_app_current_page();
    if (page != UI_PAGE_HOME && page != UI_PAGE_ENROLL)
    {
        return false;
    }

    // 上一帧还没被 ui_preview_service() 交给控件时不要覆盖后台缓冲：
    // 覆盖会让 LVGL 正在显示的画面突然变成半新半旧。
    if (s_pending_idx >= 0)
    {
        return false;
    }

    // 整帧拷贝到后台缓冲（前台缓冲仍在显示，不会看到半帧）。
    // 本函数是推帧的唯一执行者（LCD 任务单线程），s_pending_idx >= 0 期间不会
    // 有第二次推送，所以 s_back 在拷贝期间保持不变。
    const int idx = s_back;
    memcpy(s_buf[idx], rgb565, (size_t)w * (size_t)h * sizeof(lv_color_t));
    // 记录**这一帧实际写入的缓冲号**再置“有新帧”。不能只置一个 bool：
    // 显示任务与 LVGL 定时器并发执行，若定时器在写入过程中读到“有新帧”，
    // 它会自己再翻转一次索引，结果把刚写了一半的缓冲交给 lv_img，
    // 屏幕上就是花屏/闪屏（只在帧率与刷新节奏错开时出现）。
    s_pending_idx = idx;
    return true;
}

void ui_preview_service(void)
{
    const int idx = s_pending_idx;
    if (idx < 0 || s_widget_count == 0)
    {
        return;
    }

    // 当前页面没有预览区：丢弃这一帧。先清标记，否则 LCD 任务会被永久拒绝推送。
    const ui_page_t page_now = ui_app_current_page();
    if (page_now != UI_PAGE_HOME && page_now != UI_PAGE_ENROLL)
    {
        s_pending_idx = -1;
        return;
    }

    // 顺序很重要：先切换“下一帧写哪块”，再清“有新帧”标记。
    // 反过来写会留出一个窗口——LCD 任务看到标记已清、却仍读到旧的 s_back，
    // 于是把新帧 memcpy 进正在显示的那块缓冲，画面立刻半新半旧（花屏）。
    //
    // 写目标的选择还必须避开两块缓冲：
    //   s_front      刚交给 lv_img、马上要被画到面板上的；
    //   s_prev_front 面板上仍在显示的旧帧（LVGL 重画要等下一次 lv_timer_handler，
    //                最长 30 ms；这段时间 LCD 任务优先级更高，会抢占 LVGL 任务）。
    // 两块缓冲做不到这一点（写目标必然是其中之一），实测就是“画面一帧一帧
    // 平移 + 彩色花屏”；三缓冲下写目标唯一且安全。
    s_prev_front = s_front;
    s_front = idx;
    s_back = pick_back_buffer();
    s_pending_idx = -1;
    // 这样即使不加锁也不会写坏正在显示的缓冲：s_back 只在本函数里改，而本函数
    // 只在“有新帧”时运行；显示任务看到标记已清后读到的必然是刚写入的 s_back
    // （两次赋值按程序顺序提交），且它下一次 memcpy 之前一定会再检查标记。

    for (int i = 0; i < s_widget_count; i++)
    {
        lv_obj_t *img = s_widgets[i].img;
        if (img == NULL)
        {
            continue;
        }
        lv_img_set_src(img, &s_dsc[idx]);
        lv_obj_invalidate(img);
        if (!s_has_frame && s_widgets[i].hint != NULL)
        {
            lv_obj_add_flag(s_widgets[i].hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 普通 lv_obj_invalidate() 可能要等 LVGL 下一轮默认刷新周期才会绘制。
     * 预览帧已准备好，在 LVGL 任务内立即刷失效区域，避免额外等待一个周期。 */
    lv_refr_now(lv_disp_get_default());
    s_has_frame = true;
    s_push_count++;
}

uint32_t ui_preview_get_push_count(void)
{
    return s_push_count;
}

bool ui_preview_has_frame(void)
{
    return s_has_frame;
}
