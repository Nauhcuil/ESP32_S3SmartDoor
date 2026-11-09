import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildCommandPayload, payloadFitsLimit, validateCommand, MAX_PAYLOAD_BYTES } from '../src/protocol/validate.js';
test('固件支持的五条命令都能通过校验', ()=>{
    const cases = [
        [
            {
                cmd: 'open'
            },
            'open'
        ],
        [
            {
                cmd: 'query_log'
            },
            'query_log'
        ],
        [
            {
                cmd: 'set_threshold',
                value: 0.6
            },
            'set_threshold'
        ],
        [
            {
                cmd: 'set_liveness',
                value: 1
            },
            'set_liveness'
        ],
        [
            {
                cmd: 'reboot'
            },
            'reboot'
        ],
        [
            {
                cmd: ' OPEN '
            },
            'open'
        ]
    ];
    for (const [input, expected] of cases){
        const result = validateCommand(input);
        assert.equal(result.ok, true, JSON.stringify(input));
        if (result.ok) assert.equal(result.command.cmd, expected);
    }
});
test('未知命令被拒绝（固件会静默丢弃，不能让用户以为已执行）', ()=>{
    const result = validateCommand({
        cmd: 'clear_all'
    });
    assert.equal(result.ok, false);
    if (!result.ok) assert.equal(result.code, 'UNKNOWN_CMD');
});
test('阈值：范围 0.40~0.85，步进 0.05', ()=>{
    for (const value of [
        0.4,
        0.45,
        0.55,
        0.85,
        0.6
    ]){
        assert.equal(validateCommand({
            cmd: 'set_threshold',
            value
        }).ok, true, String(value));
    }
    for (const value of [
        0.39,
        0.86,
        0.51,
        0.615,
        0,
        1
    ]){
        const result = validateCommand({
            cmd: 'set_threshold',
            value
        });
        assert.equal(result.ok, false, String(value));
    }
});
test('阈值：缺 value / 非数字 / 字符串都拒绝', ()=>{
    assert.equal(validateCommand({
        cmd: 'set_threshold'
    }).ok, false);
    assert.equal(validateCommand({
        cmd: 'set_threshold',
        value: '0.6'
    }).ok, false);
    assert.equal(validateCommand({
        cmd: 'set_threshold',
        value: Number.NaN
    }).ok, false);
});
test('活体：只接受 0/1，0.5 会被固件判为“关”，因此拒绝', ()=>{
    assert.equal(validateCommand({
        cmd: 'set_liveness',
        value: 0
    }).ok, true);
    assert.equal(validateCommand({
        cmd: 'set_liveness',
        value: 1
    }).ok, true);
    const half = validateCommand({
        cmd: 'set_liveness',
        value: 0.5
    });
    assert.equal(half.ok, false);
    if (!half.ok) assert.equal(half.code, 'BAD_VALUE');
});
test('不带参数的命令不接受多余 value', ()=>{
    const result = validateCommand({
        cmd: 'reboot',
        value: 1
    });
    assert.equal(result.ok, false);
    if (!result.ok) assert.equal(result.code, 'UNEXPECTED_VALUE');
});
test('非对象请求体直接拒绝', ()=>{
    assert.equal(validateCommand(null).ok, false);
    assert.equal(validateCommand([]).ok, false);
    assert.equal(validateCommand('open').ok, false);
});
test('载荷构造：一定是 JSON，且带 token 字段（纯文本命令在固件必被拒）', ()=>{
    const payload = buildCommandPayload({
        cmd: 'open'
    }, '1234');
    assert.equal(payload, '{"cmd":"open","token":"1234"}');
    assert.equal(payload.startsWith('{'), true);
    const withValue = buildCommandPayload({
        cmd: 'set_threshold',
        value: 0.6
    }, '1234');
    assert.equal(JSON.parse(withValue).value, 0.6);
    assert.equal(JSON.parse(withValue).cmd, 'set_threshold');
});
test('载荷字节上限与固件一致（256 字节）', ()=>{
    assert.equal(payloadFitsLimit('a'.repeat(MAX_PAYLOAD_BYTES)), true);
    assert.equal(payloadFitsLimit('a'.repeat(MAX_PAYLOAD_BYTES + 1)), false);
    const long = buildCommandPayload({
        cmd: 'open'
    }, '9'.repeat(80));
    assert.equal(payloadFitsLimit(long), true);
});
