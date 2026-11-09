import { parseCloudHistory } from '../protocol/parse.js';
import { log } from '../util/log.js';
export class BemfaError extends Error {
    code;
    upstreamCode;
    constructor(code, message, upstreamCode = null){
        super(message), this.code = code, this.upstreamCode = upstreamCode;
        this.name = 'BemfaError';
    }
}
const CREDENTIAL_CODES = new Set([
    40004,
    40005,
    40007
]);
function upstreamToError(payload, httpStatus) {
    const code = typeof payload.code === 'number' ? payload.code : null;
    const message = typeof payload.message === 'string' ? payload.message : '';
    if (code === 0) {
        return new BemfaError('BEMFA_UPSTREAM', message, 0);
    }
    if (code !== null && CREDENTIAL_CODES.has(code)) {
        return new BemfaError('BEMFA_CREDENTIAL', `巴法云返回 ${code}：私钥或主题不正确（也可能是主题不属于该账号）`, code);
    }
    if (code === 10002) {
        return new BemfaError('BEMFA_BAD_REQUEST', `巴法云参数错误（10002）${message ? `：${message}` : ''}`, code);
    }
    return new BemfaError('BEMFA_UPSTREAM', `巴法云返回 ${code ?? `HTTP ${httpStatus}`}${message ? `：${message}` : ''}`, code);
}
export class BemfaHttpTransport {
    options;
    publishSuffix;
    constructor(options){
        this.options = options;
        this.publishSuffix = options.publishSuffix ?? '';
    }
    publishTarget(topic) {
        return `${topic}${this.publishSuffix}`;
    }
    async online(topic) {
        const url = this.buildUrl('/va/online', {
            topic
        });
        const data = await this.request('GET', url);
        if (typeof data === 'boolean') {
            return data;
        }
        if (typeof data === 'number') {
            return data !== 0;
        }
        if (typeof data === 'string') {
            return data.trim().toLowerCase() === 'true' || data.trim() === '1';
        }
        return false;
    }
    async history(topic, num) {
        const safeNum = Math.min(Math.max(Math.trunc(num), 1), 5000);
        const url = this.buildUrl('/va/getmsg', {
            topic,
            num: String(safeNum)
        });
        const data = await this.request('GET', url);
        return parseCloudHistory(data);
    }
    async publish(topic, msg) {
        const target = this.publishTarget(topic);
        const body = {
            uid: this.options.uid,
            topic: target,
            type: 1,
            msg
        };
        await this.request('POST', this.buildUrl('/va/postJsonMsg', {}), body, target);
    }
    buildUrl(path, params) {
        const search = new URLSearchParams({
            uid: this.options.uid,
            type: '1',
            ...params
        });
        return `${this.options.baseUrl}${path}?${search.toString()}`;
    }
    async request(method, url, body, logTopic) {
        const controller = new AbortController();
        const timer = setTimeout(()=>controller.abort(), this.options.timeoutMs);
        const path = url.split('?')[0] ?? url;
        try {
            const response = await fetch(url, {
                method,
                headers: body === undefined ? {
                    accept: 'application/json'
                } : {
                    'content-type': 'application/json',
                    accept: 'application/json'
                },
                body: body === undefined ? undefined : JSON.stringify(body),
                signal: controller.signal
            });
            let payload;
            try {
                payload = await response.json();
            } catch  {
                throw new BemfaError('BEMFA_UPSTREAM', `巴法云返回了非 JSON 响应（HTTP ${response.status}）`, null);
            }
            if (!response.ok) {
                throw upstreamToError(payload, response.status);
            }
            if (payload.code !== 0) {
                throw upstreamToError(payload, response.status);
            }
            log.info('bemfa.ok', {
                method,
                path,
                topic: logTopic ?? null,
                httpStatus: response.status
            });
            return payload.data;
        } catch (error) {
            if (error instanceof BemfaError) {
                log.warn('bemfa.fail', {
                    method,
                    path,
                    code: error.code,
                    upstreamCode: error.upstreamCode
                });
                throw error;
            }
            if (error instanceof Error && error.name === 'AbortError') {
                log.warn('bemfa.timeout', {
                    method,
                    path
                });
                throw new BemfaError('BEMFA_TIMEOUT', `请求巴法云超时（>${this.options.timeoutMs} ms）`);
            }
            log.warn('bemfa.unreachable', {
                method,
                path,
                reason: error instanceof Error ? error.message : 'unknown'
            });
            throw new BemfaError('BEMFA_UNREACHABLE', `无法连接巴法云：${error instanceof Error ? error.message : '未知网络错误'}`);
        } finally{
            clearTimeout(timer);
        }
    }
}
