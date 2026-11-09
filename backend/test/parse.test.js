import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EPOCH_MS_MIN, parseAccessRecord, parseAck, parseCloudHistory, parseHeartbeat, parseQueryResult, parseUplink } from '../src/protocol/parse.js';
const RAW_LOG = '{"seq":123,"boot":87,"ts":1762491234567,"iso":"2026-11-07 14:13:54","type":"access","result":"open","reason":"remote","name":"","sim":-1.00,"live":-1,"extra":"bemfa"}\n';
test('解析带尾部换行的日志行', ()=>{
    const parsed = parseAccessRecord(RAW_LOG);
    assert.equal(parsed.ok, true);
    if (!parsed.ok) return;
    assert.equal(parsed.value.seq, 123);
    assert.equal(parsed.value.boot, 87);
    assert.equal(parsed.value.key, '87:123');
    assert.equal(parsed.value.result, 'open');
    assert.equal(parsed.value.reason, 'remote');
    assert.equal(parsed.value.tsValid, true);
    assert.equal(parsed.value.sim, -1);
});
test('未对时：ts 是开机毫秒，必须标记 tsValid=false', ()=>{
    const parsed = parseAccessRecord({
        seq: 7,
        boot: 3,
        ts: 98765,
        iso: '',
        type: 'access',
        result: 'deny',
        reason: 'face',
        name: '',
        sim: 0.31,
        live: 0.72,
        extra: ''
    });
    assert.equal(parsed.ok, true);
    if (!parsed.ok) return;
    assert.equal(parsed.value.tsValid, false);
    assert.equal(parsed.value.ts, 98765);
});
test('坏 JSON / 缺 seq / 缺 type 分别给出可读错误，而不是抛异常', ()=>{
    const bad = parseAccessRecord('{"seq":12,');
    assert.equal(bad.ok, false);
    if (!bad.ok) assert.equal(bad.error, 'BAD_JSON');
    const noSeq = parseAccessRecord({
        boot: 3,
        type: 'access',
        result: 'open'
    });
    assert.equal(noSeq.ok, false);
    if (!noSeq.ok) assert.equal(noSeq.error, 'MISSING_SEQ');
    const noType = parseAccessRecord({
        seq: 3,
        result: 'open'
    });
    assert.equal(noType.ok, false);
    if (!noType.ok) assert.equal(noType.error, 'MISSING_TYPE');
});
test('boot 越界（非 0~255）按缺失处理，key 用 x:seq，避免伪造 boot 造成串键', ()=>{
    const parsed = parseAccessRecord({
        seq: 9,
        boot: 300,
        type: 'stranger',
        result: 'info'
    });
    assert.equal(parsed.ok, true);
    if (!parsed.ok) return;
    assert.equal(parsed.value.boot, null);
    assert.equal(parsed.value.key, 'x:9');
});
test('心跳：faces=-1 保留为未知，ts 未对时标记 tsValid=false', ()=>{
    const parsed = parseHeartbeat({
        type: 'status',
        seq: 42,
        ts: 123456,
        fw: '0.1.0',
        rssi: -52,
        ip: '192.168.1.23',
        faces: -1,
        uptime: 120
    });
    assert.equal(parsed.ok, true);
    if (!parsed.ok) return;
    assert.equal(parsed.value.faces, -1);
    assert.equal(parsed.value.tsValid, false);
    assert.equal(parsed.value.rssi, -52);
});
test('心跳：epoch 毫秒判定（1700000000000 边界）', ()=>{
    const at = parseHeartbeat({
        type: 'status',
        ts: EPOCH_MS_MIN,
        fw: 'x'
    });
    assert.equal(at.ok && at.value.tsValid, true);
    const before = parseHeartbeat({
        type: 'status',
        ts: EPOCH_MS_MIN - 1,
        fw: 'x'
    });
    assert.equal(before.ok && before.value.tsValid, false);
});
test('回执：ok 兼容 1/0 与字符串 "1"', ()=>{
    const one = parseAck('{"type":"ack","cmd":"open","ok":1}');
    assert.equal(one.ok && one.value.ok, true);
    const str = parseAck('{"type":"ack","cmd":"set_threshold","ok":"1"}');
    assert.equal(str.ok && str.value.ok, true);
    const zero = parseAck({
        type: 'ack',
        cmd: 'open',
        ok: 0
    });
    assert.equal(zero.ok && zero.value.ok, false);
});
test('回执缺 cmd 视为无效（不能把异常回执当成功）', ()=>{
    const parsed = parseAck({
        type: 'ack',
        ok: 1
    });
    assert.equal(parsed.ok, false);
    if (!parsed.ok) assert.equal(parsed.error, 'MISSING_CMD');
});
test('查询回执：坏条目计入 skipped，不拖垮整条消息', ()=>{
    const parsed = parseQueryResult({
        type: 'query',
        seq: 120,
        items: [
            {
                seq: 120,
                boot: 9,
                ts: 1762491234567,
                type: 'access',
                result: 'open',
                name: '张三'
            },
            {
                boot: 9,
                type: 'access'
            },
            'not-an-object'
        ]
    });
    assert.equal(parsed.ok, true);
    if (!parsed.ok) return;
    assert.equal(parsed.value.items.length, 1);
    assert.equal(parsed.value.skipped, 2);
    assert.equal(parsed.value.seq, 120);
});
test('上行分类：ack / query / 心跳 / 日志都能正确识别', ()=>{
    const kindOf = (raw)=>{
        const p = parseUplink(raw);
        return p.ok ? p.value.kind : `error:${p.error}`;
    };
    assert.equal(kindOf('{"type":"ack","cmd":"open","ok":1}'), 'ack');
    assert.equal(kindOf('{"type":"query","seq":1,"items":[]}'), 'query');
    assert.equal(kindOf('{"type":"status","seq":1,"fw":"0.1.0","uptime":10}'), 'heartbeat');
    assert.equal(kindOf(RAW_LOG), 'log');
});
test('云端消息规范：数组/单对象都接受，按 unix 倒序，坏项丢弃', ()=>{
    const list = parseCloudHistory([
        {
            msg: 'a',
            time: 't',
            unix: 100
        },
        {
            msg: 'b',
            time: 't',
            unix: 300
        },
        {
            msg: '',
            unix: 200
        },
        {
            msg: 'c'
        }
    ]);
    assert.deepEqual(list.map((m)=>m.msg), [
        'b',
        'a'
    ]);
    const single = parseCloudHistory({
        msg: 'only',
        unix: 10,
        time: 't'
    });
    assert.equal(single.length, 1);
    assert.equal(single[0]?.msg, 'only');
});
