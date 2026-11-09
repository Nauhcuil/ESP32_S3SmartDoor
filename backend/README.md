# 门禁桥接后端（doorcam-bridge）

微信小程序 ↔ 巴法云 ↔ ESP32 人脸门禁之间的 HTTPS 桥接服务。**纯 JavaScript（ESM）**，零运行时依赖，零开发依赖，不需要 `npm install`、不需要编译。

Node 要求：≥ 20.6（用到 `node --env-file`；若用系统环境变量启动，18 也能跑）。

---

## 1. 为什么不让小程序直连巴法云

巴法云官方提供小程序可用的 WSS MQTT（`wss://bemfa.com:9504/wss`，clientId = 私钥 UID）。本方案没有采用：

| 风险 | 说明 |
|---|---|
| 凭据泄露 | UID 是账号级私钥，打进小程序包等于公开；管理员 PIN 用于开门，更不能放前端。 |
| 会话互踢 | 固件用 UID 当 clientId。小程序用同一 UID 直连，MQTT 规范下同 clientId 会互相踢下线（官方是否放宽未实测，不能拿门禁冒险）。 |
| 鉴权旁路 | 固件只认 `token=admin_pin` 的 JSON 命令，任何“前端代填/纯文本开门”都是绕过。由后端统一注入令牌最稳。 |

因此：**小程序 → 自建 HTTPS 后端 → 巴法云 HTTP 接口 → ESP32**。后端**不建立 MQTT 连接**，只用官方三个 HTTP 接口：

- `GET  /va/online` 查主题在线
- `GET  /va/getmsg` 取主题历史消息（type=1 = MQTT）
- `POST /va/postJsonMsg` 向主题推送（type=1）

这样从根上避免 clientId 冲突，也不需要额外的 appID/secretKey。

> 备选（当前未启用）：后端用“唯一 clientId + appID/secretKey”长连 MQTT。若 P1（HTTP 推送到设备）实测失败，再切这条路径。

---

## 2. 固件协议摘要（全部来自源码，非推测）

依据：`main/net/cloud_service.c/h`、`bemfa_proto.c/h`、`net_config.c/h`、`net_service.c`、`main/sys/access_log.c/h`、`door_controller.c`、`face_service.c`、`app_nvs.c`、`app_config.h`、`prov_http.c`、`ui_settings.c`。

### 2.1 连接与认证

| 项 | 取值 |
|---|---|
| 服务器 | `bemfa.com`，端口 9501（明文）/ 9503（TLS，固件使用 `mqtts://` 并校验系统 CA） |
| clientId | 巴法云私钥 UID（NVS `net_cfg/bemfa_uid`） |
| 用户名/密码 | 留空 |
| keepalive | 60 s，重连 5 s |
| 建连条件 | WiFi 已连 + 内部 RAM 足够；未配 UID 时云端功能整体关闭 |

### 2.2 四个主题与降级映射

NVS `net_cfg/bemfa_topic` 为逗号分隔串，配置顺序是 `f0=ACCESS, f1=CMD, f2=STATUS, f3=STRANGER`：

| 填写个数 | ACCESS | STRANGER | STATUS | CMD |
|---|---|---|---|---|
| 1 | f0 | f0 | f0 | f0 |
| 2 | f0 | f0 | f0 | f1 |
| 3 | f0 | f0 | f2 | f1 |
| 4 | f0 | f3 | f2 | f1 |

空项跳过、首尾空白裁剪；超过 4 个字段只取前 4 个。后端 `src/protocol/topics.js` **逐字复刻**了这张表并有单测，避免两端映射不一致导致的静默失效（发到 A 主题、设备只听 B 主题）。

固件订阅全部非空主题（去重；含 CMD 时按 QoS1 订阅），**只有 CMD 主题的下行消息会被接受**；`>256` 字节或分片的载荷直接丢弃且**不回执**。

### 2.3 上行消息

**记录（QoS1，发到 ACCESS / STRANGER）** —— 固件日志行原文，`fgets` 读取，**末尾带 `\n`**：

```json
{"seq":123,"boot":87,"ts":1762491234567,"iso":"2026-11-07 14:13:54","type":"access","result":"open","reason":"remote","name":"","sim":-1.00,"live":-1.00,"extra":"bemfa"}
```

- `type`: `access` / `stranger` / `occupy`（`status` 类不逐条上报）
- `result`: `open` / `deny` / `deny_liveness` / `info`
- `reason`: `face` / `remote` / `voice` / `liveness` / `unknown`
- `ts` 在 SNTP 成功前是**开机毫秒**（< 1.7e12），`iso` 为空；`seq` 跨重启单调，`boot` 为 8 位随机（0~255）
- 去重键 `boot:seq`；`sim`/`live` 的 -1 表示无数据
- 队列深度 8、间隔 ≥200 ms；**离线时不发布**（云端看不到离线期间事件）

**心跳（QoS0，30 s 一次，STATUS）**：

```json
{"type":"status","seq":123,"ts":1762491234567,"fw":"0.1.0","rssi":-52,"ip":"192.168.1.23","faces":3,"uptime":9876}
```

`faces=-1` 未知；心跳**不含阈值、活体开关、配网状态**。

**回执（QoS0，STATUS）**：`{"type":"ack","cmd":"open","ok":1}` —— 无请求 ID；`ok=0` 统一表示令牌错/限速/执行失败，**不可区分**。

**查询回执（QoS0，STATUS）**：`{"type":"query","seq":N,"items":[最多 3 条日志对象]}`，随后紧跟 `query_log` 的 ack；超过 1024 字节整条不发。

### 2.4 命令（全部需要 token）

| cmd | 额外字段 | 固件校验 |
|---|---|---|
| `open` | — | 令牌 + 60 s 内 ≤3 次 |
| `query_log` | — | 令牌 |
| `set_threshold` | `value` | 令牌 + **严格 0 < value < 1** |
| `set_liveness` | `value` | 令牌 + `value ≥ 0 且 > 0.5` 为开（0/0.5 为关） |
| `reboot` | — | 令牌；先回执后 300 ms 重启 |

- 纯文本命令在固件里一定因 `token` 为空被拒（`ok=0`），所以后端**永远发 JSON**。
- 未知命令/无 `cmd`/无法解析：**静默丢弃**，不回执。
- 设备 UI 的阈值区间是 0.40~0.85、步进 0.05；后端按该口径收紧（固件本身更宽）。

### 2.5 鉴权与限速

- `token` = 管理员 PIN（NVS `doorcam/admin_pin`，默认 1234，1~8 位数字）。
- 开门限速：固件 60 s 滑窗 3 次（RAM，重启清零）；后端再按 `OPEN_LIMIT_PER_MIN` 限一层（默认 3，上限也是 3）。
- 设备离线/未配网：不发布、不补发；命令由后端先查在线，离线直接拒绝（避免“以为开了”）。

### 2.6 仍待实测（不编造）

| 编号 | 待确认 | 当前处理 |
|---|---|---|
| P1 | HTTP `postJsonMsg` 能否直达设备；是否需要发 `/set` | `BEMFA_PUBLISH_SUFFIX` 可切（默认空），`scripts/probe.js --publish` 判定；仍失败则启用备选 MQTT 发布 |
| P2 | `getmsg` 对 MQTT 主题保留多少历史 | 页面文案如实说明，另提供 `query_log` 取设备最近 3 条；不假装分页 |
| P3 | 同 UID 多客户端是否互踢 | 本方案不建 MQTT 连接，规避 |
| P4 | 巴法云 HTTP 频率限制 | 后端固定轮询节奏，页面隐藏即停 |
| P5 | ack 无请求 ID | 命令串行 + 回执时间基线，同秒回执标 `ambiguous` |

---

## 3. 目录

```
backend/
├── package.json / .env.example / .gitignore
├── src/
│   ├── index.js                  # 启动、选 live/mock、优雅退出
│   ├── server.js                 # 路由、鉴权、错误码、16 KB 限长、脱敏日志
│   ├── config.js                 # 环境变量校验（缺项拒绝启动）
│   ├── bemfa/httpClient.js       # online/getmsg/postJsonMsg（超时/错误码/脱敏）
│   ├── bemfa/errors.js
│   ├── protocol/topics.js        # 复刻固件主题降级映射
│   ├── protocol/parse.js         # 日志行/心跳/回执/查询/云端历史解析
│   ├── protocol/dedupe.js        # boot:seq 去重 + 排序 + 有界存储
│   ├── protocol/validate.js      # 命令白名单、阈值范围/步进、256 字节上限
│   ├── auth/session.js           # PIN 常数时间比对、HMAC 会话
│   ├── service/deviceService.js  # 在线/心跳/记录/命令/回执等待/开门限速
│   ├── service/mockService.js    # 演示模式（与真实 transport 完全隔离）
│   ├── util/log.js               # 日志脱敏（uid/pin/token 永不出现）
│   └── util/rateLimiter.js
├── scripts/probe.js              # 真机联调探针（默认只读）
└── test/                         # 7 个测试文件，60 个用例
```

---

## 4. 启动（不需要安装任何依赖）

```powershell
cd D:\YueQianStudy\Class_code\ESP32\04-human_face_recognition\backend
Copy-Item .env.example .env      # 填真实值；.env 已在 .gitignore
node --env-file=.env src/index.js
```

或 `npm start`（不带 .env，用系统环境变量）/ `npm run start:env`。

**演示模式（无需设备、UID、巴法云）**：

```powershell
$env:MOCK='1'
$env:ADMIN_PIN='1234'
$env:SESSION_SECRET='demo-secret-at-least-32-characters-long'
node src/index.js
```

健康检查：`GET http://127.0.0.1:8080/api/health`。

### 环境变量

| 变量 | 必填 | 说明 |
|---|---|---|
| `PORT` | 否 | 默认 8080 |
| `MOCK` | 否 | `1` = 演示模式，不访问巴法云 |
| `BEMFA_UID` | live 必填 | 设备 NVS 里的私钥 |
| `BEMFA_TOPICS` | live 必填 | 与设备配网页**完全一致**，如 `doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger` |
| `BEMFA_PUBLISH_SUFFIX` | 否 | 空 或 `/set`（P1 联调开关） |
| `ADMIN_PIN` | 是 | 与设备 `admin_pin` 一致；仅服务端保存 |
| `SESSION_SECRET` | live 必填 | ≥32 字符，用于会话签名；mock 下可省略（自动随机） |
| `OPEN_LIMIT_PER_MIN` | 否 | 默认 3，最大 3（不得超过固件） |
| `RECORDS_MAX_LIMIT` | 否 | 默认 200，单次拉取上限 |
| `BEMFA_BASE_URL` | 否 | 默认 `https://apis.bemfa.com` |
| `UPSTREAM_TIMEOUT_MS` | 否 | 默认 8000 |
| `SESSION_TTL_MINUTES` | 否 | 默认 30 |

---

## 5. API

除 `GET /api/meta`、`GET /api/health`、`POST /api/session/login` 外都需要 `Authorization: Bearer <token>`。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/meta` | `{mode, serverTime, version, limits, topicsConfigured, demoNotice}` |
| GET | `/api/health` | 存活检查 |
| POST | `/api/session/login` | `{pin}` → `{token, expiresAt}`；失败 5 次/10 分钟封禁 |
| GET | `/api/device` | `{online, lastHeartbeat, heartbeatAgeMs, stale, fetchedAt, upstreamError, message}` |
| GET | `/api/records?limit=20` | `{items, fetched, skipped, cloudWindow, truncated, fetchedAt, notice}` |
| POST | `/api/records/query` | 下发 `query_log`，返回设备最近 ≤3 条 |
| POST | `/api/commands` | `{cmd, value?}`，校验后注入 token 下发并等回执 |

错误体：`{"error":{"code","message"}}`。常见码：`PIN_INVALID`、`UNAUTHORIZED`、`SESSION_EXPIRED`、`UNKNOWN_CMD`、`OUT_OF_RANGE`、`BAD_STEP`、`BAD_VALUE`、`OPEN_RATE_LIMITED`(429)、`DEVICE_OFFLINE`(409)、`BEMFA_TIMEOUT`(504)、`BEMFA_CREDENTIAL`(502)、`BODY_TOO_LARGE`(413)。

命令返回示例：

```json
{"ok":true,"cmd":"open","ackReceived":true,"ackAt":1762491234,"ambiguous":false,"query":null,"message":"设备回执成功"}
```

---

## 6. 安全设计

- UID / PIN / 会话密钥只在服务端环境变量；小程序只有后端地址。
- 小程序登录用 PIN 换 HMAC 会话（默认 30 分钟），令牌**只存内存**，不写 Storage。
- 命令的 `token` 由后端注入；前端无法改 cmd 之外的内容，也无法传自定义 payload。
- 命令白名单 + 阈值范围/步进 + 活体 0/1 + 256 字节上限，全部在服务端复核（不信任小程序）。
- 日志按 key 名脱敏（`uid`/`pin`/`token`/`secret`），上游 URL 只记录 path。
- 请求体 16 KB 上限；超限返回 413 且不粗暴掐断连接；错误响应不回上游原始报文。
- 多实例部署时限速与会话封禁需换共享存储（当前为进程内，单实例足够）。

---

## 7. 测试与联调

```powershell
npm test          # = node --test，60 个用例，无需安装依赖
```

**本机实测（2026-11-07，Node v22.23.2，Windows）**：

- `node --test`：60 个用例，**60 pass / 0 fail**（修复了 413 用例中 `req.destroy()` 导致客户端拿到 `fetch failed` 的问题）。
- MOCK 模式真起服务冒烟：登录 200、`/api/device` 返回 `lastHeartbeat.fw=0.1.0`、`/api/records` 返回 6 条、`/api/records/query` 返回 3 条、无令牌访问 401。
- 限速：第 4 次 `open` 返回 429 `OPEN_RATE_LIMITED`；`set_threshold=0.51` 返回 400 `BAD_STEP`；`clear_all` 返回 400 `UNKNOWN_CMD`；`set_liveness=0.5` 返回 400 `BAD_VALUE`。

真机联调（默认只读，不会开门）：

```powershell
node --env-file=.env scripts/probe.js            # 只读：在线/主题/历史/热点
node --env-file=.env scripts/probe.js --publish  # 只发 query_log，验证 P1
```

探针输出：主题映射、在线状态、ACCESS 历史条数（判断 P2）、STATUS 混合消息分类、命令投递与回执（判断 P1）。失败时会提示切 `/set` 或改用备选 MQTT 方案。

---

## 8. 已知限制（不隐藏）

1. **不做服务端分页**：巴法云 getmsg 没有 offset/cursor，“加载更多”是放大 `num` 重取最近窗口。
2. **离线记录缺失**：固件断网期间不发布，云端无法补发；只能靠 `query_log` 取最近 3 条。
3. **回执归属**：固件 ack 无请求 ID；同秒回执会被标记 `ambiguous`。
4. **boot 8 位 + seq 可重置**：极端情况（擦除 SPIFFS）可能撞键，去重器保留较新记录并计数。
5. TLS 9503 使用 ESP-IDF 系统证书包校验；若系统时间或证书包异常，MQTT 错误日志会显示 TLS/证书错误。
6. **单管理员**：无角色/多账号；门禁记录属敏感数据，因此读接口也要会话。
7. **P1 未实测**：HTTP `postJsonMsg` 是否直达设备仍需用户提供 UID/主题/PIN 后跑探针确认。
