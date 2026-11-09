/**
 * @file    main.c
 * @brief   主程序入口：初始化 NVS、I2C、PCA9557、全局事件、触摸屏、LCD 与 DVP
 *          摄像头（GC0308），启动系统监控与人脸识别应用（检测/注册/识别/清空）。
 *
 * @note    使用说明：
 *          - 初始化顺序：nvs_flash_init -> i2c_init -> pca9557_init
 *            -> app_events_init -> audio_service_init -> clock_service_init
 *            -> tp_init -> lcd_init -> camera_init
 *            -> ui_lvgl_init/ui_app_start -> sys_monitor_start -> storage/日志/抓拍/门禁
 *            -> app_human_face_recognition -> net_service_init -> voice_assistant_init -> voice_prompt_init
 *            （硬件侧有先后依赖，不可颠倒；网络放最后是因为 WiFi 会占用大块连续内部 RAM，
 *            必须让 face_task 先拿到栈；语音任务栈 8 KB 也必须在 WiFi 之后分配）
 *          - tp_init 失败只打印告警（触摸为可选外设），不影响人脸识别；
 *            其余步骤失败均直接退出
 *          - 摄像头电源（DVP_PWDN）由 PCA9557 扩展 IO 第 2 位控制，
 *            camera_init() 内部会拉低上电
 *          - SCCB 控制线复用 I2C0（SIOD=GPIO1 / SIOC=GPIO2），须先 i2c_init()
 *          - 屏幕方向由 lcd.h 的 LCD_ORIENTATION 宏决定（横屏 480x320 / 竖屏
 *            320x480），摄像头按 QVGA(320x240) 采集，由 AI 模块整帧显示
 *          - 人脸识别操作（BOOT 按键，GPIO0）：
 *              - 长按(>1秒)：注册当前人脸到 Flash
 *              - 单击：识别人脸
 *              - 双击：清空所有人脸数据
 *          - 人脸数据保存在 Flash 分区表 "storage" 分区（见 partitions.csv）
 *          - I2C 必须使用新版驱动（i2c.h 中 I2C_VERSION_2，配套 IDF 5.5）
 */

#include "app_events.h"        // 全局事件组/互斥量，提供 app_events_init 函数声明
#include "audio_service.h"     // M5 音频服务：I2S 全双工 + ES8311 播放 + ES7210 录音
#include "esp_heap_caps.h"     // 内部 RAM 水位打印（M4 内存预算排查用）
#include "esp_log.h"           // 启动阶段日志
#include "access_log.h"        // 门禁事件日志（M3）：JSONL 追加写 /spiffs/log
#include "camera.h"            // 自定义摄像头头文件，提供 camera_init、camera_lcd_start 函数声明
#include "clock_service.h"     // 系统时间来源：无 SNTP 时用编译时刻兜底（主页时钟）
#include "door_controller.h"   // 门禁控制（M3）：模拟开门 + 事件日志分发
#include "freertos/FreeRTOS.h" // FreeRTOS 内核头文件，提供任务、延时等接口
#include "freertos/task.h"     // FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数
#include "i2c.h"               // 自定义 I2C 头文件，提供 i2c_init 函数声明
#include "lcd.h"               // 自定义 LCD 头文件，提供 lcd_init 函数声明
#include "net_service.h"       // M4 网络门面：WiFi STA/SoftAP 配网 + SNTP 对时
#include "nvs_flash.h"         // NVS 存储头文件，提供 nvs_flash_init 函数声明（WiFi/业务参数依赖）
#include "pca9557.h"           // 自定义 PCA9557 头文件，提供 pca9557_init 函数声明
#include "snapshot.h"          // 陌生人抓拍（M3）：写盘任务 + 环形清理
#include "storage.h"           // SPIFFS 挂载（M3）：日志/抓拍文件系统
#include "sys_monitor.h"       // 系统监控模块，提供 sys_monitor_start 函数声明
#include "tp.h"                // 自定义触摸屏头文件，提供 tp_init 函数声明（LVGL 触摸输入依赖）
#include "ui_app.h"            // LVGL 界面管理：创建页面、状态栏与开门覆盖层
#include "ui_lvgl_port.h"      // LVGL 对接层：注册 ST7796 显示与 FT6X36 触摸
#include "voice_assistant.h" // M5 离线语音：esp-sr 唤醒词 + MultiNet 中文命令词
#include "voice_prompt.h"     // M5 语音播报：内嵌离线语音素材 + 播放线程
#include "who_human_face_recognition.hpp" // 人脸识别应用头文件，提供 app_human_face_recognition 函数声明
#include "wifi_manager.h"     // 暂停/恢复 WiFi 主动连接（语音初始化期间避免内存峰值冲突）
#include <stdio.h>             // 标准输入输出头文件，提供 printf 等函数

/**
 * @brief 启动阶段打印内部 RAM 水位（M4 内存预算排查）
 * @param stage 阶段名（日志标签 "boot_mem"）
 * @note  内部 RAM 是 S3 上的稀缺资源：face_task 要 16 KB **连续**栈，WiFi 驱动初始化
 *        要多次申请内部块，LVGL 的绘制缓冲也必须在内。任何一步“先占大块”都会让后面的
 *        申请失败，因此每个初始化阶段都留一行水位，出问题时一眼看出是谁吃掉的。
 *        M5 调整内存布局后（摄像头 DMA 中转降到 16 KB、WiFi 静态 TX 降为 2 项），
 *        网络初始化后内部空闲约 13 KB；默认关闭，排查时置 1。
 */
#define BOOT_MEM_LOG 0
static void boot_mem_log(const char *stage)
{
#if BOOT_MEM_LOG
    ESP_LOGI("boot_mem", "%-16s 内部空闲=%u 最大块=%u | PSRAM 空闲=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    (void)stage;
#endif
}

/**
 * @brief 应用主函数：程序入口，初始化硬件并启动人脸识别应用
 */
void app_main(void)
{
    boot_mem_log("app_main");
    // 初始化 NVS：WiFi 配置、识别阈值、活体开关、管理员 PIN 均保存在 NVS 中。
    // 首次启动或版本升级时可能出现“无空闲页/版本不匹配”，此时擦除后重新初始化。
    esp_err_t ret = nvs_flash_init(); // 初始化默认 NVS 分区
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase()); // 擦除 NVS 分区后重试
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK)
    {
        printf("nvs_flash_init failed, ret = %d\n", ret); // NVS 是后续配置的基础，失败则不继续
        return;
    }

    // 初始化 I2C
    ret = i2c_init(); // 调用自定义 i2c_init 初始化 I2C 总线（新版驱动），返回错误码
    if (ret != ESP_OK)          // 判断 I2C 初始化是否成功
    {
        printf("i2c_init failed, ret = %d\n", ret); // 初始化失败时打印错误信息及错误码
        return;                                     // 失败则直接退出，不继续执行后续初始化
    }

    // 初始化 PCA9557 IO 扩展芯片
    ret = pca9557_init(); // 初始化 PCA9557（LCD_CS / DVP_PWDN 等控制 IO），返回错误码
    if (ret != ESP_OK)    // 判断 PCA9557 初始化是否成功
    {
        printf("pca9557_init failed, ret = %d\n", ret); // 初始化失败时打印错误信息及错误码
        return;                                         // 失败则直接退出
    }

    // 创建全局事件组与互斥量（视觉/UI/网络/音频任务间的状态同步与资源保护）
    if (app_events_init() != ESP_OK)
    {
        printf("app_events_init failed\n"); // 内存不足，无法建立任务间同步机制
        return;
    }

    // M5：音频子系统（I2S 全双工 48 kHz + ES8311 播放 + ES7210 录音）。
    // 必须在 i2c/pca9557 之后（编解码走 I2C、PA_EN 在 PCA9557 IO1），且在 face_task 之前
    // 抢到 I2S DMA 缓冲。失败只告警：识别/界面照常，只是没有声音
    if (audio_service_init() != ESP_OK)
    {
        printf("audio_service_init failed (音频功能不可用)\n");
    }

    // 初始化系统时间：网络对时之前用固件编译时刻兜底，让主页时钟能正常走；
    // M4 的 SNTP 同步成功后 time() 变成真实时间，主页时钟自动显示到秒
    clock_service_init();

    // 初始化触摸屏（FT6X36 @0x38，复用 I2C0）：供后续 LVGL 触摸输入使用。
    // 触摸失败不影响人脸识别主流程，仅打印告警后继续
    ret = tp_init();
    if (ret != ESP_OK)
    {
        printf("tp_init failed, ret = %d (触摸功能不可用)\n", ret);
    }

    boot_mem_log("i2c/pca/tp");

    // 初始化 LCD 显示屏
    ret = lcd_init();  // 初始化 ST7796 液晶屏（SPI 接口），返回错误码
    if (ret != ESP_OK) // 判断 LCD 初始化是否成功
    {
        printf("lcd_init failed, ret = %d\n", ret); // 初始化失败时打印错误信息及错误码
        return;                                     // 失败则直接退出
    }

    // 设置 LCD 显示屏背景颜色为黑色（与源工程保持一致）
    lcd_set_color(0);

    // 延时，等待摄像头模块上电稳定
    vTaskDelay(1000 / portTICK_PERIOD_MS); // 延时 1000 毫秒

    // 初始化摄像头（内部打开 DVP_PWDN 电源、配置 esp_camera、检测 GC0308）
    ret = camera_init(); // 调用自定义 camera_init 初始化摄像头，返回错误码
    if (ret != ESP_OK)   // 判断摄像头初始化是否成功
    {
        printf("camera_init failed, ret = %d\n", ret); // 初始化失败时打印错误信息及错误码
        return;                                        // 失败则直接退出
    }

    boot_mem_log("lcd+camera");

    // LVGL 接管屏幕：绘制缓冲与 LVGL 内存池的安排见 ui_lvgl_port.c 文件头，触摸可选。
    // 失败时 AI 任务会回退到直绘整帧（保持原有可用性），所以只告警不中止
    if (ui_lvgl_init() == ESP_OK)
    {
        esp_err_t ui_ret = ui_app_start(); // 创建主页/注册/日志/设置/诊断 5 个页面
        if (ui_ret != ESP_OK)
        {
            printf("ui_app_start failed, ret = %d (界面不可用，回退直绘)\\n", ui_ret);
        }
    }
    else
    {
        printf("ui_lvgl_init failed (界面不可用，回退直绘)\\n");
    }

    boot_mem_log("ui");

    // 启动系统监控任务（周期打印 fps / AI 耗时 / 堆内存 / 栈水位），必须在业务任务之前
    if (sys_monitor_start() != ESP_OK)
    {
        printf("sys_monitor_start failed (监控功能不可用)\n"); // 监控失败只影响观测，不阻塞主流程
    }

    // M3：挂载 SPIFFS（日志/抓拍）→ 初始化日志与抓拍 → 启动门禁控制。
    // 任一失败只告警：识别/开门主流程不依赖文件系统
    if (storage_init() == ESP_OK)
    {
        if (access_log_init() != ESP_OK)
        {
            printf("access_log_init failed (门禁日志不可用)\n");
        }
        if (snapshot_init() != ESP_OK)
        {
            printf("snapshot_init failed (陌生人抓拍不可用)\n");
        }
    }
    else
    {
        printf("storage_init failed (日志/抓拍不可用)\n");
    }
    if (door_controller_init() != ESP_OK)
    {
        printf("door_controller_init failed\n");
    }

    // 启动人脸识别应用：创建采集、AI 检测识别、LCD 显示、按键检测等任务。
    // 必须排在 net_service_init() 之前：face_task 需要 16 KB **连续**内部 RAM，
    // WiFi 驱动在初始化时吃掉大块连续堆，先启 WiFi 会让 face_task 报 ESP_ERR_NO_MEM
    // （实测内部 RAM 仍有 25 KB，但最大连续块只剩 8.7 KB）。
    boot_mem_log("storage/door");

    app_human_face_recognition();

    boot_mem_log("face");

    // M4：网络（WiFi STA + SoftAP 配网）+ SNTP 对时。不阻塞：已配置则后台连接，
    // 未配置则立刻开配网热点（DoorCam-XXXX）。失败只告警，人脸识别/界面照常
    //
    // 先**暂停 WiFi 主动连接**再初始化网络：下一步语音初始化里 MultiNet 的
    // create()（构建 FST）会吃掉全部剩余内部 RAM（实测那 4 s 窗口内部只剩几十
    // 字节）。若 WiFi 恰好在这段时间连接成功，esp_coex 要新建合法定时器，
    // 申请失败就 ESP_ERROR_CHECK abort 复位（“有时开机几秒就重启”）。
    // WiFi 驱动/缓冲在 net_service_init() 里照常分配，暂停的只是“发起连接”。
    wifi_manager_set_connect_paused(true);
    if (net_service_init() != ESP_OK)
    {
        printf("net_service_init failed (网络功能不可用)\n");
    }

    boot_mem_log("net");

    // M5 第二阶段：离线语音（唤醒词 + 中文命令词）。放在网络之后：
    // AFE/MultiNet 的工作缓冲都在 PSRAM，但任务栈 8 KB 是**连续内部 RAM**，
    // 必须等 WiFi/MQTT 把大块连续堆分完之后再申请，否则会报 ESP_ERR_NO_MEM。
    // 模型从 model 分区 mmap（3.0 MB），失败只告警：语音不可用，其余功能照常
    if (voice_assistant_init() != ESP_OK)
    {
        printf("voice_assistant_init failed (语音功能不可用)\n");
    }

    // M5 第二期：语音播报（离线素材，见 main/audio/prompts/）。不占内部 RAM：
    // 素材是 Flash 里的常量，初始化时创建常驻播报任务（3 KB PSRAM 栈），失败只告警
    //（voice_prompt_say 内部会退回提示音）
    if (voice_prompt_init() != ESP_OK)
    {
        printf("voice_prompt_init failed (语音播报不可用)\n");
    }

    // 语音引擎建完、内部 RAM 峰值已过：恢复 WiFi 主动连接（已连上的不会重连）。
    // 未配网时这里只开热点，同样不受影响
    wifi_manager_set_connect_paused(false);
}
