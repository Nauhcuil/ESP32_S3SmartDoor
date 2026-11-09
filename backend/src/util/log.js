const SENSITIVE_KEY = /^(uid|pin|adminpin|admin_pin|token|secret|sessionsecret|session_secret|authorization)$/i;
const SECRET_IN_TEXT = [
    /(uid=)[^&\s]+/gi,
    /(token["':=\s]+)[^\s"',}]+/gi,
    /(pin["':=\s]+)[^\s"',}]+/gi
];
function maskText(text) {
    let out = text;
    for (const pattern of SECRET_IN_TEXT){
        out = out.replace(pattern, (_match, prefix)=>`${prefix}***`);
    }
    return out;
}
function sanitize(key, value) {
    if (SENSITIVE_KEY.test(key)) {
        return '***';
    }
    if (typeof value === 'string') {
        return maskText(value.length > 300 ? `${value.slice(0, 300)}…` : value);
    }
    if (typeof value === 'number' || typeof value === 'boolean' || value === null) {
        return value;
    }
    if (Array.isArray(value)) {
        return value.map((item)=>typeof item === 'string' ? maskText(item) : item);
    }
    return value;
}
function emit(level, event, fields) {
    const payload = {
        ts: new Date().toISOString(),
        level,
        event
    };
    if (fields !== undefined) {
        for (const [key, value] of Object.entries(fields)){
            payload[key] = sanitize(key, value);
        }
    }
    const line = JSON.stringify(payload);
    if (level === 'error') {
        console.error(line);
    } else {
        console.log(line);
    }
}
export const log = {
    info: (event, fields)=>emit('info', event, fields),
    warn: (event, fields)=>emit('warn', event, fields),
    error: (event, fields)=>emit('error', event, fields)
};
export function redact(text) {
    return maskText(text);
}
