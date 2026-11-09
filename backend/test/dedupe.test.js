import { test } from 'node:test';
import assert from 'node:assert/strict';
import { RecordStore } from '../src/protocol/dedupe.js';
function rec(seq, ts, boot = 7, result = 'open') {
    return {
        boot,
        seq,
        ts,
        tsValid: true,
        iso: '',
        type: 'access',
        result,
        reason: 'face',
        name: '',
        sim: -1,
        live: -1,
        extra: '',
        key: `${boot}:${seq}`
    };
}
test('相同 boot:seq 的消息只保留一条（QoS1 重投 / 云端重复返回）', ()=>{
    const store = new RecordStore(50);
    assert.equal(store.add(rec(10, 1000)).added, true);
    const again = store.add(rec(10, 1000));
    assert.equal(again.duplicate, true);
    assert.equal(again.added, false);
    assert.equal(store.size(), 1);
});
test('跨重启：同一 seq 不同 boot 视为不同记录', ()=>{
    const store = new RecordStore(50);
    store.add(rec(10, 1000, 1));
    store.add(rec(10, 2000, 2));
    assert.equal(store.size(), 2);
    assert.equal(store.list()[0]?.boot, 2);
});
test('撞键（boot 只有 8 位随机）：内容不同则保留较新 ts，并标记 collision', ()=>{
    const store = new RecordStore(50);
    store.add(rec(10, 1000, 7, 'open'));
    const collision = store.add(rec(10, 1500, 7, 'deny'));
    assert.equal(collision.collision, true);
    assert.equal(store.list(1)[0]?.result, 'deny');
});
test('列表按 seq 倒序，超过容量时淘汰最旧', ()=>{
    const store = new RecordStore(3);
    store.addMany([
        rec(1, 100),
        rec(2, 200),
        rec(3, 300),
        rec(4, 400)
    ]);
    assert.equal(store.size(), 3);
    assert.deepEqual(store.list().map((item)=>item.seq), [
        4,
        3,
        2
    ]);
});
test('latestSeq 用于判断云端是否漏掉事件', ()=>{
    const store = new RecordStore(10);
    assert.equal(store.latestSeq(), null);
    store.add(rec(5, 100));
    store.add(rec(9, 200));
    assert.equal(store.latestSeq(), 9);
});
