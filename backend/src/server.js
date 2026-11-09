import { createServer } from 'node:http';
import { verifyPin, createSessionToken, verifySessionToken } from './auth/session.js';
import { expectBemfaError } from './bemfa/errors.js';
import { BemfaError } from './bemfa/httpClient.js';
import { resolveTopics } from './protocol/topics.js';
import { validateCommand } from './protocol/validate.js';
import { ServiceError } from './service/errors.js';
import { FailureLimiter } from './util/rateLimiter.js';
import { log, redact } from './util/log.js';
const MAX_BODY_BYTES = 16 * 1024;
const LOGIN_MAX_FAILURES = 5;
const LOGIN_WINDOW_MS = 10 * 60_000;
const CORS_HEADERS = {
    'access-control-allow-origin': '*',
    'access-control-allow-methods': 'GET,POST,OPTIONS',
    'access-control-allow-headers': 'content-type,authorization',
    'access-control-max-age': '86400'
};
class HttpError extends Error {
    status;
    code;
    constructor(status, code, message){
        super(message), this.status = status, this.code = code;
        this.name = 'HttpError';
    }
}
function sendJson(res, status, body) {
    const payload = JSON.stringify(body);
    res.writeHead(status, {
        ...CORS_HEADERS,
        'content-type': 'application/json; charset=utf-8',
        'content-length': Buffer.byteLength(payload, 'utf8')
    });
    res.end(payload);
}
function sendError(res, status, code, message) {
    sendJson(res, status, {
        error: {
            code,
            message
        }
    });
}
function readJsonBody(req) {
    return new Promise((resolve, reject)=>{
        const chunks = [];
        let size = 0;
        let settled = false;
        req.on('data', (chunk)=>{
            if (settled) {
                return;
            }
            size += chunk.length;
            if (size > MAX_BODY_BYTES) {
                settled = true;
                chunks.length = 0;
                req.pause();
                reject(new HttpError(413, 'BODY_TOO_LARGE', '请求体超过 16 KB 上限'));
                return;
            }
            chunks.push(chunk);
        });
        req.on('end', ()=>{
            if (settled) {
                return;
            }
            settled = true;
            const raw = Buffer.concat(chunks).toString('utf8').trim();
            if (raw.length === 0) {
                resolve({});
                return;
            }
            try {
                resolve(JSON.parse(raw));
            } catch  {
                reject(new HttpError(400, 'BAD_JSON', '请求体不是合法 JSON'));
            }
        });
        req.on('error', ()=>{
            if (settled) {
                return;
            }
            settled = true;
            reject(new HttpError(400, 'BAD_REQUEST', '读取请求体失败'));
        });
    });
}
function clientKey(req) {
    return req.socket.remoteAddress ?? 'unknown';
}
function clampLimit(raw, max, fallback) {
    if (raw === null) {
        return fallback;
    }
    const value = Number(raw);
    if (!Number.isInteger(value) || value < 1) {
        return fallback;
    }
    return Math.min(value, max);
}
export function createBridgeServer(config, service, deps = {}) {
    const now = deps.now ?? (()=>Date.now());
    const loginLimiter = new FailureLimiter(LOGIN_MAX_FAILURES, LOGIN_WINDOW_MS);
    const server = createServer((req, res)=>{
        const startedAt = now();
        const method = req.method ?? 'GET';
        const rawUrl = req.url ?? '/';
        const path = rawUrl.split('?')[0] ?? '/';
        res.on('finish', ()=>{
            log.info('http', {
                method,
                path,
                status: res.statusCode,
                ms: now() - startedAt
            });
        });
        void handle(req, res, method, path, rawUrl).catch((error)=>{
            if (error instanceof HttpError) {
                sendError(res, error.status, error.code, error.message);
                return;
            }
            if (error instanceof ServiceError) {
                sendError(res, error.httpStatus, error.code, error.message);
                return;
            }
            if (error instanceof BemfaError) {
                sendError(res, mapBemfaStatus(error), error.code, redact(error.message));
                return;
            }
            log.error('http.unhandled', {
                path,
                message: error instanceof Error ? error.message : 'unknown'
            });
            sendError(res, 500, 'INTERNAL_ERROR', '后端内部错误，请查看服务端日志');
        });
    });
    async function handle(req, res, method, path, rawUrl) {
        if (method === 'OPTIONS') {
            res.writeHead(204, CORS_HEADERS);
            res.end();
            return;
        }
        if (method === 'GET' && (path === '/api/health' || path === '/healthz')) {
            sendJson(res, 200, {
                ok: true,
                mode: service.mode,
                serverTime: now()
            });
            return;
        }
        if (method === 'GET' && path === '/api/meta') {
            sendJson(res, 200, {
                mode: service.mode,
                serverTime: now(),
                version: '0.1.0',
                limits: {
                    openPerMinute: config.openLimitPerMinute,
                    recordsMaxLimit: config.recordsMaxLimit,
                    sessionTtlMinutes: Math.round(config.sessionTtlMs / 60_000)
                },
                topicsConfigured: resolveTopics(config.bemfa.topicsCsv).fieldCount,
                demoNotice: service.mode === 'mock' ? '演示模式：所有数据为模拟数据，不会访问巴法云，也不会操作真实门禁' : null
            });
            return;
        }
        if (method === 'POST' && path === '/api/session/login') {
            const key = clientKey(req);
            if (loginLimiter.isBlocked(key, now())) {
                throw new HttpError(429, 'LOGIN_BLOCKED', 'PIN 尝试次数过多，请 10 分钟后再试');
            }
            const body = await readJsonBody(req);
            const pin = typeof body === 'object' && body !== null ? body.pin : undefined;
            if (typeof pin !== 'string' || !/^\d{1,8}$/.test(pin.trim())) {
                loginLimiter.recordFailure(key, now());
                throw new HttpError(401, 'PIN_INVALID', 'PIN 必须是 1~8 位数字');
            }
            if (!verifyPin(pin.trim(), config.adminPin)) {
                loginLimiter.recordFailure(key, now());
                throw new HttpError(401, 'PIN_INVALID', 'PIN 不正确');
            }
            loginLimiter.reset(key);
            const session = createSessionToken(config.sessionSecret, config.sessionTtlMs, now());
            log.info('auth.login.ok', {
                expiresAt: session.expiresAt
            });
            sendJson(res, 200, {
                token: session.token,
                expiresAt: session.expiresAt
            });
            return;
        }
        const token = readBearer(req);
        if (token === null) {
            throw new HttpError(401, 'UNAUTHORIZED', '缺少会话令牌，请先输入管理员 PIN 登录');
        }
        const session = verifySessionToken(token, config.sessionSecret, now());
        if (!session.ok) {
            throw new HttpError(401, session.reason === 'EXPIRED' ? 'SESSION_EXPIRED' : 'UNAUTHORIZED', session.reason === 'EXPIRED' ? '会话已过期，请重新输入管理员 PIN' : '会话令牌无效，请重新登录');
        }
        if (method === 'GET' && path === '/api/device') {
            sendJson(res, 200, await service.getDeviceStatus());
            return;
        }
        if (method === 'GET' && path === '/api/records') {
            const limit = clampLimit(new URL(rawUrl, 'http://localhost').searchParams.get('limit'), config.recordsMaxLimit, 20);
            sendJson(res, 200, await service.getRecords(limit));
            return;
        }
        if (method === 'POST' && path === '/api/records/query') {
            const result = await service.sendCommand({
                cmd: 'query_log'
            });
            sendJson(res, 200, {
                items: result.query?.items ?? [],
                skipped: result.query?.skipped ?? 0,
                ackReceived: result.ackReceived,
                ok: result.ok,
                message: result.message,
                querySeq: result.query?.seq ?? null
            });
            return;
        }
        if (method === 'POST' && path === '/api/commands') {
            const body = await readJsonBody(req);
            const validation = validateCommand(body);
            if (!validation.ok) {
                throw new HttpError(400, validation.code, validation.message);
            }
            const result = await service.sendCommand(validation.command);
            sendJson(res, 200, result);
            return;
        }
        throw new HttpError(404, 'NOT_FOUND', `接口不存在：${method} ${path}`);
    }
    return {
        config,
        service,
        listen (port, host = '0.0.0.0') {
            return new Promise((resolve, reject)=>{
                server.once('error', reject);
                server.listen(port, host, ()=>{
                    server.removeListener('error', reject);
                    const address = server.address();
                    resolve(typeof address === 'object' && address !== null ? address.port : port);
                });
            });
        },
        close () {
            return new Promise((resolve, reject)=>{
                server.close((error)=>error !== undefined ? reject(error) : resolve());
            });
        }
    };
}
function readBearer(req) {
    const header = req.headers.authorization;
    if (typeof header !== 'string') {
        return null;
    }
    const match = /^Bearer\s+(.+)$/i.exec(header.trim());
    return match === null ? null : match[1].trim();
}
function mapBemfaStatus(error) {
    if (error.code === 'BEMFA_TIMEOUT') {
        return 504;
    }
    if (error.code === 'BEMFA_CREDENTIAL') {
        return 502;
    }
    if (error.code === 'BEMFA_NO_TOPIC') {
        return 500;
    }
    return 502;
}
