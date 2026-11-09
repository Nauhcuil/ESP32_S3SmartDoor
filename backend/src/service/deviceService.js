import { expectBemfaError } from '../bemfa/errors.js';
import { RecordStore } from '../protocol/dedupe.js';
import { parseAck, parseQueryResult, parseUplink } from '../protocol/parse.js';
import { buildCommandPayload, payloadFitsLimit } from '../protocol/validate.js';
import { SlidingWindowLimiter } from '../util/rateLimiter.js';
import { log } from '../util/log.js';
import { ServiceError } from './errors.js';
export const HEARTBEAT_STALE_MS = 90_000;
const ACK_WAIT_MS = 8_000;
const ACK_POLL_MS = 700;
function snapshotMessages(messages) {
    const snapshot = {
        heartbeats: [],
        acks: [],
        queries: [],
        records: [],
        skipped: 0,
        total: messages.length
    };
    for (const message of messages){
        const parsed = parseUplink(message.msg);
        if (!parsed.ok) {
            snapshot.skipped += 1;
            continue;
        }
        switch(parsed.value.kind){
            case 'heartbeat':
                snapshot.heartbeats.push(parsed.value.heartbeat);
                break;
            case 'ack':
                snapshot.acks.push(parsed.value.ack);
                break;
            case 'query':
                snapshot.queries.push(parsed.value.query);
                break;
            case 'log':
                snapshot.records.push(parsed.value.record);
                break;
        }
    }
    return snapshot;
}
function collectAckTiming(messages) {
    const out = [];
    for (const message of messages){
        const parsed = parseAck(message.msg);
        if (parsed.ok) {
            out.push({
                ack: parsed.value,
                unix: message.unix
            });
        }
    }
    return out;
}
export class DeviceService {
    config;
    transport;
    mode = 'live';
    store;
    openLimiter;
    now;
    sleep;
    queue = Promise.resolve();
    constructor(config, transport, deps = {}){
        this.config = config;
        this.transport = transport;
        this.store = new RecordStore(config.recordsMaxLimit);
        this.openLimiter = new SlidingWindowLimiter(config.openLimitPerMinute, 60_000);
        this.now = deps.now ?? (()=>Date.now());
        this.sleep = deps.sleep ?? ((ms)=>new Promise((resolve)=>setTimeout(resolve, ms)));
    }
    async getDeviceStatus() {
        const fetchedAt = this.now();
        const statusTopic = this.config.bemfa.topics.status;
        try {
            const [online, messages] = await Promise.all([
                this.transport.online(this.config.bemfa.topics.cmd || statusTopic),
                statusTopic === '' ? Promise.resolve([]) : this.transport.history(statusTopic, 20)
            ]);
            const snapshot = snapshotMessages(messages);
            const heartbeat = snapshot.heartbeats[0] ?? null;
            const heartbeatAgeMs = this.computeAgeMs(heartbeat, messages);
            let message;
            if (!online) {
                message = '设备离线：检查设备供电、Wi-Fi 与巴法云私钥/主题是否一致';
            } else if (heartbeat === null) {
                message = '设备在线，但云端暂未取到心跳消息（巴法云可能只保留该主题最新消息）';
            } else if (heartbeatAgeMs !== null && heartbeatAgeMs > HEARTBEAT_STALE_MS) {
                message = '设备在线，但心跳已陈旧：可能刚重连或状态上报异常';
            } else {
                message = '设备在线';
            }
            return {
                online,
                lastHeartbeat: heartbeat,
                heartbeatAgeMs,
                stale: heartbeatAgeMs !== null && heartbeatAgeMs > HEARTBEAT_STALE_MS,
                fetchedAt,
                upstreamError: null,
                message
            };
        } catch (error) {
            const code = expectBemfaError(error);
            log.warn('device.status.fail', {
                code: code.code
            });
            return {
                online: false,
                lastHeartbeat: null,
                heartbeatAgeMs: null,
                stale: false,
                fetchedAt,
                upstreamError: code.code,
                message: code.message
            };
        }
    }
    async getRecords(limit) {
        const fetchedAt = this.now();
        const accessTopic = this.config.bemfa.topics.access;
        const strangerTopic = this.config.bemfa.topics.stranger;
        const safeLimit = Math.min(Math.max(Math.trunc(limit), 1), this.config.recordsMaxLimit);
        if (accessTopic === '') {
            return {
                items: [],
                fetched: 0,
                skipped: 0,
                cloudWindow: 0,
                truncated: false,
                fetchedAt,
                notice: '设备未配置门禁事件主题（ACCESS），没有云端记录可读取'
            };
        }
        const topics = accessTopic === strangerTopic ? [
            accessTopic
        ] : [
            accessTopic,
            strangerTopic
        ];
        const results = await Promise.all(topics.filter((topic)=>topic !== '').map((topic)=>this.transport.history(topic, safeLimit)));
        const messages = results.flat();
        const snapshot = snapshotMessages(messages);
        const outcome = this.store.addMany(snapshot.records);
        if (outcome.collisions > 0) {
            log.warn('records.collision', {
                collisions: outcome.collisions
            });
        }
        const items = this.store.list(safeLimit);
        const reachedWindow = results.some((batch)=>batch.length >= safeLimit);
        return {
            items,
            fetched: messages.length,
            skipped: snapshot.skipped,
            cloudWindow: messages.length,
            truncated: reachedWindow,
            fetchedAt,
            notice: reachedWindow || items.length >= safeLimit ? '巴法云只返回该主题最近的消息，更早记录未在此列表中；需要设备本地历史请用“查询设备记录”（固件最多返回最近 3 条）' : null
        };
    }
    async sendCommand(command) {
        const run = ()=>this.executeCommand(command);
        const next = this.queue.then(run, run);
        this.queue = next.then(()=>undefined, ()=>undefined);
        return next;
    }
    async executeCommand(command) {
        if (command.cmd === 'open' && !this.openLimiter.tryAcquire(this.now())) {
            const retryAfter = Math.ceil(this.openLimiter.retryAfterMs(this.now()) / 1000);
            throw new ServiceError('OPEN_RATE_LIMITED', `远程开门触发限速（后端上限 ${this.config.openLimitPerMinute} 次/分钟，固件同样限速 3 次/60 s），请 ${retryAfter} 秒后再试`, 429);
        }
        const payload = buildCommandPayload(command, this.config.adminPin);
        if (!payloadFitsLimit(payload)) {
            throw new ServiceError('COMMAND_TOO_LARGE', '命令载荷超过固件 256 字节上限，已被后端拦截', 400);
        }
        const statusTopic = this.config.bemfa.topics.status;
        const cmdTopic = this.config.bemfa.topics.cmd;
        if (statusTopic === '' || cmdTopic === '') {
            throw new ServiceError('DEVICE_OFFLINE', '主题未配置完整（缺少 CMD 或 STATUS），无法下发命令', 409);
        }
        const online = await this.transport.online(cmdTopic);
        if (!online) {
            throw new ServiceError('DEVICE_OFFLINE', '设备当前离线，命令未下发（巴法云不会把离线期间的消息补发给设备）', 409);
        }
        const before = await this.transport.history(statusTopic, 10);
        const beforeAcks = collectAckTiming(before).filter((item)=>item.ack.cmd === command.cmd);
        const baselineUnix = beforeAcks.length > 0 ? Math.max(...beforeAcks.map((item)=>item.unix)) : 0;
        const sentAt = this.now();
        const sentUnix = Math.floor(sentAt / 1000);
        await this.transport.publish(cmdTopic, payload);
        log.info('command.sent', {
            cmd: command.cmd,
            hasValue: command.value !== undefined,
            payloadBytes: Buffer.byteLength(payload, 'utf8'),
            topic: this.transport.publishTarget(cmdTopic)
        });
        let ackAt = null;
        let ackOk = false;
        let ackReceived = false;
        let ambiguous = false;
        let query = null;
        let querySkipped = 0;
        const deadline = sentAt + ACK_WAIT_MS;
        while(this.now() < deadline){
            await this.sleep(ACK_POLL_MS);
            const after = await this.transport.history(statusTopic, 20);
            const acks = collectAckTiming(after).filter((item)=>item.ack.cmd === command.cmd && item.unix >= sentUnix - 1);
            if (!ackReceived && acks.length > 0) {
                const candidate = acks.sort((a, b)=>a.unix - b.unix)[0];
                ackReceived = true;
                ackOk = candidate.ack.ok;
                ackAt = candidate.unix;
                ambiguous = candidate.unix <= Math.max(baselineUnix, sentUnix);
            }
            if (command.cmd === 'query_log' && query === null) {
                for (const message of after){
                    if (message.unix < sentUnix - 1) {
                        continue;
                    }
                    const parsed = parseQueryResult(message.msg);
                    if (parsed.ok) {
                        query = parsed.value;
                        querySkipped = parsed.value.skipped;
                        break;
                    }
                }
            }
            if (ackReceived && (command.cmd !== 'query_log' || query !== null)) {
                break;
            }
        }
        if (!ackReceived && command.cmd === 'query_log' && query !== null) {}
        let message;
        if (!ackReceived) {
            message = '命令已下发，但 8 秒内未收到设备回执：设备可能刚离线、主题不匹配，或消息被固件静默丢弃（非 CMD 主题/分片/超过 256 字节均不回执）';
        } else if (!ackOk) {
            message = '设备回执 ok=0：固件不区分“管理员令牌不匹配 / 开门限速 / 执行失败”，请核对设备端 admin_pin 与后端 ADMIN_PIN，并避免频繁开门';
        } else if (ambiguous) {
            message = '设备回执成功，但回执时间与上一条命令相同秒，无法排除是上一条命令的回执';
        } else {
            message = '设备回执成功';
        }
        if (query !== null && querySkipped > 0) {
            message += `（查询结果中 ${querySkipped} 条格式异常已跳过）`;
        }
        return {
            ok: ackReceived && ackOk,
            cmd: command.cmd,
            ...command.value !== undefined ? {
                value: command.value
            } : {},
            ackReceived,
            ackAt,
            ambiguous,
            query,
            message
        };
    }
    computeAgeMs(heartbeat, messages) {
        if (heartbeat !== null && heartbeat.tsValid && heartbeat.ts !== null) {
            return Math.max(0, this.now() - heartbeat.ts);
        }
        if (messages.length > 0) {
            const newest = Math.max(...messages.map((item)=>item.unix));
            return Math.max(0, this.now() - newest * 1000);
        }
        return null;
    }
}
