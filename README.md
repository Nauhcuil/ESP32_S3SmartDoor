# ESP32-S3 人脸门禁

基于 ESP-IDF 5.5.x 与 ESP-DL 的端侧人脸门禁示例：ESP32-S3 在本地完成摄像头采集、人脸检测/识别和门禁决策，并提供 LVGL 触摸 UI、离线语音、事件日志、Wi-Fi 配网与巴法云通信。仓库还包含面向小程序类客户端的 Node.js HTTPS 桥接后端。

> 本仓库仍在迭代中。活体检测目前是启发式实现，并非经过认证的专用反欺骗方案；真实门禁部署前请自行进行安全评估与实机验证。默认管理员 PIN 不适合生产使用。

## 功能概览

- GC0308 DVP 摄像头与 ST7796 SPI LCD，LVGL 中文界面。
- ESP-DL 人脸检测、质量筛选、轨迹跟踪、注册与识别；默认手动识别。
- 启发式活体门控、模拟门禁动作、本地 JSONL 日志与陌生人抓拍。
- ES8311/ES7210 I2S 音频，ESP-SR 离线唤醒与命令词、PCM 提示音。
- Wi-Fi STA/SoftAP 配网、SNTP 和巴法云 MQTT。
- `backend/`：零运行时依赖的 Node.js HTTPS bridge，包含鉴权、命令校验、限速、设备状态和记录查询。

## 仓库结构

```text
main/                  ESP-IDF 应用、硬件驱动、UI、人脸/网络/音频模块
components/esp-dl/     vendored ESP-DL 组件源码
backend/               Node.js 巴法云桥接后端及自动化测试
miniapp-direct-tests/  外部小程序服务层的冒烟测试脚本
partitions.csv         自定义 Flash 分区表
sdkconfig.defaults     ESP32-S3 项目默认配置
项目实现与面试指南.md  架构详解、已知限制与面试问答
```

`build/` 和 `managed_components/` 是本机构建输出/下载依赖，不提交到 Git。ESP-IDF 首次构建会根据清单恢复托管依赖。

## 硬件与构建

目标硬件为 ESP32-S3，240 MHz、16 MB Flash，要求启用 OCT 80 MHz PSRAM。工程使用自定义分区：`storage` 保存 ESP-DL 人脸库，`spiffs` 保存日志/抓拍，`model` 保存 ESP-SR 模型。

在 Windows PowerShell 激活 ESP-IDF 5.5.x 环境后，从仓库根目录构建：

```powershell
. 'C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1'
idf.py build
```

如需切换目标或配置缺失：

```powershell
idf.py set-target esp32s3
idf.py build
```

新增源文件后执行 `idf.py reconfigure`，然后再构建。烧录示例：

```powershell
idf.py -p COM14 flash monitor
```

具体引脚、分区、内存配置、启动顺序和排障说明见 [项目实现与面试指南.md](./项目实现与面试指南.md) 与源码注释。设备接线应以对应开发板版本为准。

## 后端测试与演示

Node.js 版本要求为 20.6 或更高；后端使用 Node 内置模块，无需安装依赖。在 `backend/` 下运行测试：

```powershell
npm test
```

Mock 演示模式不连接巴法云，也不会操作真实门禁：

```powershell
$env:MOCK = '1'
$env:ADMIN_PIN = '1234'
$env:SESSION_SECRET = 'demo-secret-at-least-32-characters-long'
node src/index.js
```

真实联调时按 `backend/.env.example` 配置本地 `backend/.env`。**不要提交真实 UID、PIN、Wi-Fi 密码、会话密钥或其他凭据**。探针脚本默认只读；带 `--publish` 会向设备发送查询命令。

## 安全与已知限制

- 请在实际设备上修改默认管理员 PIN，并在后端设置随机的 `SESSION_SECRET`。
- 人脸特征保存在设备本地；启发式活体不能保证抵御所有照片、视频回放或其他攻击。
- 云端 ACK 没有请求 ID，同秒回执可能无法归属到具体命令；断网期间的云端记录也受上游接口能力限制。
- 后端的失败登录计数和限速目前是进程内状态；多实例部署需要共享存储。
- 固件主要通过 ESP-IDF 构建和实机验证；后端有 Node 自动化测试。请勿将演示或 Mock 结果当作真实门锁验证。

## 许可证

本仓库尚未声明项目整体许可证。`components/esp-dl/` 和其他 vendored/托管组件可能有各自许可证；再分发或商用前，请核实相应许可与第三方资产权利。
