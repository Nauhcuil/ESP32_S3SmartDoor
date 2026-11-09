/**
 * @file    tp.c
 * @brief   触摸屏驱动封装：负责 FT6x36 触摸屏的初始化（创建 I2C 面板 IO 与触摸设备实例），
 *          并提供获取触摸设备句柄的接口，供上层读取触摸坐标使用。
 *
 * @note    使用说明：
 *          - 依赖组件：esp_lcd_touch、esp_lcd_touch_ft6x36（managed_components 自动拉取）
 *          - 前置条件：必须先调用 i2c_init() 完成 I2C 总线初始化，再调用 tp_init()
 *          - 屏幕分辨率与方向由 lcd.h 的 LCD_ORIENTATION 宏决定（横屏 480x320 /
 *            竖屏 320x480）。**触摸的 x_max/y_max 必须填控制器原始范围（竖装 320x480），
 *            方向标志也要在原始坐标系下表达**，详见 tp_init() 中的说明；写错会出现
 *            “屏幕点不中 / 按钮无反应”。
 *          - 触摸设备地址 0x38（FT6x36），通过宏 ESP_LCD_TOUCH_IO_I2C_FT6x36_CONFIG()
 * 快速获取默认配置
 *          - 未使用硬件复位/中断引脚（rst_gpio_num=-1, int_gpio_num=-1），采用轮询方式读取
 *          - 调用流程：tp_init() -> tp_get_handle() -> esp_lcd_touch_read_data() /
 * esp_lcd_touch_get_coordinates()
 */

#include "tp.h"                // 自定义触摸屏头文件，提供 tp_init、tp_get_handle 函数声明
#include "driver/gpio.h"       // GPIO 驱动头文件，提供 gpio 的函数接口
#include "freertos/FreeRTOS.h" // FreeRTOS 内核头文件，提供任务、延时等接口
#include "freertos/task.h"     // FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数
#include "i2c.h"               // 自定义 I2C 头文件，提供 i2c_init 函数声明
#include "lcd.h"               // 自定义 LCD 头文件，提供 LCD_WIDTH/LCD_HEIGHT 及方向宏
#include <stdio.h>             // 标准输入输出头文件，提供 printf 等函数

/**
 * @brief 触摸诊断开关：置 1 时按 200 ms 限速打印控制器原始坐标（方向标志校准用）
 * @note  已用日志校准出横屏映射（见 tp_init() 中的 flags 说明），默认关闭。
 *        再次出现“触摸偏移/按钮点不中”时置 1 重新采集即可。
 */
#define TP_LOG_RAW_COORDS 0

#if TP_LOG_RAW_COORDS
#include "esp_log.h"   // 日志接口（仅诊断模式需要）
#include "esp_timer.h" // 毫秒级时间戳（原始坐标日志限速）

static const char *TP_TAG = "tp"; // 日志标签

/**
 * @brief 坐标处理回调（在镜像/交换之前被调用），打印控制器给出的原始坐标
 * @note  由 esp_lcd_touch_get_coordinates() 在 LVGL 任务上下文调用；日志限速 200 ms，
 *        避免按住不放时刷屏。
 */
static void tp_log_raw_cb(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                          uint8_t *point_num, uint8_t max_point_num)
{
    (void)strength;
    (void)max_point_num;

    const int x_max = (int)tp->config.x_max;
    const int y_max = (int)tp->config.y_max;

    static int64_t last_us = 0;
    const int64_t now = esp_timer_get_time();
    if (now - last_us < 200 * 1000)
    {
        return;
    }
    last_us = now;

    if (*point_num > 0)
    {
        // 候选 A（当前配置 mirror_y + swap）：sx = y_max - y，sy = x
        // 候选 B（mirror_x + swap）：sx = x_max - x，sy = y
        const int sx_a = (int)y_max - (int)y[0];
        const int sy_a = (int)x[0];
        const int sx_b = (int)x_max - (int)x[0];
        const int sy_b = (int)y[0];
        ESP_LOGI(TP_TAG, "原始触摸 x=%u y=%u → A(%d,%d) B(%d,%d)", (unsigned)x[0], (unsigned)y[0],
                 sx_a, sy_a, sx_b, sy_b);
    }
}
#endif

// 全局变量定义，用于存储各种硬件设备的句柄
static esp_lcd_touch_handle_t g_ft6x36_handle =
    NULL; // FT6x36触摸屏设备句柄（静态全局，仅本文件内部访问）

/**
 * @brief Touchscreen 初始化函数：初始化触摸屏设备
 */
esp_err_t tp_init(void) // 触摸屏初始化函数：返回 ESP_OK 表示成功
{
    esp_lcd_panel_io_handle_t io_handle =
        NULL; // 定义 I2C 面板 IO 句柄并初始化为空，用于管理 I2C 通信

    // 初始化触摸屏设备，核心要得到触摸屏的设备地址
    esp_lcd_panel_io_i2c_config_t io_config =
        ESP_LCD_TOUCH_IO_I2C_FT6x36_CONFIG(); // 使用 FT6x36 默认宏配置 I2C 面板 IO（设备地址
                                              // 0x38、8位寄存器寻址等）
    io_config.scl_speed_hz =
        I2C_FREQ_HZ; // 宏未配置该字段（默认 0），新版驱动要求 scl_speed_hz>0，
                     // 必须显式指定 SCL 频率（100kHz），否则报 invalid scl frequency

    // 初始化触摸屏设备
    // 注意（曾导致“按钮点了没反应”）：
    //   esp_lcd_touch_get_coordinates() 的软件坐标变换顺序是 先镜像、后交换轴，且镜像用
    //   `x_max - x`，因此 x_max/y_max 必须填**触摸控制器自身的原始坐标范围**（裸屏竖装
    //   320x480），而不是屏幕显示尺寸；交换轴放在最后，交换后 X 用 y_max 截断、Y 用 x_max 截断。
    //   若把 x_max/y_max 写成横屏的 480/320，镜像后再交换，Y 会算出 480-x_raw（常大于 320），
    //   坐标落到屏幕外，LVGL 就收不到任何点击。
    const int raw_w = (LCD_SWAP_XY ? LCD_HEIGHT : LCD_WIDTH);  // 控制器原始宽度（交换前）
    const int raw_h = (LCD_SWAP_XY ? LCD_WIDTH : LCD_HEIGHT);  // 控制器原始高度（交换前）
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = raw_w,
        .y_max = raw_h,
#if TP_LOG_RAW_COORDS
        .process_coordinates = tp_log_raw_cb, // 临时诊断：打印原始坐标
#endif
        .rst_gpio_num = -1, // 不使用复位引脚（-1 表示未连接），依赖上电默认状态
        .int_gpio_num = -1, // 不使用中断引脚（-1 表示未连接），采用轮询方式读取
        .levels =           // 配置复位与中断引脚的有效电平
        {
            .reset = 0,     // 复位引脚低电平有效（未接线，仅作默认配置）
            .interrupt = 0, // 中断引脚低电平有效（未接线，仅作默认配置）
        },
        .flags = // 配置触摸坐标的坐标变换标志位（在控制器原始坐标系下表达，见上方说明）
        {
            // 横屏安装时，触摸控制器原始坐标系相当于把屏幕顺时针转 90°：
            //   屏幕 X = (原始高度 - 原始 Y)，屏幕 Y = 原始 X
            // 由于 esp_lcd_touch 先做镜像再做交换，这正好等于
            //   mirror_y=1（原始 Y 用 y_max 镜像） + swap_xy=1
            // （实测：点右侧四个按钮时原始坐标为 x≈170..320、y≈35..140；
            //   若改用 mirror_x，坐标会整体偏移到预览区/标题栏，按钮永远点不中。）
            .swap_xy = LCD_SWAP_XY,
            .mirror_x = (LCD_SWAP_XY ? 0 : LCD_MIRROR_X),
            .mirror_y = (LCD_SWAP_XY ? 1 : 0),
        },
    };

    esp_err_t ret = ESP_OK; // 定义错误码变量并初始化为成功，用于保存各步骤返回值

#if (I2C_VERSION == I2C_VERSION_1)
    ret = esp_lcd_new_panel_io_i2c(I2C_NUM, &io_config, &io_handle);
#else
    ret = esp_lcd_new_panel_io_i2c_v2(i2c_get_bus_handle(), &io_config, &io_handle);
#endif
    if (ret != ESP_OK) // 判断面板 IO 创建是否成功
    {
        printf("esp_lcd_new_panel_io_i2c failed, ret = %d\n",
               ret); // 创建失败时打印错误信息及错误码
        return ret;  // 失败则返回错误码，终止初始化
    }

    ret = esp_lcd_touch_new_i2c_ft6x36(
        io_handle,
        &tp_cfg,
        &g_ft6x36_handle); // 基于面板 IO 创建 FT6x36 触摸设备实例，保存句柄到全局变量
    if (ret != ESP_OK)     // 判断触摸设备创建是否成功
    {
        printf("esp_lcd_touch_new_i2c_ft6x36 failed, ret = %d\n",
               ret); // 创建失败时打印错误信息及错误码
        return ret;  // 失败则返回错误码，终止初始化
    }
    return ESP_OK; // 全部初始化成功，返回 ESP_OK
}

esp_lcd_touch_handle_t tp_get_handle(void) // 获取触摸屏设备句柄的接口函数
{
    return g_ft6x36_handle; // 返回全局触摸设备句柄，供上层读取触摸数据使用
}
