import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolveTopics, splitTopicCsv } from '../src/protocol/topics.js';
test('4 个主题：f0=ACCESS, f1=CMD, f2=STATUS, f3=STRANGER（与固件一致）', ()=>{
    const t = resolveTopics('doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger');
    assert.equal(t.access, 'doorcamEvent');
    assert.equal(t.cmd, 'doorcamCmd');
    assert.equal(t.status, 'doorcamStatus');
    assert.equal(t.stranger, 'doorcamStranger');
    assert.equal(t.fieldCount, 4);
});
test('只有 1 个主题：四个槽位全部回落到它', ()=>{
    const t = resolveTopics('one');
    assert.deepEqual({
        access: t.access,
        stranger: t.stranger,
        status: t.status,
        cmd: t.cmd
    }, {
        access: 'one',
        stranger: 'one',
        status: 'one',
        cmd: 'one'
    });
});
test('2 个主题：f0 承担 ACCESS/STRANGER/STATUS，f1 承担 CMD', ()=>{
    const t = resolveTopics('event,cmd');
    assert.equal(t.access, 'event');
    assert.equal(t.stranger, 'event');
    assert.equal(t.status, 'event');
    assert.equal(t.cmd, 'cmd');
});
test('3 个主题：f0 承担 ACCESS/STRANGER，f1=CMD，f2=STATUS', ()=>{
    const t = resolveTopics('event,cmd,status');
    assert.equal(t.access, 'event');
    assert.equal(t.stranger, 'event');
    assert.equal(t.status, 'status');
    assert.equal(t.cmd, 'cmd');
});
test('跳过空项并裁掉首尾空白', ()=>{
    assert.deepEqual(splitTopicCsv('  a , , b '), [
        'a',
        'b'
    ]);
    const t = resolveTopics(' event , cmd , , status ');
    assert.equal(t.access, 'event');
    assert.equal(t.cmd, 'cmd');
    assert.equal(t.status, 'status');
    assert.equal(t.fieldCount, 3);
});
test('空串：所有槽位为空，不抛异常', ()=>{
    const t = resolveTopics('');
    assert.equal(t.fieldCount, 0);
    assert.equal(t.cmd, '');
});
test('超过 4 个字段只取前 4 个（固件数组上限）', ()=>{
    const t = resolveTopics('a,b,c,d,e');
    assert.equal(t.stranger, 'd');
    assert.equal(t.fieldCount, 4);
});
