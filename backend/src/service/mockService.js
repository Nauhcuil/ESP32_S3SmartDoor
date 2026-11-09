import { SlidingWindowLimiter } from '../util/rateLimiter.js';
import { ServiceError } from './errors.js';
const MOCK_BOOT = 42;
function record(seq, ts, type, result, reason, name, sim, live, extra = '') {
    return {
        boot: MOCK_BOOT,
        seq,
        ts,
        tsValid: true,
        iso: new Date(ts).toISOString().slice(0, 19).replace('T', ' '),
        type,
        result,
        reason,
        name,
        sim,
        live,
        extra,
        key: `${MOCK_BOOT}:${seq}`
    };
}
export class MockService {
    mode = 'mock';
    records;
    seq = 1200;
    openLimiter;
    now;
    constructor(config, deps = {}){
        this.now = deps.now ?? (()=>Date.now());
        this.openLimiter = new SlidingWindowLimiter(config.openLimitPerMinute, 60_000);
        const base = this.now();
        this.records = [
            record(this.seq - 5, base - 12 * 60_000, 'stranger', 'info', 'unknown', '', -1, -1, 'snap_mock_01.jpg'),
            record(this.seq - 4, base - 40 * 60_000, 'access', 'open', 'remote', '', -1, -1, 'bemfa'),
            record(this.seq - 3, base - 55 * 60_000, 'access', 'deny', 'face', '', 0.31, 0.72, ''),
            record(this.seq - 2, base - 80 * 60_000, 'access', 'open', 'face', '张三', 0.93, 0.88, ''),
            record(this.seq - 1, base - 95 * 60_000, 'access', 'deny_liveness', 'liveness', '李四', 0.88, 0.21, ''),
            record(this.seq, base - 120 * 60_000, 'access', 'open', 'face', '王五', 0.9, 0.81, '')
        ];
    }
    async getDeviceStatus() {
        const now = this.now();
        return {
            online: true,
            lastHeartbeat: {
                seq: this.seq,
                ts: now,
                tsValid: true,
                fw: '0.1.0',
                rssi: -58,
                ip: '192.168.1.66',
                faces: 3,
                uptime: 3600
            },
            heartbeatAgeMs: 0,
            stale: false,
            fetchedAt: now,
            upstreamError: null,
            message: '演示模式：以下为模拟设备，与真实巴法云完全隔离'
        };
    }
    async getRecords(limit) {
        const sorted = this.records.slice().sort((a, b)=>b.seq - a.seq).slice(0, limit);
        return {
            items: sorted,
            fetched: sorted.length,
            skipped: 0,
            cloudWindow: sorted.length,
            truncated: sorted.length >= limit,
            fetchedAt: this.now(),
            notice: '演示数据：不代表真实门禁记录'
        };
    }
    async sendCommand(command) {
        const now = this.now();
        if (command.cmd === 'open') {
            if (!this.openLimiter.tryAcquire(now)) {
                throw new ServiceError('OPEN_RATE_LIMITED', '演示模式：开门限速已触发，请稍后再试', 429);
            }
            this.seq += 1;
            this.records.push(record(this.seq, now, 'access', 'open', 'remote', '', -1, -1, 'bemfa'));
        }
        const query = command.cmd === 'query_log' ? {
            seq: this.seq,
            items: this.records.slice().sort((a, b)=>b.seq - a.seq).slice(0, 3),
            skipped: 0
        } : null;
        return {
            ok: true,
            cmd: command.cmd,
            ...command.value !== undefined ? {
                value: command.value
            } : {},
            ackReceived: true,
            ackAt: Math.floor(now / 1000),
            ambiguous: false,
            query,
            message: '演示模式：命令未真正发送到设备'
        };
    }
}
