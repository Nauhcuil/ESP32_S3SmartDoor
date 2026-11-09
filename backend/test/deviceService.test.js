import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadConfig } from '../src/config.js';
import { DeviceService } from '../src/service/deviceService.js';
import { ServiceError } from '../src/service/errors.js';
const STATUS = 'status';
const CMD = 'cmd';
function makeConfig(topics = 'evt,cmd,status,strg') {
    return loadConfig({
        MOCK: '0',
        ADMIN_PIN: '1234',
        SESSION_SECRET: 's'.repeat(40),
        BEMFA_UID: 'uid-placeholder',
        BEMFA_TOPICS: topics
    });
}
class FakeTransport {
    clock;
    messages = new Map();
    published = [];
    offline = false;
    ackOk = true;
    ackDelaySec = 1;
    constructor(clock){
        this.clock = clock;
    }
    publishTarget(topic) {
        return topic;
    }
    async online() {
        return !this.offline;
    }
    async history(topic, num) {
        return (this.messages.get(topic) ?? []).slice(0, num);
    }
    async publish(topic, msg) {
        this.published.push({
            topic,
            msg
        });
        const body = JSON.parse(msg);
        const unix = Math.floor(this.clock() / 1000) + this.ackDelaySec;
        this.push(STATUS, JSON.stringify({
            type: 'ack',
            cmd: body.cmd,
            ok: this.ackOk ? 1 : 0
        }), unix);
        if (body.cmd === 'query_log') {
            this.push(STATUS, JSON.stringify({
                type: 'query',
                seq: 12,
                items: [
                    {
                        seq: 12,
                        boot: 9,
                        ts: 1762491234567,
                        type: 'access',
                        result: 'open',
                        name: '张三'
                    }
                ]
            }), unix);
        }
    }
    push(topic, msg, unix) {
        const list = this.messages.get(topic) ?? [];
        list.push({
            msg,
            unix,
            time: ''
        });
        list.sort((a, b)=>b.unix - a.unix);
        this.messages.set(topic, list);
    }
}
function makeService(transport, clock) {
    return new DeviceService(makeConfig(), transport, {
        now: ()=>clock.t,
        sleep: async (ms)=>{
            clock.t += ms;
        }
    });
}
test('远程开门：下发 JSON 且 token 由后端注入，回执 ok=1 视为成功', async ()=>{
    const clock = {
        t: 1_700_000_000_000
    };
    const transport = new FakeTransport(()=>clock.t);
    const service = makeService(transport, clock);
    const result = await service.sendCommand({
        cmd: 'open'
    });
    assert.equal(result.ok, true);
    assert.equal(result.ackReceived, true);
    assert.equal(result.ambiguous, false);
    assert.equal(transport.published.length, 1);
    const sent = JSON.parse(transport.published[0].msg);
    assert.equal(sent.cmd, 'open');
    assert.equal(sent.token, '1234');
    assert.equal(transport.published[0].topic, CMD);
});
test('回执 ok=0：明确提示固件无法区分令牌/限速/失败', async ()=>{
    const clock = {
        t: 1_700_000_000_000
    };
    const transport = new FakeTransport(()=>clock.t);
    transport.ackOk = false;
    const service = makeService(transport, clock);
    const result = await service.sendCommand({
        cmd: 'reboot'
    });
    assert.equal(result.ok, false);
    assert.equal(result.ackReceived, true);
    assert.match(result.message, /令牌/);
});
test('设备离线：拒绝下发，不制造“已开门”的假象', async ()=>{
    const clock = {
        t: 1_700_000_000_000
    };
    const transport = new FakeTransport(()=>clock.t);
    transport.offline = true;
    const service = makeService(transport, clock);
    await assert.rejects(()=>service.sendCommand({
            cmd: 'open'
        }), (error)=>error instanceof ServiceError && error.code === 'DEVICE_OFFLINE' && error.httpStatus === 409);
    assert.equal(transport.published.length, 0);
});
test('后端开门限速：第 4 次直接 429，且不会下发到设备', async ()=>{
    const clock = {
        t: 1_700_000_000_000
    };
    const transport = new FakeTransport(()=>clock.t);
    const service = makeService(transport, clock);
    for(let i = 0; i < 3; i += 1){
        const result = await service.sendCommand({
            cmd: 'open'
        });
        assert.equal(result.ok, true);
    }
    await assert.rejects(()=>service.sendCommand({
            cmd: 'open'
        }), (error)=>error instanceof ServiceError && error.code === 'OPEN_RATE_LIMITED');
    assert.equal(transport.published.length, 3);
});
test('查询记录：返回固件 query 回执里的最多 3 条', async ()=>{
    const clock = {
        t: 1_700_000_000_000
    };
    const transport = new FakeTransport(()=>clock.t);
    const service = makeService(transport, clock);
    const result = await service.sendCommand({
        cmd: 'query_log'
    });
    assert.equal(result.ackReceived, true);
    assert.equal(result.query?.items.length, 1);
    assert.equal(result.query?.items[0]?.name, '张三');
});
test('同秒回执标记 ambiguous，避免把上一条命令回执当成本次结果', async ()=>{
    const clock = {
        t: 1_700_000_000_000
    };
    const transport = new FakeTransport(()=>clock.t);
    transport.ackDelaySec = 0;
    const service = makeService(transport, clock);
    const result = await service.sendCommand({
        cmd: 'open'
    });
    assert.equal(result.ackReceived, true);
    assert.equal(result.ambiguous, true);
});
test('记录：两个主题合并去重，坏消息计入 skipped', async ()=>{
    const clock = {
        t: 1_700_000_000_000
    };
    const transport = new FakeTransport(()=>clock.t);
    const line = '{"seq":11,"boot":9,"ts":1762491234567,"iso":"","type":"access","result":"open","reason":"face","name":"张三","sim":0.93,"live":0.88,"extra":""}\n';
    transport.push('evt', line, 111);
    transport.push('evt', line, 111);
    transport.push('evt', '{"seq":', 112);
    transport.push('strg', '{"seq":12,"boot":9,"ts":1762491235000,"type":"stranger","result":"info","reason":"unknown","sim":-1,"live":-1}', 113);
    const service = makeService(transport, clock);
    const view = await service.getRecords(20);
    assert.equal(view.items.length, 2);
    assert.equal(view.skipped, 1);
    assert.deepEqual(view.items.map((item)=>item.seq), [
        12,
        11
    ]);
    assert.equal(view.truncated, false);
});
test('状态：心跳超过 90 s 标记 stale，并给出中文提示', async ()=>{
    const clock = {
        t: 1_700_000_000_000
    };
    const transport = new FakeTransport(()=>clock.t);
    transport.push(STATUS, JSON.stringify({
        type: 'status',
        seq: 100,
        ts: clock.t - 5 * 60_000,
        fw: '0.1.0',
        rssi: -60,
        ip: '192.168.1.20',
        faces: 2,
        uptime: 5000
    }), Math.floor((clock.t - 5 * 60_000) / 1000));
    const service = makeService(transport, clock);
    const status = await service.getDeviceStatus();
    assert.equal(status.online, true);
    assert.equal(status.stale, true);
    assert.equal(status.lastHeartbeat?.faces, 2);
    assert.match(status.message, /陈旧/);
});
test('状态：离线时返回明确原因而不是空白字段', async ()=>{
    const clock = {
        t: 1_700_000_000_000
    };
    const transport = new FakeTransport(()=>clock.t);
    transport.offline = true;
    const service = makeService(transport, clock);
    const status = await service.getDeviceStatus();
    assert.equal(status.online, false);
    assert.equal(status.lastHeartbeat, null);
    assert.match(status.message, /离线/);
});
