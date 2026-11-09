import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createSessionToken, verifyPin, verifySessionToken } from '../src/auth/session.js';
import { FailureLimiter, SlidingWindowLimiter } from '../src/util/rateLimiter.js';
const SECRET = 'test-secret-'.padEnd(32, 'x');
test('PIN 比对：正确/错误/长度不同都返回布尔，不抛异常', ()=>{
    assert.equal(verifyPin('1234', '1234'), true);
    assert.equal(verifyPin('1234', '12345'), false);
    assert.equal(verifyPin('', '1234'), false);
    assert.equal(verifyPin('04321', '4321'), false);
});
test('会话令牌：签发后可验证，过期后拒绝', ()=>{
    const t0 = 1_700_000_000_000;
    const session = createSessionToken(SECRET, 30 * 60_000, t0);
    const ok = verifySessionToken(session.token, SECRET, t0 + 60_000);
    assert.equal(ok.ok, true);
    if (ok.ok) assert.equal(ok.expiresAt, t0 + 30 * 60_000);
    const expired = verifySessionToken(session.token, SECRET, t0 + 31 * 60_000);
    assert.equal(expired.ok, false);
    if (!expired.ok) assert.equal(expired.reason, 'EXPIRED');
});
test('会话令牌：篡改 payload 或签名都拒绝', ()=>{
    const t0 = 1_700_000_000_000;
    const { token } = createSessionToken(SECRET, 60_000, t0);
    const [payload, signature] = token.split('.');
    const tamperedPayload = verifySessionToken(`${payload}x.${signature}`, SECRET, t0);
    assert.equal(tamperedPayload.ok, false);
    if (!tamperedPayload.ok) assert.equal(tamperedPayload.reason, 'BAD_SIGNATURE');
    const tamperedSignature = verifySessionToken(`${payload}.${signature}x`, SECRET, t0);
    assert.equal(tamperedSignature.ok, false);
    assert.equal(verifySessionToken('not-a-token', SECRET, t0).ok, false);
});
test('会话令牌：换密钥即失效（密钥只存服务端）', ()=>{
    const t0 = 1_700_000_000_000;
    const { token } = createSessionToken(SECRET, 60_000, t0);
    assert.equal(verifySessionToken(token, `${SECRET}other`, t0).ok, false);
});
test('开门限速：60 s 窗口内第 4 次被拒，窗口滑过后恢复', ()=>{
    const limiter = new SlidingWindowLimiter(3, 60_000);
    const t0 = 1_000_000;
    assert.equal(limiter.tryAcquire(t0), true);
    assert.equal(limiter.tryAcquire(t0 + 1_000), true);
    assert.equal(limiter.tryAcquire(t0 + 2_000), true);
    assert.equal(limiter.tryAcquire(t0 + 3_000), false);
    assert.equal(limiter.retryAfterMs(t0 + 3_000), 57_000);
    assert.equal(limiter.tryAcquire(t0 + 61_000), true);
});
test('登录失败计数：连续 5 次后封禁，重置后解除', ()=>{
    const limiter = new FailureLimiter(5, 10 * 60_000);
    const key = '127.0.0.1';
    const t0 = 1_000_000;
    for(let i = 0; i < 5; i += 1){
        limiter.recordFailure(key, t0 + i);
    }
    assert.equal(limiter.isBlocked(key, t0 + 100), true);
    assert.equal(limiter.isBlocked(key, t0 + 10 * 60_000 + 1), false);
    limiter.reset(key);
    assert.equal(limiter.isBlocked(key, t0 + 10 * 60_000 + 1), false);
});
