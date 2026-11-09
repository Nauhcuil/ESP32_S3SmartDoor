export const COMMAND_NAMES = [
    'open',
    'query_log',
    'set_threshold',
    'set_liveness',
    'reboot'
];
export const MAX_PAYLOAD_BYTES = 256;
export const THRESHOLD_MIN = 0.4;
export const THRESHOLD_MAX = 0.85;
export const THRESHOLD_STEP = 0.05;
function reject(code, message) {
    return {
        ok: false,
        code,
        message
    };
}
function isCommandName(value) {
    return COMMAND_NAMES.includes(value);
}
function isStepAligned(value, step) {
    const quotient = value / step;
    return Math.abs(quotient - Math.round(quotient)) < 1e-9;
}
export function validateCommand(input) {
    if (typeof input !== 'object' || input === null || Array.isArray(input)) {
        return reject('BAD_BODY', '请求体必须是 JSON 对象');
    }
    const body = input;
    const rawCmd = body.cmd;
    if (typeof rawCmd !== 'string' || rawCmd.trim().length === 0) {
        return reject('MISSING_CMD', '缺少命令名 cmd');
    }
    const normalized = rawCmd.trim().toLowerCase();
    if (!isCommandName(normalized)) {
        return reject('UNKNOWN_CMD', `固件不支持命令「${rawCmd.trim()}」`);
    }
    const hasValue = body.value !== undefined && body.value !== null;
    const rawValue = body.value;
    if (normalized === 'set_threshold') {
        if (!hasValue) {
            return reject('MISSING_VALUE', 'set_threshold 需要 value（0.40~0.85，步进 0.05）');
        }
        if (typeof rawValue !== 'number' || !Number.isFinite(rawValue)) {
            return reject('BAD_VALUE', 'value 必须是数字');
        }
        if (!isStepAligned(rawValue, THRESHOLD_STEP)) {
            return reject('BAD_STEP', '阈值必须按 0.05 步进（0.40、0.45 … 0.85）');
        }
        if (rawValue < THRESHOLD_MIN || rawValue > THRESHOLD_MAX) {
            return reject('OUT_OF_RANGE', '阈值必须在 0.40~0.85 之间');
        }
        if (!(rawValue > 0 && rawValue < 1)) {
            return reject('OUT_OF_RANGE', '固件要求阈值严格大于 0 且小于 1');
        }
        return {
            ok: true,
            command: {
                cmd: 'set_threshold',
                value: Number(rawValue.toFixed(2))
            }
        };
    }
    if (normalized === 'set_liveness') {
        if (!hasValue) {
            return reject('MISSING_VALUE', 'set_liveness 需要 value（0 或 1）');
        }
        if (typeof rawValue !== 'number' || !Number.isFinite(rawValue)) {
            return reject('BAD_VALUE', 'value 必须是数字');
        }
        if (rawValue !== 0 && rawValue !== 1) {
            return reject('BAD_VALUE', '活体检测只接受 0（关）或 1（开）；固件对 0.5 的判定是“关”');
        }
        return {
            ok: true,
            command: {
                cmd: 'set_liveness',
                value: rawValue
            }
        };
    }
    if (hasValue) {
        return reject('UNEXPECTED_VALUE', `命令 ${normalized} 不接受 value 参数`);
    }
    return {
        ok: true,
        command: {
            cmd: normalized
        }
    };
}
export function buildCommandPayload(command, adminPin) {
    const payload = {
        cmd: command.cmd,
        token: adminPin
    };
    if (command.value !== undefined) {
        payload.value = command.value;
    }
    return JSON.stringify(payload);
}
export function payloadFitsLimit(payload) {
    return Buffer.byteLength(payload, 'utf8') <= MAX_PAYLOAD_BYTES;
}
