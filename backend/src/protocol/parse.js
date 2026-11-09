export const EPOCH_MS_MIN = 1_700_000_000_000;
function fail(error) {
    return {
        ok: false,
        error
    };
}
export function toJsonObject(input) {
    if (typeof input === 'string') {
        const trimmed = input.trim();
        if (trimmed.length === 0) {
            return null;
        }
        try {
            const parsed = JSON.parse(trimmed);
            return isObject(parsed) ? parsed : null;
        } catch  {
            return null;
        }
    }
    return isObject(input) ? input : null;
}
function isObject(value) {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}
function asNumber(value) {
    if (typeof value === 'number' && Number.isFinite(value)) {
        return value;
    }
    if (typeof value === 'string' && value.trim() !== '') {
        const n = Number(value);
        return Number.isFinite(n) ? n : null;
    }
    return null;
}
function asInt(value) {
    const n = asNumber(value);
    if (n === null || !Number.isInteger(n)) {
        return null;
    }
    return n;
}
function asString(value) {
    return typeof value === 'string' ? value : '';
}
export function parseAccessRecord(input) {
    const obj = toJsonObject(input);
    if (obj === null) {
        return fail('BAD_JSON');
    }
    const seq = asInt(obj.seq);
    if (seq === null || seq < 0) {
        return fail('MISSING_SEQ');
    }
    const type = asString(obj.type);
    if (type.length === 0) {
        return fail('MISSING_TYPE');
    }
    const bootRaw = asInt(obj.boot);
    const boot = bootRaw !== null && bootRaw >= 0 && bootRaw <= 255 ? bootRaw : null;
    const tsRaw = asNumber(obj.ts);
    const ts = tsRaw !== null && tsRaw > 0 ? tsRaw : null;
    const tsValid = ts !== null && ts >= EPOCH_MS_MIN;
    const simRaw = asNumber(obj.sim);
    const liveRaw = asNumber(obj.live);
    return {
        ok: true,
        value: {
            boot,
            seq,
            ts,
            tsValid,
            iso: asString(obj.iso),
            type,
            result: asString(obj.result),
            reason: asString(obj.reason),
            name: asString(obj.name),
            sim: simRaw,
            live: liveRaw,
            extra: asString(obj.extra),
            key: `${boot === null ? 'x' : boot}:${seq}`
        }
    };
}
export function parseHeartbeat(input) {
    const obj = toJsonObject(input);
    if (obj === null) {
        return fail('BAD_JSON');
    }
    if (asString(obj.type) !== 'status') {
        return fail('NOT_HEARTBEAT');
    }
    const seqRaw = asInt(obj.seq);
    const tsRaw = asNumber(obj.ts);
    const uptime = asInt(obj.uptime);
    return {
        ok: true,
        value: {
            seq: seqRaw !== null && seqRaw >= 0 ? seqRaw : null,
            ts: tsRaw !== null && tsRaw > 0 ? tsRaw : null,
            tsValid: tsRaw !== null && tsRaw >= EPOCH_MS_MIN,
            fw: asString(obj.fw),
            rssi: asInt(obj.rssi),
            ip: asString(obj.ip),
            faces: asInt(obj.faces),
            uptime: uptime !== null && uptime >= 0 ? uptime : null
        }
    };
}
export function parseAck(input) {
    const obj = toJsonObject(input);
    if (obj === null) {
        return fail('BAD_JSON');
    }
    if (asString(obj.type) !== 'ack') {
        return fail('NOT_ACK');
    }
    const cmd = asString(obj.cmd);
    if (cmd.length === 0) {
        return fail('MISSING_CMD');
    }
    const okRaw = obj.ok;
    const ok = okRaw === true || okRaw === 1 || typeof okRaw === 'string' && okRaw.trim() === '1';
    return {
        ok: true,
        value: {
            cmd,
            ok
        }
    };
}
export function parseQueryResult(input) {
    const obj = toJsonObject(input);
    if (obj === null) {
        return fail('BAD_JSON');
    }
    if (asString(obj.type) !== 'query') {
        return fail('NOT_QUERY');
    }
    const seqRaw = asInt(obj.seq);
    const rawItems = Array.isArray(obj.items) ? obj.items : [];
    const items = [];
    let skipped = 0;
    for (const raw of rawItems){
        const parsed = parseAccessRecord(raw);
        if (parsed.ok) {
            items.push(parsed.value);
        } else {
            skipped += 1;
        }
    }
    return {
        ok: true,
        value: {
            seq: seqRaw !== null && seqRaw >= 0 ? seqRaw : null,
            items,
            skipped
        }
    };
}
export function parseUplink(input) {
    const obj = toJsonObject(input);
    if (obj === null) {
        return fail('BAD_JSON');
    }
    const type = asString(obj.type);
    if (type === 'ack') {
        const ack = parseAck(obj);
        return ack.ok ? {
            ok: true,
            value: {
                kind: 'ack',
                ack: ack.value
            }
        } : ack;
    }
    if (type === 'query') {
        const query = parseQueryResult(obj);
        return query.ok ? {
            ok: true,
            value: {
                kind: 'query',
                query: query.value
            }
        } : query;
    }
    if (type === 'status' && (obj.fw !== undefined || obj.uptime !== undefined)) {
        const hb = parseHeartbeat(obj);
        return hb.ok ? {
            ok: true,
            value: {
                kind: 'heartbeat',
                heartbeat: hb.value
            }
        } : hb;
    }
    const record = parseAccessRecord(obj);
    return record.ok ? {
        ok: true,
        value: {
            kind: 'log',
            record: record.value
        }
    } : record;
}
export function parseCloudHistory(data) {
    const list = Array.isArray(data) ? data : isObject(data) ? [
        data
    ] : [];
    const out = [];
    for (const item of list){
        if (!isObject(item)) {
            continue;
        }
        const msg = asString(item.msg);
        const unix = asInt(item.unix);
        if (msg.length === 0 || unix === null) {
            continue;
        }
        out.push({
            msg,
            time: asString(item.time),
            unix
        });
    }
    out.sort((a, b)=>b.unix - a.unix);
    return out;
}
