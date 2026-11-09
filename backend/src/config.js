import { randomBytes } from 'node:crypto';
import { resolveTopics } from './protocol/topics.js';
export class ConfigError extends Error {
    constructor(message){
        super(message);
        this.name = 'ConfigError';
    }
}
function readInt(raw, fallback, name, min, max) {
    if (raw === undefined || raw.trim() === '') {
        return fallback;
    }
    const value = Number(raw);
    if (!Number.isInteger(value) || value < min || value > max) {
        throw new ConfigError(`${name} 必须是 ${min}~${max} 的整数，当前：${raw}`);
    }
    return value;
}
export function loadConfig(env = process.env) {
    const mock = env.MOCK === '1' || env.MOCK?.toLowerCase() === 'true';
    const adminPin = (env.ADMIN_PIN ?? '').trim();
    if (!/^\d{1,8}$/.test(adminPin)) {
        throw new ConfigError('ADMIN_PIN 必须是 1~8 位数字，且要与设备 NVS 里的 admin_pin 一致');
    }
    const sessionSecretRaw = (env.SESSION_SECRET ?? '').trim();
    if (sessionSecretRaw.length < 32) {
        if (!mock) {
            throw new ConfigError('SESSION_SECRET 至少 32 个字符（可用 openssl rand -hex 32 生成）');
        }
    }
    const sessionSecret = sessionSecretRaw.length >= 32 ? sessionSecretRaw : randomBytes(32).toString('hex');
    const topicsCsv = (env.BEMFA_TOPICS ?? '').trim();
    const topics = resolveTopics(topicsCsv);
    const uid = (env.BEMFA_UID ?? '').trim();
    if (!mock) {
        if (uid.length === 0) {
            throw new ConfigError('BEMFA_UID 未配置：请填设备 NVS 里 net_cfg/bemfa_uid 的私钥');
        }
        if (topics.fieldCount === 0) {
            throw new ConfigError('BEMFA_TOPICS 未配置：请与设备配网页的主题串完全一致');
        }
        if (topics.cmd.length === 0) {
            throw new ConfigError('BEMFA_TOPICS 解析后没有命令主题，远程命令无法下发');
        }
        if (topics.status.length === 0) {
            throw new ConfigError('BEMFA_TOPICS 解析后没有状态主题，无法读取心跳与回执');
        }
    }
    const publishSuffix = (env.BEMFA_PUBLISH_SUFFIX ?? '').trim();
    if (publishSuffix !== '' && publishSuffix !== '/set') {
        throw new ConfigError('BEMFA_PUBLISH_SUFFIX 只允许留空或 /set');
    }
    const baseUrl = (env.BEMFA_BASE_URL ?? 'https://apis.bemfa.com').trim().replace(/\/+$/, '');
    if (!/^https?:\/\//.test(baseUrl)) {
        throw new ConfigError('BEMFA_BASE_URL 必须是 http(s) 地址');
    }
    return {
        port: readInt(env.PORT, 8080, 'PORT', 1, 65535),
        mock,
        bemfa: {
            uid,
            topics,
            topicsCsv,
            publishSuffix,
            baseUrl,
            timeoutMs: readInt(env.UPSTREAM_TIMEOUT_MS, 8000, 'UPSTREAM_TIMEOUT_MS', 1000, 30000)
        },
        adminPin,
        sessionSecret,
        sessionTtlMs: readInt(env.SESSION_TTL_MINUTES, 30, 'SESSION_TTL_MINUTES', 1, 24 * 60) * 60_000,
        openLimitPerMinute: readInt(env.OPEN_LIMIT_PER_MIN, 3, 'OPEN_LIMIT_PER_MIN', 1, 3),
        recordsMaxLimit: readInt(env.RECORDS_MAX_LIMIT, 200, 'RECORDS_MAX_LIMIT', 1, 200)
    };
}
