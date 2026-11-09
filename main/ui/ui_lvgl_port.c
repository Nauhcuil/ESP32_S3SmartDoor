/**
 * @file    ui_lvgl_port.c
 * @brief   LVGL 对接层实现：初始化 esp_lvgl_port，注册 ST7796 显示与 FT6X36 触摸。
 *
 * @note    - 显示缓冲必须留在内部 DMA RAM：实测放 PSRAM（buff_spiram=true）时，SPI 从
 *            PSRAM 取数会与摄像头 DMA 抢带宽，偶发 "DMA TX underflow"，随后
 *            lcd_panel.io.spi 回收事务失败、本次 flush 的 on_color_trans_done 不再回调，
 *            LVGL 一直停在“等待 flush 完成”，LVGL 任务空转把 IDLE0 饿死（task_wdt），
 *            现场表现就是“切换页面后预览冻结、看门狗复位”。所以 M4 改为
 *            **行数 20 → UI_LVGL_DRAW_LINES(6)** 来省内部 RAM：
 *            单块 480x6x2 = 5.76 KB，双缓冲 11.5 KB（原本 20 行是 38.4 KB）。
 *          - 同时 LVGL 的内存池（48 KB）通过 CMake 注入 LV_MEM_POOL_ALLOC 放到 PSRAM
 *            （见 main/CMakeLists.txt 与 ui/lv_mem_psram.c），这是 M4 内部 RAM 预算的
 *            主要来源：face_task 的 16 KB 连续栈与 esp_wifi_init 都要内部 RAM。
 *          - 屏幕方向由 lcd_init()（MADCTL 交换坐标轴 + 镜像）和 tp.c（触摸坐标变换）
 *            统一处理，所以 LVGL 的 rotation 全部为 false，否则会二次旋转。
 *          - LVGL 任务放在 Core0、优先级 1：Core1 留给摄像头与 SPI 中断；调度器每隔
 *            1~30 ms 就让 LVGL 休眠一次，IDLE0 照样能被喂狗。优先级 0 时预览只有 ~6 fps；
 *            会饿死 IDLE0 的是 face_task 的逐帧推理，已在 face_task 里做检测节流。
 */

#include "ui_lvgl_port.h"

#include <stdio.h>
#include <string.h>

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lcd.h"
#include "tp.h"
#include "ui_preview.h"
#include "ui_theme.h"

static const char *TAG = "ui_lvgl_port"; // 日志标签

/**
 * @brief LVGL 绘制缓冲行数（双缓冲，每块 480 x 行数 x 2 字节）
 * @note  M1~M3 为 20 行（单块 19.2 KB，双缓冲 38.4 KB）。M4 需要给 WiFi 与 face_task
 *        腾内部 RAM，降到 10 行（单块 9.6 KB，双缓冲 19.2 KB）。代价是整屏刷新从
 *        16 次 DMA 变成 32 次，每次多一份命令开销，实测仍在可接受范围。
 *        M4 第二轮再降到 6 行（单块 5.76 KB，双缓冲 11.5 KB）：esp_wifi_init 之后内部 RAM
 *        只剩约 7.5 KB 时，软 AP 启动阶段的 beacon eb 走 wifi_zalloc——先向 PSRAM 申请，
 *        PSRAM 拿不到才回落到内部 RAM；回落时的连续块太小就返回 NULL，驱动拿到 NULL
 *        仍继续使用，直接空指针解引用（LoadProhibited），上电即重启。
 *        M4 第三轮降到 4 行（单块 3.84 KB，双缓冲 7.7 KB）：云端/MQTT 起来后内部 RAM
 *        只剩 3.6 KB，配网门户处理手机请求还要分配会话缓冲；4 行仍能整屏分 80 段刷完，
 *        单段 3.8 KB 的 SPI 事务在 80 MHz 下约 0.4 ms，肉眼无差。
 */
#define UI_LVGL_DRAW_LINES 4

/** @brief LVGL 显示对象（注册成功后非 NULL） */
static lv_disp_t *s_disp = NULL;
/** @brief LVGL 触摸输入设备 */
static lv_indev_t *s_indev = NULL;
/** @brief 对接层初始化成功标志 */
static bool s_ready = false;

/**
 * @brief 面板显存快照开关：置 1 时每帧在锁内把整屏拷到 PSRAM，供事后比对
 * @note  用于离线定位花屏（CPU 侧无法读回显存时，用视频/截图替代）：
 *        每帧刷新都留下“设计上应该显示的内容”，便于与屏幕实拍逐像素比对，
 *        判断错乱来自绘制（CPU 侧）还是传输（DMA/SPI）。正式固件保持 0。
 */
#ifndef UI_PANEL_SNAPSHOT
#define UI_PANEL_SNAPSHOT 0
#endif

/**
 * @brief 屏幕截图调试开关：置 1 时把目标页面（默认主页）渲染成 JPEG 写进 SPIFFS
 * @note  用于离线核对“屏幕实际显示的内容”（花屏/错位/颜色/中文缺字/控件重叠）。
 *        做法：**必须在 LVGL 任务里**（见 screenshot_timer_cb）用 lv_snapshot 把页面渲染到
 *        一张新的“假显示”上（与 LVGL 正常刷新同一套绘制路径，绕开面板与 DMA），
 *        编码为 JPEG 写入 /spiffs/snap/ui_shot_<页面号>.jpg，并打印抽样像素。
 *        要截其它页面时编译期加 -DUI_SHOT_PAGE=<ui_page_t 值>（如删除人脸页 = 5），
 *        定时器会在启动后自动切到该页面。
 *        注意：不要在定时器回调里直接改 disp 的 draw_buf/clear_cb 或调 lv_obj_redraw，
 *        lv_obj_redraw 会直接调用 disp->driver->clear_cb，而 esp_lvgl_port 的驱动里
 *        没有这个回调（NULL）→ 跳到地址 0，表现为 InstrFetchProhibited 复位。
 *        正式固件保持 0。
 */
#ifndef UI_SCREENSHOT_DEBUG
#define UI_SCREENSHOT_DEBUG 0
#endif

/** @brief 截图目标页面（默认主页；联调其它页面时由 -DUI_SHOT_PAGE 覆盖） */
#ifndef UI_SHOT_PAGE
#define UI_SHOT_PAGE 0
#endif

#if UI_SCREENSHOT_DEBUG
#include "esp_timer.h"
#include "img_converters.h" // 只用到 fmt2jpg：RGB565 → JPEG
// 文件读写用标准 stdio（VFS 已挂载 SPIFFS），不要包含 fs.h——它会解析到 lwip 的同名头文件
#include "ui_app.h"

/** @brief 首次截图时刻（毫秒），-1 表示尚未开始等待 */
static int64_t s_shot_start_ms = -1;
/** @brief 已完成的截图次数 */
static int s_shot_count = 0;
/** @brief 截图定时器是否已创建（只在持 LVGL 锁时置位/读取） */
static bool s_shot_started = false;
/** @brief 计划截图次数与间隔（调试用：第一次在主页显示 15 s 后，之后每 60 s 一张） */
#define UI_SHOT_PLAN    2
#define UI_SHOT_FIRST_MS 15000
#define UI_SHOT_NEXT_MS  60000

/**
 * @brief 截图目标页面（与 ui_page_t 取值一致）
 * @note  截图在进入目标页 15 s 后进行；文件名用 ui_shot_<页面号>.jpg，
 *        便于多次改页截图而不互相覆盖。
 */
#define UI_SHOT_TARGET_PAGE ((ui_page_t)UI_SHOT_PAGE)

/**
 * @brief 把当前屏幕（UI_SHOT_TARGET_PAGE）渲染成一张 JPEG 写到 SPIFFS，并打印抽样像素
 * @return true 写入成功
 * @note  必须在 LVGL 任务里调用（定时器回调满足此条件），不需要也不应再额外加
 *        ui_lvgl_lock——LVGL 锁是递归锁，在任务内部再加一次虽可，但会掩盖死锁风险。
 *        lv_snapshot_take 用自己的“假显示”和绘制上下文渲染，完全不碰真实 disp 的
 *        draw_buf / clear_cb，因此不影响正在进行的刷新，也不会像 lv_obj_redraw
 *        那样冲到 NULL 的 clear_cb 上。
 */
static bool capture_home_jpeg(void)
{
    // 顶层图层覆盖全部屏幕（状态栏 + 开门覆盖层），且不随页面切换变化，
    // 但页面内容在活动屏幕上，所以实际截图对象是 lv_scr_act()。
    lv_obj_t *top = lv_scr_act();
    const int w = UI_SCREEN_W;
    const int h = UI_SCREEN_H;

    const uint32_t need = lv_snapshot_buf_size_needed(top, LV_IMG_CF_TRUE_COLOR);
    // 整屏图像放 PSRAM：内部 RAM 只剩约 30 KB 连续块，一次性要 307 KB，放内部一定失败。
    // 渲染由 CPU 完成，不经过 DMA，放在 PSRAM 不会引起 SPI/DMA 问题。
    const size_t img_bytes = need ? need : (size_t)w * h * 2;
    uint8_t *img =
        (uint8_t *)heap_caps_malloc(img_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (img == NULL)
    {
        ESP_LOGE(TAG, "截图缓冲分配失败（需要 %u 字节 PSRAM）", (unsigned)img_bytes);
        return false;
    }

    // lv_snapshot_take_to_buf 要求缓冲不小于 lv_snapshot_buf_size_needed()，
    // 它会把整个顶层图层（含扩展绘制区）渲染进缓冲。
    lv_img_dsc_t dsc;
    if (lv_snapshot_take_to_buf(top, LV_IMG_CF_TRUE_COLOR, &dsc, img, (uint32_t)img_bytes) !=
        LV_RES_OK)
    {
        ESP_LOGE(TAG, "截图渲染失败（缓冲 %u 字节）", (unsigned)img_bytes);
        heap_caps_free(img);
        return false;
    }
    if (dsc.header.w != (uint32_t)w || dsc.header.h != (uint32_t)h)
    {
        ESP_LOGW(TAG, "截图尺寸 %ux%u 与屏幕 %dx%d 不一致（可能含扩展绘制区）", dsc.header.w,
                 dsc.header.h, w, h);
    }

    // ---- 抽样像素（原始内存字节序）：每 40 行打一行，每行 11 个点 ----
    // 每行 11 个点：ESP_LOGI 单条日志有长度上限，40 个点会被截断。
    // LV_COLOR_16_SWAP 下 lv_color_t 的内存布局是 [RRRRR GGG][GGG BBBBB]，
    // 与 img_converters 的 RGB565 解析（高字节在前）一致，不需要交换字节。
    ESP_LOGI(TAG, "截图：屏幕 %dx%d，抽样像素（原始字节，间隔 40 px）", w, h);
    for (int y = 10; y < h; y += 40)
    {
        char line[48];
        int n = 0;
        for (int x = 10; x < w && n < 36; x += 40)
        {
            const size_t idx = (size_t)y * dsc.header.w + x;
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x%02x ", img[idx * 2],
                          img[idx * 2 + 1]);
        }
        ESP_LOGI(TAG, "  y=%3d |%s|", y, line);
    }

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    bool ok = false;
    if (fmt2jpg(img, (size_t)dsc.header.w * dsc.header.h * 2, (uint16_t)dsc.header.w,
                (uint16_t)dsc.header.h, PIXFORMAT_RGB565, 80, &jpg, &jpg_len) &&
        jpg != NULL)
    {
        char path[48];
        snprintf(path, sizeof(path), "/spiffs/snap/ui_shot_%d.jpg", (int)UI_SHOT_TARGET_PAGE);
        FILE *f = fopen(path, "wb");
        if (f != NULL)
        {
            ok = (fwrite(jpg, 1, jpg_len, f) == jpg_len);
            fclose(f);
            ESP_LOGI(TAG, "截图已写入 %s（%u 字节）", path, (unsigned)jpg_len);
        }
        else
        {
            ESP_LOGE(TAG, "主页截图写文件失败（SPIFFS 是否已挂载？）");
        }
        free(jpg);
    }
    else
    {
        ESP_LOGE(TAG, "主页截图 JPEG 编码失败");
    }

    heap_caps_free(img);
    return ok;
}

/**
 * @brief 截图调试定时器：主页出现 15 s 后截一张，之后每 60 s 一张，共 UI_SHOT_PLAN 张
 * @note  只在主页可见时执行；失败 1 s 后重试，最多重试 3 次。
 *        截图本身要渲染整屏 + JPEG 编码，耗时约数百毫秒，期间界面会短暂停顿，属调试行为。
 */
static void screenshot_timer_cb(lv_timer_t *timer)
{
    static int attempts = 0;

    if (s_shot_count >= UI_SHOT_PLAN || attempts >= 3)
    {
        lv_timer_del(timer);
        return;
    }
    if (ui_app_current_page() != UI_SHOT_TARGET_PAGE)
    {
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_shot_start_ms < 0)
    {
        s_shot_start_ms = now_ms;
    }
    const int64_t due_ms = s_shot_count == 0 ? UI_SHOT_FIRST_MS : UI_SHOT_NEXT_MS;
    if (now_ms - s_shot_start_ms < due_ms)
    {
        return;
    }

    attempts++;
    if (capture_home_jpeg())
    {
        s_shot_count++;
        attempts = 0;
        s_shot_start_ms = now_ms;
        ESP_LOGI(TAG, "截图完成（第 %d 张）", s_shot_count);
    }
    else
    {
        ESP_LOGW(TAG, "截图失败（第 %d 次），%s", attempts, attempts >= 3 ? "放弃" : "1 s 后重试");
        s_shot_start_ms = now_ms - due_ms + 1000; // 1 s 后重试
    }
}
#endif

bool ui_lvgl_is_ready(void)
{
    return s_ready;
}

lv_disp_t *ui_lvgl_get_disp(void)
{
    return s_disp;
}

bool ui_lvgl_lock(uint32_t timeout_ms)
{
    if (!s_ready)
    {
        return false;
    }
    return lvgl_port_lock(timeout_ms);
}

void ui_lvgl_unlock(void)
{
    if (s_ready)
    {
        lvgl_port_unlock();
    }
}

/**
 * @brief 预览服务定时器回调：把 LCD 任务写入后台缓冲的新帧交给图像控件
 * @note  在 LVGL 任务内执行，不需要（也不应）额外加锁；见 ui_preview.c 的说明。
 */
static void preview_service_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_preview_service();
}

esp_err_t ui_lvgl_init(void)
{
    if (s_ready)
    {
        return ESP_OK; // 幂等
    }

    esp_lcd_panel_io_handle_t io_handle = lcd_get_io_handle();
    esp_lcd_panel_handle_t panel_handle = lcd_get_panel_handle();
    if (io_handle == NULL || panel_handle == NULL)
    {
        ESP_LOGE(TAG, "LCD 未初始化（io=%p panel=%p）", io_handle, panel_handle);
        return ESP_ERR_INVALID_STATE;
    }

    // ---- 1. 初始化 esp_lvgl_port 的任务与定时器 ----
    // 任务固定在 Core0、优先级 1：
    //   - 不放到 Core1：Core1 上摄像头 DMA 与 SPI 中断都在忙，LVGL 会与它们抢 CPU。
    //   - 优先级 1 高于 IDLE0，但 esp_lvgl_port 的任务循环每次处理完定时器都会
    //     vTaskDelay(1~30 ms)（见 lvgl_port_task），IDLE0 在每个空隙都能运行并喂看门狗。
    //     设为 0 时 LVGL 与 IDLE 同级轮转，只能分到约一半 CPU：实测预览掉到 6 fps 左右、
    //     触摸响应也发涩；提到 1 后 LVGL 在 face_task 阻塞的间隙能拿满 CPU。
    //   - 真正会饿死 IDLE 的是 face_task 逐帧推理（见 face_task.cpp 的检测节流）。
    //   - task_max_sleep_ms 设为 30 ms：一次 lv_timer_handler 只刷新一帧就休眠。
    // 栈 6 KB：面板刷新路径中存在较大局部变量；实测运行期剩余 5 KB（8 KB 时），
    // M4 内部 RAM 紧张收到 6 KB，仍需保留 ≥2 KB 余量。
    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 1,
        .task_stack = 6 * 1024,
        .task_affinity = 0,
        .task_max_sleep_ms = 30, // = CONFIG_LV_DISP_DEF_REFR_PERIOD，每次只刷一帧
        .timer_period_ms = 5,
    };
    esp_err_t ret = lvgl_port_init(&port_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "lvgl_port_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // ---- 2. 注册显示 ----
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = UI_SCREEN_W * UI_LVGL_DRAW_LINES, // 行数见 UI_LVGL_DRAW_LINES
        .double_buffer = true,
        .hres = UI_SCREEN_W,
        .vres = UI_SCREEN_H,
        .monochrome = false,
        .rotation =
            {
                // 面板方向已在 lcd_init/tp.c 中处理，此处不得再旋转
                .swap_xy = false,
                .mirror_x = false,
                .mirror_y = false,
            },
        .flags =
            {
                // 留在内部 DMA RAM（原因见文件头注释：放 PSRAM 会 DMA underflow 后
                // flush 永不回调，LVGL 卡死并触发 task_wdt）。内部 RAM 的节省改由
                // UI_LVGL_DRAW_LINES(4) 与 LVGL 内存池放 PSRAM 实现。
                .buff_dma = true,
                .buff_spiram = false,
            },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL)
    {
        ESP_LOGE(TAG, "lvgl_port_add_disp 失败（PSRAM 不足？）");
        return ESP_ERR_NO_MEM;
    }

    // ---- 3. 注册触摸 ----
    esp_lcd_touch_handle_t tp = tp_get_handle();
    if (tp != NULL)
    {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_disp,
            .handle = tp,
        };
        s_indev = lvgl_port_add_touch(&touch_cfg);
        if (s_indev == NULL)
        {
            // 触摸失败不阻断启动：按键仍可操作
            ESP_LOGW(TAG, "lvgl_port_add_touch 失败，界面将只能用按键操作");
        }
        else
        {
            ESP_LOGI(TAG, "触摸已接入 LVGL");
        }
    }
    else
    {
        ESP_LOGW(TAG, "触摸未初始化，界面将只能用按键操作");
    }

    s_ready = true;
    ESP_LOGI(TAG, "LVGL 初始化完成（%dx%d，双缓冲 %d 行，绘制缓冲在内部 RAM）", UI_SCREEN_W,
             UI_SCREEN_H, UI_LVGL_DRAW_LINES);

    // ---- 4. 预览服务定时器：在 LVGL 任务内把新帧交给预览控件 ----
    // 必须在锁内创建：定时器链表由 LVGL 任务遍历，锁外插入会被并发读到半完成状态。
    if (ui_lvgl_lock(50))
    {
        lv_timer_create(preview_service_timer_cb, UI_PREVIEW_SERVICE_MS, NULL);
        ui_lvgl_unlock();
    }
    else
    {
        ESP_LOGE(TAG, "创建预览服务定时器失败（加锁超时），摄像头预览将不刷新");
    }

#if UI_SCREENSHOT_DEBUG
    // LVGL 定时器只能在持 LVGL 锁时创建：定时器链表会被 LVGL 任务并发遍历，
    // 在启动任务里直接 lv_timer_create() 出现过 lv_timer_exec 中跳转空地址
    // （InstrFetchProhibited）复位。这里在锁内创建即可。
    if (!s_shot_started && ui_lvgl_lock(50))
    {
        lv_timer_create(screenshot_timer_cb, 500, NULL);
        ui_app_switch_page(UI_SHOT_TARGET_PAGE); // 目标不是主页时先切过去
        s_shot_started = true;
        ui_lvgl_unlock();
    }
#endif

    return ESP_OK;
}
