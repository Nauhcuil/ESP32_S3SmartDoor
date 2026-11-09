#ifndef __LCD_H__
#define __LCD_H__

#include "driver/gpio.h"
#include "esp_lcd_types.h"

/* ==================== 屏幕方向配置 ====================
 * 通过修改 LCD_ORIENTATION 宏即可在竖屏/横屏之间切换，本工程默认横屏。
 *   - 竖屏：LCD_WIDTH=320、LCD_HEIGHT=480（MADCTL: 仅水平镜像）
 *   - 横屏：LCD_WIDTH=480、LCD_HEIGHT=320（MADCTL: 交换坐标轴 + 水平/垂直镜像）
 * 横屏由 ST7796 控制器的 MADCTL 寄存器（MV/MX/MY 位）实现，无需改动上层绘制逻辑。
 */
#define LCD_ORIENTATION_PORTRAIT   0
#define LCD_ORIENTATION_LANDSCAPE  1
#define LCD_ORIENTATION            LCD_ORIENTATION_LANDSCAPE   // 当前选择：横屏

#if (LCD_ORIENTATION == LCD_ORIENTATION_LANDSCAPE)
#define LCD_WIDTH    480   /* 横屏有效显示宽度（像素） */
#define LCD_HEIGHT   320   /* 横屏有效显示高度（像素） */
#define LCD_SWAP_XY  1     /* 交换坐标轴（ST7796 MADCTL MV 位） */
#define LCD_MIRROR_X 1     /* 水平镜像（ST7796 MADCTL MX 位） */
#define LCD_MIRROR_Y 1     /* 垂直镜像（ST7796 MADCTL MY 位） */
#else
#define LCD_WIDTH    320   /* 竖屏有效显示宽度（像素） */
#define LCD_HEIGHT   480   /* 竖屏有效显示高度（像素） */
#define LCD_SWAP_XY  0     /* 不交换坐标轴 */
#define LCD_MIRROR_X 1     /* 水平镜像（与当前板卡实际接法匹配） */
#define LCD_MIRROR_Y 0     /* 不垂直镜像 */
#endif


/* ==================== 函数声明（extern "C" 防止 C++ 名字修饰） ==================== */
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lcd_init(void);
void lcd_cs(uint32_t state);
void lcd_set_color(uint16_t color);
void lcd_draw_picture(uint32_t x_s,uint32_t y_s,uint32_t width,uint32_t height,uint16_t* pic);
esp_lcd_panel_handle_t lcd_get_panel_handle(void);

/**
 * @brief 获取 LCD 的 SPI 面板 IO 句柄（LVGL 注册显示时传给 esp_lvgl_port）
 * @return esp_lcd_panel_io_handle_t 面板 IO 句柄，需在 lcd_init() 成功之后调用
 */
esp_lcd_panel_io_handle_t lcd_get_io_handle(void);

/**
 * @brief 在持锁的调用方线程里同步面板 IO：发一条 NOP 空参数命令，轮询等待
 *        所有在飞的 DMA 事务完成后返回
 * @note  - 背景：esp_lvgl_port 把面板 IO 的 on_color_trans_done 注册成了 LVGL 的
 *          flush 完成回调，任何颜色传输完成都会让 LVGL 认为"这块缓冲已发完"。
 *          若在 LVGL 锁外另有模块直绘（摄像头预览），其传输完成回调会在 LVGL
 *          正在刷新时提前清掉 flushing，造成撕裂/闪烁。
 *        - 面板 IO 的轮询事务（tx_param）会先回收全部在飞事务，因此本函数返回时，
 *          LVGL 的 flushing 状态一定是准确的。
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示 LCD 未初始化
 */
esp_err_t lcd_sync_io(void);

/* ==================== 文本/图形绘制接口（人脸识别等模块使用） ==================== */
/**
 * @brief 在屏幕指定区域绘制图像数据（直接透传 esp_lcd_panel_draw_bitmap）
 * @param x_start    起始 X 坐标（左上角）
 * @param y_start    起始 Y 坐标（左上角）
 * @param x_end      结束 X 坐标（右下角，开区间）
 * @param y_end      结束 Y 坐标（右下角，开区间）
 * @param color_data RGB565 像素数据指针
 */
void lcd_draw_bitmap(uint32_t x_start, uint32_t y_start, uint32_t x_end, uint32_t y_end, const void *color_data);

/**
 * @brief 在 LCD 指定区域填充纯色（用于清除信息显示区域）
 * @param x     起始 X 坐标
 * @param y     起始 Y 坐标
 * @param w     区域宽度（像素）
 * @param h     区域高度（像素）
 * @param color 填充颜色（RGB565 格式）
 */
void lcd_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color);

/**
 * @brief 在 LCD 上显示字符串（支持中文，使用 font 目录点阵字库，font_size 仅支持 16/32）
 * @param x         起始 X 坐标
 * @param y         起始 Y 坐标
 * @param fc        前景色（RGB565）
 * @param bc        背景色（RGB565）
 * @param font_size 字号（16 或 32）
 * @param str       要显示的字符串（UTF-8 编码）
 */
void lcd_display_string(uint32_t x, uint32_t y, uint32_t fc, uint32_t bc, uint32_t font_size, char *str);

/**
 * @brief 格式化显示字符串（printf 风格格式化后调用 lcd_display_string）
 */
void lcd_display_string_fmt(uint32_t x, uint32_t y, uint32_t fc, uint32_t bc, uint32_t font_size, char *str, ...);

#ifdef __cplusplus
}
#endif

#endif
