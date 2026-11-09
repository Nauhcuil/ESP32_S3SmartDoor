# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

本项目是基于 ESP-IDF 5.5.x 的 ESP32-S3 固件：板载 GC0308 DVP 摄像头、ST7796 480x320 SPI 液晶屏、ES8311 音频编解码与 FT6X36 触摸屏（均挂 I2C），在片上进行实时人脸检测与人脸识别（使用 ESP-DL）。屏幕提示文字为中文；源码注释同样是中文（每个文件/函数带 Doxygen 风格说明），修改代码时请保持这种风格。

目标配置：`esp32s3`、240 MHz、16 MB Flash，必须启用 PSRAM（OCT 80 MHz）。分区表为自定义（`partitions.csv`）：

| 分区 | 大小 | 用途 |
|---|---|---|
| `factory` | 6.5 MB | 应用 |
| `storage` | 256 KB | esp-dl 裸人脸库（**不是**文件系统） |
| `spiffs` | 2 MB | 日志、陌生人抓拍等文件 |
| `model` | 3.5 MB | ESP-SR 语音模型（mmap） |
| `coredump` | 64 KB | 崩溃转储 |

已注册的人脸数据持久化保存在 `storage` 分区。M0 基线变更了 `storage` 偏移（原 0xc00000 → 0x690000），升级后旧人脸库需重新录入。

## 构建 / 烧录 / 监视

需要先激活 ESP-IDF 环境。本机 IDF 由 EIM 安装：IDF 源码在 `D:/YueQianStudy/Tool/esp/v5.5.6/esp-idf`，工具链与 Python venv 在 `C:/Espressif/tools`（`IDF_TOOLS_PATH=C:/Espressif/tools`）。**Git Bash 下 `export.sh` 会因 MSys/Mingw 不受支持而拒绝激活**，应改用 PowerShell 环境脚本：

```powershell
. 'C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1'
idf.py build
```

仓库内没有测试、linter 或 CI，唯一的验证手段是 `idf.py build`。`build/` 是真实构建目录，其中的 `compile_commands.json` 供 clangd 使用。`sdkconfig.before_m0_regen` 是 M0 改造前的配置备份，仅供对照，不属于构建输入。

```bash
idf.py set-target esp32s3   # 仅在 sdkconfig 缺失/需要切换目标时执行
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # Windows 下：idf.py -p COM14 flash monitor
idf.py menuconfig           # 配置存于 sdkconfig（修改后用 `idf.py save-defconfig` 回写 sdkconfig.defaults）
```

**修改配置的正确流程**：先编辑 `sdkconfig.defaults`，再删除 `sdkconfig` 并执行 `idf.py reconfigure`（或 `set-target`）重新生成。直接改 defaults 不会覆盖已存在的 `sdkconfig`。

组件依赖由 IDF Component Manager 根据 `main/idf_component.yml` 管理（`lvgl` ~8.3.11、`esp_lvgl_port`、`esp-sr` **~2.4.7**、`esp32-camera`、`esp_lcd_st7796`、`esp_lcd_touch_ft6x36`、`es8311`）。**`esp-sr` 必须锁在 2.4.x**：2.5.0 起依赖托管组件 `esp-dl` 3.3.x，会与 vendored 的 `components/esp-dl` 2.0.0（人脸识别用）同名冲突；2.4.7 只依赖 `esp-dsp`/`dl_fft`/`cjson`。ES8311/ES7210 的实际驱动是工程内自写的（托管 `es8311` 组件因旧版 I2C 驱动没被使用）；已安装副本位于 `managed_components/`，版本锁定在 `dependencies.lock`。要更换托管组件，修改 `main/idf_component.yml` 后重新构建，不要手工改动 `managed_components/`。

## 架构

`main/main.c` 是唯一的硬件启动入口，依次调用 `nvs_flash_init() → i2c_init() → pca9557_init() → app_events_init() → audio_service_init() → clock_service_init() → tp_init() → lcd_init() → camera_init() → ui_lvgl_init() → ui_app_start() → sys_monitor_start() → storage/log/snapshot/door_controller → app_human_face_recognition() → net_service_init() → voice_assistant_init()`（`tp_init()` 失败只告警不中止；M1 起屏幕由 LVGL 接管，`ui_lvgl_init`/`ui_app_start` 失败同样只告警并回退到直绘）。`nvs_flash_init` 之后的硬件顺序不可颠倒：摄像头 SCCB 复用 I2C0（GPIO1/2），摄像头电源由 PCA9557 输出第 2 位控制，LCD 片选为 PCA9557 第 0 位。`main.c` 不调用 `camera_lcd_start()`——`camera.c` 中的预览路径仅供独立预览使用，人脸识别应用自行创建采集/显示任务。
**末尾两步的顺序是内存约束，不要调换**：`app_human_face_recognition()` 要 16 KB 连续内部 RAM（WiFi 初始化后就拿不到）；`voice_assistant_init()` 的 8 KB 任务栈虽然从 PSRAM 分配，但 AFE/MultiNet 创建时会临时切碎内部堆，所以必须排在 WiFi/MQTT 之后。两者失败都只告警。
**语音初始化期间必须暂停 WiFi 主动连接**：`net_service_init()` 之前调 `wifi_manager_set_connect_paused(true)`，`voice_prompt_init()` 之后置回 false。原因是 MultiNet `create()`（打印 `Build fst from commands.`）会吃掉**全部**剩余内部 RAM（实测那 4 s 里 `内部=19 最大块=0`）；若 WiFi 恰好在窗口内连接成功，esp_coex 要建合法定时器（`ets_timer_setfn` → `esp_timer_create`），申请失败就 `ESP_ERROR_CHECK` abort（现场：“有时开机几秒就重启”）。WiFi 驱动与缓冲在 `esp_wifi_init` 阶段就已分配，暂停的只是“发起连接”动作（配网热点不受影响）。

M2 起 `who_human_face_recognition.cpp` 降为**编排层（shim）**：只创建两个深度为 2 的帧队列、启动摄像头/显示/按键任务，并把业务请求转给 `face_service`（`main/face/`）。模型只由 `face_task` 持有。

```
摄像头采集 (Core1, 5) --xQueueAIFrame(2)--> face_task (Core0,5) --xQueueLCDFrame(2)--> LCD 任务 (Core0,5)
                                                ^ face_cmd_queue（命令/响应，单发单收）
button_task (Core0,3) ──只调用 face_service 门面（阻塞等待回执），自己不碰相机/模型
```

- **`face_service`（C 门面）**：`face_service_init()` 创建命令队列并启动 face_task；`enroll / recognize / list / delete / clear_all / set_threshold / get_status ...` 由任意任务调用。单请求在飞（`s_api_mutex` + 完成信号量），调用方阻塞至回执或超时。错误码：`FACE_OK`、`FACE_ERR_NO_FACE`、`FACE_ERR_QUALITY`、`FACE_ERR_NOT_ENROLLED` 等。
- **`face_task`（C++，栈 16 KB）**：持有 `HumanFaceDetectMSR01` → `HumanFaceDetectMNP01`、`FaceRecognition112V1S8`（M6 从 S16 换成 S8；`set_partition(storage)` + `set_ids_from_flash()`）、`face_tracker`、`face_quality`、`face_liveness`。主循环：取帧(100 ms 超时) → 检测 → `analyze_frame`（质量评估 / 轨迹更新 / 在帧上画检测框 / 更新主页状态卡与状态快照）→ 处理一条命令 → 否则仅在 AUTO 策略下自动识别（稳定轨迹每 1 s）→ 帧送入显示队列 → 写 Flash 类命令（删除/清空/阈值）。

- **识别策略（默认 MANUAL）**：`face_service.c` 的 `s_strategy` 默认 `FACE_STRATEGY_MANUAL`——主页平时只显示预览与“请点开始识别”，只有收到 `FACE_CMD_RECOGNIZE`（主页“开始识别”按钮、GPIO11 单击、后续语音）才识别一帧。手动模式不产生连续推理，预览帧率稳定。切回自动识别只需 `face_service_set_strategy(FACE_STRATEGY_AUTO)`。手动识别每次都会清掉 `s_track_access_done/s_track_deny_done`，保证同一轨迹重复点击也有覆盖层与日志。
- **自动识别的节流与活体门控**：活体（启发式，`face_liveness.c`）在非识别帧按 ≥100 ms 节奏喂纹理/运动，识别帧强制喂入并附带 `get_face_emb(-1)`（最近一次输入的特征；`get_face_emb(匹配 id)` 返回的是注册模板，跨帧恒定，不可用于一致性）。`decide_liveness_action()`：LIVE→开门；SPOOF→拒绝；样本不足（UNKNOWN）→ `FACE_ACTION_NONE`（界面“活体检测中”，不开门不拒绝），自动识别下次复核。兜底：已匹配人员连续 ≥5 个识别帧且 ≥4 s 仍无确定结论，按活体失败拒绝。
- **检测灵敏度（“一直显示未检测到人脸”的根因）**：`FACE_DETECT_RESIZE_SCALE`（`face_task.cpp` 顶部，默认 0.5）决定 MSR01 把 320x240 帧缩到多小再送网络。缩得太小（0.3）时几十像素的人脸会缩成十几像素而彻底消失——模型自检（`face_selftest.cpp` 路径 4）在 QVGA 画布上实测：脸高 58 px 时 0.3 检不出、0.5/1.0 能检出；脸高 36 px 时只有 1.0 能检出。耗时随面积增长：0.3≈18 ms、0.5≈50 ms、1.0≈208 ms（1.0 会把流水线压到 3 fps）。MNP01 的 score 门限也从 0.4 降到 0.3，否则出现“MSR01 候选 1、MNP01 结果 0”。改这两个值后请重跑自检路径 4 复核。
- **ESP-DL 2.0 API 要点**：阈值方法是 `set_thresh()/get_thresh()`（没有 `set_threshold`）；`delete_id()` 在库空时返回 -1，成功与否应通过重新 `get_enrolled_ids()` 校验；`set_ids_from_flash()` 无数据时返回 -1（不要直接当已录入人数显示），且**原始返回值同时就是分区里的记录数**——读出来是 0 而分区里有数据，说明模型校验码对不上（换模型/分区偏移变了），需要 `check_partition()` + 清库重录；启动日志“人脸库加载完成：N 个（set_ids_from_flash=M）”里的 M 就是它。`set_partition()` 返回 1 表示设置成功。
- **帧缓冲区归属**：`camera_fb_t*` 由 LCD 任务唯一 `esp_camera_fb_return()`。face_task 必须先把帧送入 LCD 队列、再执行写 Flash 的命令——LCD 与 Flash 共用 SPI/Flash 总线，先写 Flash 会黑屏。
- **按键**（GPIO 11，低电平有效，4 状态机，20 ms 消抖）：长按 >1 s = 注册（`face_service_enroll("")`，默认 `Person_N`）；单击 = 识别；双击 = 清空人脸库。
- **主页状态卡（M2）**：`ui_pages.c` 创建控件后调用 `ui_home_register_widgets()` 登记；face_task 通过 `ui_home_set_state(text, 0xRRGGBB)` / `ui_home_set_counts(enrolled, today)` 更新（内部 20 ms 锁超时，等不到就丢本次文字更新；内容未变不重绘）。计数传负数表示不变。主页布局：状态卡（84 px）+“开始识别”整行按钮 + 注册/门禁记录（半宽）+ 设备设置（整行）；**主页不再有“设备诊断”入口**（避免右侧按钮过密、触控目标过小），改从设置页第 6 行的“设备诊断”按钮进入，返回也回设置页。
- **LCD 任务**：LVGL 模式下把帧交给 `ui_preview_push_frame()`（限速 45 ms；仅主页/注册页生效），否则 `lcd_draw_bitmap` 居中直绘。**限速值与 `UI_PREVIEW_SERVICE_MS` 必须成对调**：LCD 任务（优先级 5）每消费一帧才可能推一次，而推帧会被“上一帧还没被 LVGL 定时器取走”拒绝，拒绝时把时间戳清零、下一帧立即重试。实测（采集 22 fps、检测 4.5 fps，窗口统计见下）：限速 106 ms + 服务 50 ms → 上屏 ~7 fps；70 + 30 → ~9 fps；**45 + 30 → ~13 fps**（刷新间隔 45~60 ms，肉眼流畅）。再往下收益很小：LVGL 预览服务定时器实测最快约 39 ms 一拍（渲染 + LVGL 内部刷新周期），且每帧要拷 153 KB。
- **face_task 检测节流**：非命令帧距上次推理不足 `FACE_TRACK_INTERVAL_MS`（200 ms）时直接补画上一帧的脸框并送预览，不跑模型。检测单帧 20~70 ms，逐帧推理会让 face_task 独占 Core0（LVGL 优先级 1 抢不到 CPU → 预览卡顿、IDLE0 任务看门狗）。命令帧（识别/注册）不节流。
- **帧率口径（排查“画面卡顿”时先看这三个数）**：`sys_monitor` 每 5 s 打印的 `fps=` 是**检测帧率**（只统计真正跑完两级的帧，受 200 ms 节流限制，正常 4~5），**不是**采集或上屏帧率；`preview=` 才是真正上屏的预览帧率。调试期可用 `sys_monitor_start_fps_window(ms)` 开一个一次性窗口，到时打印“采集/送显示/检测”三个帧率（计数器分别在摄像头采集任务与 face_task 送显示处打点）。实测：采集 22 fps、送显示 22 fps、检测 4.5 fps、上屏 13 fps——采集与送显示远高于 `fps=` 是正常的，别据此判断掉帧。**驱动层掉帧不会计入这三个数**：`cam_hal: FB-SIZE: n != 153600`（残帧被驱动丢弃）以及随后的 `122880`（丢 8 段）只在串口出现，画面表现为“跳一下”。根因与修法见 `sys/sys_monitor` 一行（本质上都是“帧内的半缓冲段没被拷走”：`cam_start_frame()` 里手动 `ll_cam_do_vsync()` 之后 DMA 立刻开跑，cam_task 只要晚 1.7 ms 以上，环形中转区就被写满整圈）。

M2 的人脸业务层（`main/face/`，`INCLUDE_DIRS` 已加入 `face`）：

| 模块 | 作用 |
|---|---|
| `face_service.h/.c` | C 门面：命令/响应（单发单收）、阈值与活体开关、`face_action_text()`、状态快照 |
| `face_task.cpp` | 模型唯一持有者：检测/跟踪/质量/识别/活体/开门覆盖层；只在 Core0 运行 |
| `face_internal.h` | face_task ↔ face_service 的内部接口（回执、状态发布） |
| `face_tracker.h/.c` | 基于 IoU 的轨迹跟踪与稳定判定 |
| `face_quality.h/.c` | 人脸框尺寸/亮度/清晰度等质量门限 |
| `face_liveness.h/.c` | 启发式活体：纹理、运动、一致性加权；预留 `LivenessDetector` 模型接口。**阈值必须按实机日志调**：真人脸（QVGA、脸高约 100 px）的纹理比只有 1.4~1.5，区间取 1.15~1.75、纹理门限 25、综合 55、运动 0.6；纹理得分的初值/重置值是中性 50（“无法评估”≠“过平”），且只要 ≥5 个分区有效就评估。每次识别打印 `[纹理 比值 运动 一致 样本 综合]`，据此继续校准。 |

`main/` 下的支撑模块（各 `x.h` 均以 `extern "C"` 声明接口，供 C++ 调用）：

| 模块 | 作用 |
|---|---|
| `i2c` | I2C0 主机，GPIO1(SDA)/GPIO2(SCL)。**必须使用新版驱动**（`i2c.h` 中 `I2C_VERSION_2`）——esp32-camera 2.x 的 SCCB 通过 `i2c_master_get_bus_handle()` 复用该总线；混入旧版驱动会在启动检查时报 "driver_ng is not allowed" 并复位。 |
| `pca9557` | IO 扩展芯片 @0x19（bit0 = LCD CS，bit2 = DVP_PWDN）。 |
| `lcd` | ST7796 走 SPI3（MOSI 40 / SCLK 41 / DC 39，80 MHz）。方向由 `lcd.h` 的 `LCD_ORIENTATION` 宏决定（默认横屏 480x320，MADCTL 交换坐标轴 + 镜像）。整帧快速绘制用 `lcd_draw_bitmap`。文字使用 `main/font/` 中内嵌的 16/32 px 点阵字库；UTF-8 中文字符每字占 3 字节。面板 IO 开启了 `flags.psram_dma_direct`：帧缓冲在 PSRAM，若不开，SPI 每帧要临时申请 153 KB 内部 DMA 缓冲（内部 RAM 不足会导致 LCD 黑屏 + `Failed to allocate priv TX buffer`）。 |
| `camera` | `camera_init()`：经 PCA9557 控制 PWDN，RGB565 QVGA，2 个帧缓冲置于 PSRAM，`CAMERA_GRAB_WHEN_EMPTY`，GC0308 水平镜像。**`CONFIG_CAMERA_PSRAM_DMA` 必须为 n**（与参考例程一致）：打开时 DMA 直接写 PSRAM，全靠 `esp_cache_msync` 失效缓存，与 LVGL 读 PSRAM、Flash 操作交叠会读到旧行/半行，现场表现为“画面从右向左平移 + 彩色花屏”；两段式（内部 DMA 缓冲 → memcpy 到 PSRAM）只多一次拷贝。`camera_lcd_start()` 为独立预览（按 LCD 尺寸裁剪、每次 160 行分块传输），AI 应用不使用。 |
| `who_ai_utils.cpp` | `draw_detection_result()` 直接在 RGB565 缓冲上画边界框和 5 个关键点；`app_camera_decode()` 对非 RGB565 帧分配 RGB888 内存（调用者负责 free）。 |
| `tp` | FT6X36 触摸 @0x38，复用 I2C0。**`x_max/y_max` 填控制器原始范围（竖装 320x480），不是屏幕尺寸**；`esp_lcd_touch_get_coordinates()` 的软件变换顺序是“先镜像（`x_max - x`）再交换轴”，所以横屏用 `mirror_y + swap_xy`（镜像标志在原始坐标系下与面板 MADCTL 的 MX/MY 互换）。写错会让坐标落到屏幕外，表现为“按钮点了没反应”。调试时把 `tp.c` 的 `TP_LOG_RAW_COORDS` 置 1 打印原始坐标。`main.c` 已调用 `tp_init()`，句柄用 `tp_get_handle()` 取。 |
| `key/uart/led/pwm/i2s/es8311_codec` | 板级辅助模块（GPIO0/11 按键、ES8311 + I2S 播放经 `EMBED_FILES` 内嵌的 `canon.pcm`）。I2S 目前仅 TX。`main.c` 未引用，可按需复用。 |

M0 基线的应用级模块（`main/` 根目录与 `main/sys/`）：

| 模块 | 作用 |
|---|---|
| `app_events.h/.c` | 全局事件组 `g_sys_events`（`SYS_EVENT_*` 位）与互斥量 `g_log_mutex`/`g_face_db_mutex`/`g_audio_mutex`，由 `app_events_init()` 一次创建。 |
| `sys/app_config.h` | 应用默认参数（识别阈值、活体开关、模拟开门 `DOOR_RELAY_GPIO`、监控周期、NVS 键名与默认管理员 PIN）。 |
| `sys/sys_monitor.h/.c` | `sys_monitor_start()` 周期（5 s）打印 fps、检测/识别平均与峰值耗时、内部 RAM/PSRAM、关键任务剩余栈。业务任务用 `sys_monitor_report_frame()` / `sys_monitor_report_recognize()` 上报耗时。**栈水位必须用 `xTaskGetHandle()` + `uxTaskGetStackHighWaterMark()` 逐任务取，禁止用 `uxTaskGetSystemState()`**：后者在 FreeRTOS 内核自旋锁 `xKernelLock` 内遍历所有任务并扫描全部栈（face_task 16 KB、voice_task 8 KB 栈在 PSRAM），实测持锁 2.5~3 ms；Core1 上的 `cam_task`（摄像头 DMA 任务）被中断唤醒后也要拿这把锁才能上 CPU，于是 GDMA 把 3 个节点的环形中转区整圈覆盖，现场就是每 5 s 一次 `cam_hal: FB-SIZE: 145920 != 153600` 掉帧 + 下一帧再丢 8 段。改成逐任务取之后 90 s 实测 0 次。诊断页用 `sys_monitor_get_status()`（帧率/耗时/堆/开机时长，口径与 5 s 日志一致）与 `sys_monitor_get_task_stack(name, &bytes)`（传入完整任务名即可，内部会按 `configMAX_TASK_NAME_LEN` 截断后精确查找；LVGL 任务名是 `"LVGL task"`，带空格）。调试用 `sys_monitor_start_fps_window(ms)` 开一次性窗口（`esp_timer` 回调里打印，不占业务核），配套 `sys_monitor_count_capture()` / `sys_monitor_count_display()` 打点。 |
| `sys/clock_service.h/.c` | 系统时间来源：无 SNTP 时用 `__DATE__`/`__TIME__`（编译时刻）兜底，时区固定 `CST-8`；`clock_service_format_time(buf, size, with_date)` 输出 `"YYYY/MM/DD HH:MM"` 或 `"HH:MM"`。主页标题栏右上角的时钟由 `ui_home_attach_clock()` 创建（`UI_FONT_TITLE`，每秒一个 LVGL 定时器，内容不变不重绘）。M4 接 SNTP 后 `time()` 变真实时间，UI 不用改。 |

M1 的界面层（`main/ui/`，`INCLUDE_DIRS` 已加入 `ui`）：

| 模块 | 作用 |
|---|---|
| `ui_lvgl_port.h/.c` | `ui_lvgl_init()` 用 esp_lvgl_port 注册 ST7796 显示与 FT6X36 触摸，并对外提供 `ui_lvgl_is_ready()` / `ui_lvgl_lock(timeout_ms)` / `ui_lvgl_unlock()`。**LVGL 任务固定在 Core0、优先级 1**——esp_lvgl_port 的任务循环每次处理完定时器都 `vTaskDelay(1~30 ms)`，IDLE0 照样能被喂狗；优先级 0 时 LVGL 与 IDLE 平分 CPU，实测预览只剩 ~6 fps。真正会饿死 IDLE0 的是 face_task 逐帧推理，已由 `FACE_TRACK_INTERVAL_MS` 检测节流解决（见上）。显示缓冲 **20 行 x 双缓冲放在内部 DMA RAM**（`buff_dma=true, buff_spiram=false`）：摄像头 DMA 大量读写 PSRAM，LVGL 缓冲若放 PSRAM，SPI 刷新会抢带宽，启动首帧曾报 `DMA TX underflow`。rotation 全为 false（方向已由 `lcd_init` 与 `tp.c` 处理）。 |
| `ui_app.h/.c` | 页面枚举 `ui_page_t`（HOME/ENROLL/LOGS/SETTINGS/DIAG/**FACE_LIST**）、`ui_app_start()` 一次性创建 6 个页面、`ui_app_switch_page()`、`ui_app_set_status(text, 0xRRGGBB)`、`ui_app_show_door_overlay()`（M3 模拟开门用）。所有对外函数内部持 LVGL 锁，可从任意任务调用。 |
| `ui_pages.c`、`ui_widgets.c`、`ui_theme.h` | `ui_pages.c` 只保留主页/注册页/人脸库页；门禁记录、设置、诊断三页在 M3 拆成独立模块（`ui_logs.c`/`ui_settings.c`/`ui_diag.c`，`ui_pages.h` 转发它们的头文件给 `ui_app.c`）。`ui_widgets.c`/`ui_theme.h` 提供通用控件（标题栏/卡片/文字/按钮/列表）；尺寸与配色常量集中在 `ui_theme.h`，颜色一律写 `lv_color_hex(0xRRGGBB)`（`LV_COLOR_16_SWAP` 下直接赋 u16 会让红色变蓝）。**`ui_list_create()` 必须给容器设 `LV_FLEX_FLOW_COLUMN`**：`lv_obj_create()` 默认 `layout=NONE`，不设 flex 时追加的子项全停在 (0,0) 完全重叠（“人名挤在一起、点一下误删多个”）。**`ui_card_create()` 默认四边 8 px 内边距**：行高 ≤40 px 且卡片里放按钮/文字时要先 `lv_obj_set_style_pad_top/bottom()` 改小，否则 26~34 px 的子对象会被父卡片裁掉上下各几像素（实测设置页按钮被裁、诊断页 22 px 行只露半行字）。 |
| `ui_enroll.h/.c` | 注册页交互（开始注册、实时状态）与人脸库列表：`ui_enroll_attach()` 绑注册页控件，`ui_enroll_bind_face_list()` 绑删除页列表，共用一个 250 ms 定时器（仅在 ENROLL/FACE_LIST 可见时工作）。录入/删除都在一次性工作线程里阻塞调用 `face_service`。**列表指纹** `s_list_fp_count` 在录入/删除成功后必须置 -1 强制重建，否则定时器认为“未变化”而跳过（曾出现“注册 1 人后删除页没记录、第 2 人才出现”）；`< 0` 时也会拉取（1 s 节流）。成功提示里的姓名取人脸库实际记录名，不要用 `id` 拼（id 会回填）。 |
| `ui_face_list_create()` | 删除人脸页（`UI_PAGE_FACE_LIST`，全宽 464 px、行高 40 px）：注册页右侧只有 135 px 放不下人脸库列表，所以列表单独成页，注册页只留“删除人脸”入口按钮。点条目弹居中 `lv_msgbox` 二次确认（中文必须 `lv_obj_set_style_text_font(mbox, UI_FONT_BODY, 0)`，默认字体无汉字）。 |
| `ui_home_api.h/.c` | 主页状态卡/计数的跨任务更新接口（`ui_home_set_state` / `ui_home_set_counts`），内部持 LVGL 锁（20 ms 超时）并在控件创建前暂存最新文字。 |
| `ui_home.h/.c` | 主页“开始识别”按钮的非阻塞实现：回调里只调用 `ui_home_recognize_async()`，它创建一个一次性 worker（4 KB，Core0，优先级 2）阻塞等待 `face_service_recognize()` 回执；`s_busy` 防止连点排队。**不要在 LVGL 回调里直接调 `face_service_recognize()`**（会冻结界面与预览）。 |
| `ui_preview.c` | 摄像头预览：**三块** PSRAM 图像缓冲（`UI_PREVIEW_BUFS=3`）+ 三个 `lv_img_dsc_t`，LCD 任务只把帧 `memcpy` 进“后台”缓冲并记录**该帧实际写入的缓冲号**（`s_pending_idx`），真正的 `lv_img_set_src` + `invalidate` 由 `ui_preview_service()`（LVGL 定时器，`UI_PREVIEW_SERVICE_MS` = 30 ms）在 LVGL 任务内完成。**两块不够**：刚交给 `lv_img` 的 `s_front` 与面板上仍在显示的 `s_prev_front` 都不能被写，而 LCD 任务优先级 5 会抢占 LVGL 任务（优先级 1），两缓冲时写目标必然是其中之一 → “画面一帧一帧平移 + 彩色花屏”。**不能用“bool 有新帧 + 定时器自己翻转索引”**：显示任务与 LVGL 定时器并发，定时器可能把只写了一半的后台缓冲交给 `lv_img`。切换顺序必须是“先置下一帧写哪块，再清有新帧标记”，否则 LCD 任务会读到旧的 `s_back` 而覆盖正在显示的缓冲。字节序原样拷贝（`LV_COLOR_16_SWAP` 下与摄像头流逐字节相同）。对外提供 `ui_preview_get_push_count()` 供 `sys_monitor` 打印 `preview=` 帧率。 |
| `ui_logs.h/.c` | 门禁记录页（M3）：`access_log_read_page()` 每页 4 条（行高 30），上一页/下一页/清空记录。**翻页记的是每页的 seq 上界** `s_page_before[]`（日志只追加，上界只在翻页后的首次刷新时写入，否则新记录会挪动边界、同一批出现在两页里），不用页码→序号整表。读 SPIFFS **必须在 LVGL 锁外**（Flash 读会短暂关 cache），拿锁后只更新控件；定时器 500 ms，仅在页面可见且“强制刷新 / 第 0 页总条数变化”时才读文件。清空走一次性线程（3 KB）。标题栏下方是表头，时间列 `format_ts()` 在 ts 不是 epoch 毫秒时显示 `--`（宁可未知，也不显示 1970）。 |
| `ui_settings.h/.c` | 设备设置页（M3）：6 行（阈值 ±0.05 / 活体开关 / 陌生人抓拍开关 / 管理员密码 / 网络配置 / 诊断入口 + 恢复出厂），行高 34、间距 4。可写项一律走 `face_service_set_threshold/set_liveness` 与 `app_nvs_*`；**会阻塞的**（设阈值要等回执、恢复出厂要清库清日志清抓拍）放进一次性线程 `ui_set_job`（3 KB），线程结束置 `s_dirty` 由 300 ms 定时器补刷新。密码对话框 = `lv_msgbox` 居中 + `lv_textarea`（password 模式、只收数字）+ 数字键盘，两阶段（先验当前密码→再输新密码）；**密码只存 NVS**，出厂默认见 `app_config.h`，界面只显示 `****`。恢复出厂不整区擦 NVS（M4 的 WiFi/云端参数归各自模块）。 |
| `ui_diag.h/.c` | 设备诊断页（M3 先接实时数据，M6 补环境传感器）：10 行 x 22 px，内容为阈值/活体、人脸数量、检测/预览帧率、检测耗时、内部 RAM/PSRAM、SPIFFS 已用%、抓拍张数、网络/云端（M4 之前固定“未启用”）、固件版本。定时器 1 s，**只在诊断页可见时**采集；`storage_usage()`/`snapshot_count()`/`sys_monitor_get_task_stack()` 都在锁外做，取锁后 `set_row()` 逐行比较、内容未变不重绘。入口在设置页（主页不放），返回也回设置页。 |
| `ui_strings.h` | 全部中文文案的集中地；字体子集按它生成（见 `ui/README_font.md` 与 `ui/gen_font_chars.py`）。 |
| `ui/ui_fonts/font_cn_16.c`、`font_cn_24.c` | lv_font_conv 生成的 4bpp 非压缩子集（字符集 `ui/font_symbols.txt` + ASCII 0x20-0x7E），缺字回退到 Montserrat。**新增文案后必须把缺字补进 `font_symbols.txt` 并重新生成**，否则中文显示空白（或只出现方块）。两个字库必须用**同一份** `font_symbols.txt` 生成（曾出现 24px 字库缺少“到/整/晰/该/调”，主页标题出现方块）；校验方法：`python -I main/ui/gen_font_chars.py` 看字符数，并确认两份字库的 cmap `list_length` 一致。 |

`lcd_get_io_handle()` 暴露面板 IO 句柄，供 LVGL 注册显示使用。

M3 的存储 / 日志 / 抓拍 / 门禁控制（`main/sys/`）：

| 模块 | 作用 |
|---|---|
| `storage.h/.c` | `storage_init()` 挂载 SPIFFS `spiffs` 分区到 `/spiffs`（失败自动格式化，仍失败只告警），创建 `log`/`snap` 占位文件并置 `SYS_EVENT_STORAGE_READY`；`storage_is_ready()`、`storage_usage(total, used)`。 |
| `access_log.h/.c` | 门禁事件日志：`/spiffs/log/events.jsonl` 单行 JSON 追加写，超 256 KB 轮转保留 4 个文件。`seq` 启动时从文件尾恢复，配合 `boot` 字段供云端去重。API：`access_log_append`、`read_recent`、`read_page(before_seq)`、`today_open_count()`（1 s 缓存；时间未同步时只统计本次上电）、`clear`。类型/结果常量在头文件。 |
| `snapshot.h/.c` | 陌生人抓拍：队列深度 2 + `snapshot_task`（Core0, 2, 5 KB）。调用方 `frame2jpg()` 后 `snapshot_make_name()` → `snapshot_submit()`（失败时由 submit 释放 jpg）；写盘后按文件名排序删除超额文件（上限 60）。 |
| `door_controller.h/.c` | 模拟开门：队列深度 8 + `door_task`（Core0, 3, 5 KB）。`door_controller_open()` / `deny()` / `stranger()` 写日志、更新主页计数、显示覆盖层与状态；`APP_CFG_DOOR_RELAY_GPIO >= 0` 时才驱动继电器脉冲（默认 -1 不接）。 |

抓拍与日志判定参数（确认帧数、冷却、每小时上限等）集中在 `sys/app_config.h`；抓拍开关存 NVS（`app_nvs_get/set_snapshot_enabled`）。

M4 的网络与云端（`main/net/`，`INCLUDE_DIRS` 已加入 `net`）：

| 模块 | 作用 |
|---|---|
| `net_config.h/.c` | WiFi 与巴法云参数（SSID/密码/UID/主题/端口）只存 NVS，代码里**没有**任何凭据；`net_config_get/set_wifi`、`net_config_get/set_bemfa`、`has_wifi`、`has_bemfa`、`clear_wifi`。 |
| `wifi_manager.h/.c` | STA + 事件驱动重连（15 s × 3 次失败进配网）；`start_provisioning()`（配网期间暂停云端，内部 RAM 让给门户）；`apply_credentials()` 写 NVS 后 2 s 重启；`set_connect_paused(true/false)` 暂停/恢复主动连接（语音初始化期间用，见 `main.c` 一行）。**`esp_wifi_set_mode` 必须在 `esp_wifi_set_config` 之前**，`esp_wifi_set_storage(ESP_WIFI_STORAGE_RAM)` 只能在 `esp_wifi_start` 之后。 |
| `prov_http.h/.c` | SoftAP `DoorCam-XXXX`（MAC 低 2 字节）+ HTTP 门户：`/` 表单、`/save` 写 NVS 后重启、`/favicon.ico`、五个系统探测地址返回 302 到首页（Android/iOS/Windows 自动弹“登录网络”）。页面缓冲 4 KB **在 PSRAM**（放 .bss 会吃掉界面工作线程所需的内部 RAM）；`httpd_resp_send` 带 Content-Length（手机对 chunked 短页面兼容差）；`max_uri_handlers=8`，多注册的探测 URI 会被静默忽略。`PROV_SELFTEST` 发布版必须 0。 |
| `time_sync.h/.c` | SNTP 3 个服务器、周期 15 分钟，成功置 `SYS_EVENT_TIME_VALID`；`clock_service` 的 `time()` 变真实时间后主页时钟（秒级）与日志时间戳自动正确。 |
| `net_service.h/.c` | 网络门面：`net_service_init()` 按序启动 WiFi → SNTP → 云端；`net_service_summary()`/`get_ap_ssid()`/`start_provisioning()` 给设置页与诊断页用。 |
| `cloud_service.h/.c` | 巴法云 MQTT：任务只按需创建（WiFi 连上后才建，配网时暂停）；4 个主题槽位 ACCESS/STRANGER/STATUS/CMD 由逗号分隔字符串解析（只填 1~3 个时按降级映射）；**连接后订阅全部 4 个主题**（巴法云按“是否被订阅”显示在线，只订阅命令主题会让其余主题显示离线；下行仍只认命令主题）；日志行 QoS1 上报到 ACCESS/STRANGER，心跳 30 s 与命令回执 QoS0 上报到 STATUS；远程命令 JSON `{"cmd":"open","token":"<管理员PIN>"}`，纯文本命令无令牌一律拒绝；远程开门限速 3 次/分钟。 |
| `bemfa_proto.h/.c` | 下行解析（JSON 或纯文本）、心跳/回执拼接。命令词：`open`/`query_log`/`set_threshold`(+value)/`set_liveness`(+value)/`reboot`。 |

M5 的离线语音与音频（`main/audio/` 新增，`i2s`/`es8311_codec`/`es7210_codec` 在 `main/` 根目录）：

| 模块 | 作用 |
|---|---|
| `i2s.h/.c` | **I2S0 全双工**（`i2s_new_channel(&cfg, &tx, &rx)`）：ESP32 作主机，MCLK=GPIO38（12.288 MHz = 48 kHz × 256）、BCLK=GPIO14（1.536 MHz）、WS=GPIO13，DOUT=GPIO45→ES8311 DSDIN，DIN=GPIO12←ES7210 SD_OUT。16 bit、Philips 格式、48 kHz。两块编解码共用同一组时钟；RX 通道在全双工下被驱动切为从机（启动日志的 `switched from master to slave` 是正常的）。对外 `i2s_tx_write` / `i2s_rx_read` / `i2s_rx_drain`（环形缓冲清空）。 |
| `es8311_codec.h/.c` | 播放侧 DAC（I2C @0x18）。M5 起用新版 I2C 直接写寄存器（旧版组件 `espressif/es8311` 走 `i2c_cmd_link_*`，与本工程 `I2C_VERSION_2` 冲突）。新增 `es8311_set_volume/get_volume`（0~100 → 寄存器 0x32）、`es8311_set_mute`。 |
| `es7210_codec.h/.c` | 录音侧四路 ADC（I2C @0x41，CE 引脚决定地址）。**本板是模拟麦接 ES7210 的 MIC 输入（MICBIAS 2.87 V），与 ES8311 无关**。48 kHz、I2S 从机、16 bit；增益寄存器 0x43~0x46 低 4 位，默认 `ES7210_MIC_GAIN_DB=10`（30 dB）。不用托管组件的原因同 ES8311：它依赖旧版 I2C 驱动。 |
| `audio_service.h/.c` | **I2S TX 的唯一 owner**：`audio_service_play_mono16/play_tone`（阻塞、`g_audio_mutex` 串行化）。播放时序：清 DMA → ES8311 解除静音 → 20 ms → 拉高 PA_EN（PCA9557 **IO1**）→ 单声道扩立体声按 1024 帧块写 → 补 30 ms 静音 → ES8311 静音 → 10 ms → 拉低 PA_EN。扩声道缓冲（8 KB）与提示音缓冲用 PSRAM，失败退回内部 RAM。`audio_service_play_mono16_rate(pcm, frames, src_rate_hz, …)` 供 16 kHz 播报素材使用（内部线性插值 3 倍升到 48 kHz，要求 `I2S_SAMPLE_RATE % src_rate_hz == 0`）。`audio_service_record_test()` 仅自检用。`AUDIO_SELFTEST` 发布版必须为 0（它占用 RX，会和语音任务抢数据，实测触发 `audio_test` 栈溢出）。 |
| `voice_prompt.h/.c` | **语音播报**（M5 第二期）：离线语音片段 + 一次性播放线程。素材在 `main/audio/prompts/`，由 `main/CMakeLists.txt` 的 `EMBED_FILES` 嵌进 Flash（链接符号 `_binary_p_<名字>_pcm_start/_end`），**格式 16 kHz / 16bit 小端 / 单声道裸 PCM（无 WAV 头）**，替换录音保持文件名即可。`s_assets[]` 的下标就是 `voice_prompt_id_t`（新增素材要同时加 extern 与表项）。`voice_prompt_say()` 非阻塞投递（队列 8，满则丢最旧，1.5 s 内同一条合并），播报线程从 **PSRAM** 分配 3 KB 栈、空闲 8 s 自删；播放前 `voice_assistant_set_input_paused(true)`，避免 AFE 把扬声器声音当成麦克风声道切换信号。**暂存缓冲 `s_stage` 在首次播放时才从 PSRAM 分配**（启动期不要预占）。 |
| `voice_assistant.h/.c` | **语音助手**：`voice_assistant_init()` 从 `model` 分区 mmap 模型（`esp_srmodel_init("model")`，`wn9_hilexin` + `mn7_cn`，合计约 2.83 MB），创建 MultiNet7 与 AFE（`AFE_TYPE_SR` + `AFE_MODE_LOW_COST` + `AFE_MEMORY_ALLOC_MORE_PSRAM`），起 `voice_task`。**先建 MultiNet、再建 AFE**：MultiNet 构建 FST 时会吃掉全部剩余内部 RAM（实测峰值只剩几十字节），放在前面能留 ~16 KB 余量给 WiFi；AFE 常驻占内部 RAM 约 16.5 KB，是一次性小块分配。各阶段内存收支由 `voice_mem_log()` 打印（`[内存] … 内部空闲= 最大块=`）。`voice_assistant_state_text()` / `voice_assistant_get_level()` / `voice_assistant_last_command()` 供诊断页与状态卡；播报期间的输入暂停由 `voice_assistant_set_input_paused()`（语音播报用）+ `SYS_EVENT_AUDIO_BUSY`（普通音频）共同完成。 |

**M5 硬件接线**：ES8311 CCLK→GPIO1（与 I2C SDA 共用总线线序，注意不是额外引脚）、CDATA→GPIO2、MCLK→GPIO38、SCLK→GPIO14、LRCK→GPIO13、DSDIN→GPIO45；ES7210 SCLK→GPIO14、LRCK→GPIO13、SD_OUT→GPIO12。PA_EN 在 PCA9557 IO1（上电默认低，仅播放期间拉高）。

**语音数据流**：`voice_task`（Core0，优先级 4，栈 8 KB）每轮读 40 ms I2S 立体声 → 取麦克风所在声道并 3 倍抽取（48 kHz → 16 kHz）→ 填满 `s_feed_chunk` 就 `afe->feed/fetch` → AFE 输出（512 点）拼进约 1 s 的队列喂给 MultiNet（512 点/次）。状态机两段：待唤醒（WakeNet9 `Hi,乐鑫`）→ 聆听命令（5 s 窗口）→ 命中派发 → **继续留在聆听窗口**（可连说多条命令；`VOICE_CMD_REPEAT_MS` 内同一命令不重复派发）。唤醒后前 `VOICE_CMD_ARM_MS`（400 ms）内的 MultiNet 结果不采信（唤醒词尾音）；**`VOICE_MIN_PROB` 只是日志参考量，不参与放行**（见下条）。**播报期间（`SYS_EVENT_AUDIO_BUSY`）照常读 RX 但丢弃并推进 MultiNet 状态机**，恢复后 drain + `reset_engines()`，否则会听见自己的扬声器。

- **命令词必须用拼音登记**（mn7_cn 的根因坑）：`esp_mn_commands_add(id, "da kai men jin")`，中文串只用于日志/界面回显。`check_speech_command()` 对中文 UTF-8 一律返回 0（日志 `invalid command`）；`esp_mn_commands_phoneme_add()` 只在英文 mn7 下校验音素，中文模型走 grapheme 分支、校验的还是中文串，照样被拒——不要用它。命令 ID = `s_commands[]` 下标，结果按 `res->command_id[0]` 反查中文。
- **命令采信只看输入侧证据（守卫 v4）**：实机日志证明 mn7_cn 对**每一次** DETECTED 都返回 `command_id[0]=1, prob[0]=0, string/raw_string=""`，人说话与纯静音完全一样——**结果串与 prob 不能当判据**（按它们过滤会把真人命令一起挡掉，屏幕恒显示“未听清”）。现行规则：本轮累计的“VAD 判说话且 16 kHz 峰值过门限”帧数 ≥ `VOICE_MN_MIN_SPEECH_FRAMES`(6) 才采信；**一句语音只判定一次**（出结果即把语音帧清零）；静音关闸（`VOICE_MN_GRACE_MS`=1.5 s）时同样清零。调试先看启动打印的 `esp_mn_commands_print()` / `esp_mn_active_commands_print()`（命令 ID ↔ 短语号是否一一对应）与每帧 `命中评估：… 语音帧=… 候选=… 短语=…`。
- **同一句只派发一次（调用侧）**：`mn_step()` 返回 ID 后，调用方 `dispatch_command()` → `reset_engines()` → `enter_state(VOICE_STATE_CMD)`（**留在聆听窗口**，可连说多条）→ `break` 出本帧循环。直接 `continue` 会让同一段音频的后续块再命中一次（说一句触发两遍），本帧剩余采样也会污染刚 reset 的 AFE。
- **命令表（8 条，ID = `s_commands[]` 下标）**：0 打开门禁 / 1 打开门 / 2 查询记录 / 3 注册人脸 / 4 网络状态 / 5 声音大一点 / 6 声音小一点 / 7 取消操作。曾有的“今天有谁来过”已删除（它要读 SPIFFS，而 voice_task 的栈在 PSRAM，碰 Flash 会 `esp_task_stack_is_sane_cache_disabled()` panic）。
- **命令派发不阻塞**：语音“打开门禁/打开门”只等价于按一下主页的“开始识别”——走 `ui_home_recognize_async()`（**不是**直接调 `face_service_recognize_async()`）：前者先把主页状态卡写成“识别中”并启动轮询，用户才看得到反应；直接调服务层屏幕毫无变化，现场就是“说了打开门没反应”。**派发时不要播“正在识别”**：识别只要 ~400 ms，播报那 1 s 会盖住识别结论（结论播报见 `face_task`）。页面切换（查询记录/注册人脸）一律交给 3 KB 内部栈的一次性线程 `switch_page_job`。所有分支必须在毫秒级返回，否则停止读 I2S 会让 RX DMA 溢出、唤醒延迟变大。
- **麦克风声道不可写死**：ES7210 只接 1 路麦，落在 L 还是 R 取决于寄存器格式。启动 `probe_mic_channel()` 只作初值（安静时两路底噪 RMS 几乎相同，实测同一板不同次上电能量差 20 倍——I2S/ES7210 上电瞬态导致），**真正的判定靠运行期 `channel_monitor()`**：累计 1.5 s 的两路平方和/峰值，峰值 ≥2000（确认真在说话）且较响一路 ≥1.6 倍才切换，静默窗口丢弃。切换时 drain + `reset_engines()` 回待唤醒。没有它就会出现“诊断页 RMS 有数、却怎么喊都唤不醒”。
- **任务栈放 PSRAM**：`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)`（需要 `freertos/idf_additions.h` 与 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`，默认开）。WiFi 之后内部最大块约 8.7 KB，8 KB 内部栈靠运气；PSRAM 栈在 Flash 操作期间会随 cache 关闭挂起，本任务不落 Flash，安全（峰值水位约 2 KB）。
- **模型分区**：`model`（0x8D0000，3.5 MB）由 `idf.py flash` 自动写入 `build/srmodels/srmodels.bin`（2.83 MB），启动日志 `模型加载：唤醒=wn9_hilexin，命令词=mn7_cn`。分区缺失/模型名不对时 `esp_srmodel_filter` 返回 NULL，初始化直接失败（只告警，其余功能照常）。
- **诊断页“语音”行**：状态（`待唤醒`/`聆听命令`/`播报中，暂停`/`未就绪`）+ 输入 RMS。语音状态字符串定义在 `voice_assistant.c` 但**必须在 `ui_strings.h` 登记**（`UI_TXT_VOICE_STATE_*`），因为 `gen_font_chars.py` 只扫描 `ui_strings.h`；直接写字面量会导致字库缺字形。
- **字体生成注意**：`font_symbols.txt` 现有 264 个非 ASCII 字符，两份字库 cmap `list_length` 必须一致（263，`°` 不在 AliFangYuanT.ttf 里）。**Git Bash 里 `--symbols "$(cat ...)"` 会被按 GBK 代码页转换**，生成结果是乱码字库（文件头 `Opts:` 一看就知道）；必须用 PowerShell 以 UTF-8 读文件后调用，脚本见 `ui/README_font.md`。

**内部 RAM 是 M4/M5 后最稀缺的资源**（详情见 `sdkconfig.defaults` 注释）：1600 B 静态 RX ×2、1600 B 静态 TX ×2、`ESP_WIFI_MGMT_SBUF_NUM=6`（range 6~32，写更小会被夹回默认 32）、MQTT 客户端创建约 −6.8 KB、识别/录入**首次推理**永久约 −6 KB、摄像头 DMA 中转缓冲 15 KB（`CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX=16384`，半缓冲 7680 B 一次连续申请）。界面工作线程要 3 KB 连续栈（看“最大块”），不足时表现为“点注册/识别没反应”。M5 实测：语音引擎 + MQTT 都起来后内部空闲约 **14 KB、最大块约 4.6 KB**（这就是界面 3 KB 工作线程的余量）；WiFi 后、语音前约 35 KB / 26 KB；进入配网（SoftAP + httpd）后最大块只剩 ~1.8 KB——配网门户期间不要新建需要内部连续栈的任务。改 sdkconfig 后务必 `idf.py reconfigure`，否则 build/config 不更新（曾出现 sdkconfig=6 而 sdkconfig.h=32）。

**SPIFFS 是扁平文件系统**：`mkdir()` 返回 `ENOTSUP`（errno 134），目录只用同名空文件做占位；`readdir()` 的 `d_name` 是带 `/spiffs/` 前缀的完整路径，判断扩展名/排序前要先取 basename，删除时直接用完整路径。

`components/esp-dl/` 是 vendored 的 ESP-DL 2.0.0 源码（模型位于 `include/model_zoo/`，预编译库位于 `lib/esp32s3/*.a`），不属于项目自身代码。其中自带 `examples/`，可作为检测器/识别器 API 的参考。

## 编码约定

- 向 `main/`（含子目录）添加源文件无需改 CMake：`main/CMakeLists.txt` 用 `file(GLOB_RECURSE ... "./*.c" "./*.cpp")` 递归收集。但 GLOB 结果在配置期缓存，**新增源文件后必须执行 `idf.py reconfigure`**（否则链接期报 undefined reference；IDF 组件依赖脚本不允许 `CONFIGURE_DEPENDS`）。**新增头文件目录要加入 `INCLUDE_DIRS`**（当前为 `.`、`font`、`sys`、`ui`、`face`、`net`、`audio`），否则 `#include "xxx.h"` 找不到。
- `.clang-format`：LLVM 基准，4 空格缩进，花括号独占一行（Allman），行宽上限 100。
- 任务栈大小：face_task 16 KB（必须是**连续内部 RAM**）、AI(自检) 8 KB、按键 4 KB（2 KB 会在 `ESP_LOGI` 的 `vsnprintf` 中溢出）、LCD 4 KB、采集 3 KB、voice_task 8 KB（**PSRAM**，理由见 M5 节）、界面一次性工作线程 3~4 KB。
- 大缓冲区（帧缓冲、预览缓冲）必须用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 从 PSRAM 分配；例外是 LVGL 绘制缓冲与 SPI 面板缓冲——它们要与摄像头 PSRAM DMA 抢带宽，必须留在内部 RAM。
- 任务名超过 `configMAX_TASK_NAME_LEN`（默认 16）会被截断，按名字查找任务时用前缀比较。
- `sdkconfig.defaults` 中 WiFi 缓冲裁剪时，`ESP_WIFI_RX_BA_WIN` 不得大于 `ESP_WIFI_STATIC_RX_BUFFER_NUM × 2`，否则 `wifi_init.c` 直接 `#error`。
