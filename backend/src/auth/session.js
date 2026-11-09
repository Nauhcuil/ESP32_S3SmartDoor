import { createHash, createHmac, randomBytes, timingSafeEqual } from 'node:crypto';
const TOKEN_VERSION = 1;
function base64url(input) {
    return input.toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
function fromBase64url(input) {
    const padded = input.replace(/-/g, '+').replace(/_/g, '/');
    return Buffer.from(padded, 'base64');
}
function sign(payloadB64, secret) {
    return base64url(createHmac('sha256', secret).update(payloadB64).digest());
}
export function verifyPin(input, expected) {
    const a = createHash('sha256').update(input, 'utf8').digest();
    const b = createHash('sha256').update(expected, 'utf8').digest();
    return timingSafeEqual(a, b);
}
export function createSessionToken(secret, ttlMs, nowMs = Date.now()) {
    const payload = {
        v: TOKEN_VERSION,
        iat: nowMs,
        exp: nowMs + ttlMs,
        n: base64url(randomBytes(9))
    };
    const payloadB64 = base64url(Buffer.from(JSON.stringify(payload), 'utf8'));
    return {
        token: `${payloadB64}.${sign(payloadB64, secret)}`,
        expiresAt: payload.exp
    };
}
export function verifySessionToken(token, secret, nowMs = Date.now()) {
    const parts = token.split('.');
    if (parts.length !== 2) {
        return {
            ok: false,
            reason: 'MALFORMED'
        };
    }
    const [payloadB64, signature] = parts;
    const expected = sign(payloadB64, secret);
    const given = Buffer.from(signature, 'utf8');
    const wanted = Buffer.from(expected, 'utf8');
    if (given.length !== wanted.length || !timingSafeEqual(given, wanted)) {
        return {
            ok: false,
            reason: 'BAD_SIGNATURE'
        };
    }
    let payload;
    try {
        payload = JSON.parse(fromBase64url(payloadB64).toString('utf8'));
    } catch  {
        return {
            ok: false,
            reason: 'MALFORMED'
        };
    }
    if (typeof payload.exp !== 'number' || payload.v !== TOKEN_VERSION) {
        return {
            ok: false,
            reason: 'MALFORMED'
        };
    }
    if (payload.exp <= nowMs) {
        return {
            ok: false,
            reason: 'EXPIRED'
        };
    }
    return {
        ok: true,
        expiresAt: payload.exp
    };
}
