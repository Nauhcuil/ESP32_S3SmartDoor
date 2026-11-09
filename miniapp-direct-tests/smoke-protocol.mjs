import assert from 'node:assert/strict';
import { resolveTopics } from './services/protocol/topics.js';
import { parseUplink } from './services/protocol/parse.js';
import { RecordStore } from './services/protocol/dedupe.js';
import { buildCommandPayload, payloadFitsLimit, validateCommand, utf8ByteLength } from './services/protocol/validate.js';
import { DirectService } from './services/directService.js';

let passed = 0;
const cases = [];
function test(name, fn) {
    cases.push({ name, fn });
}
async function run() {
    for (const item of cases) {
        await item.fn();
        passed += 1;
        console.log(`PASS ${item.name}`);
    }
    console.log(`${passed}/${cases.length} PASS`);
}

const LOG_LINE = '{"seq":123,"boot":87,"ts":1762491234567,"iso":"2026-11-07 14:13:54","type":"access","result":"open","reason":"remote","name":"","sim":-1.00,"live":-1.00,"extra":"bemfa"}\n';
const HEARTBEAT = '{"type":"status","seq":123,"ts":1762491234567,"fw":"0.1.0","rssi":-52,"ip":"192.168.1.23","faces":3,"uptime":9876}';

test('主题降级映射：4 个字段', () => {
    const t = resolveTopics('doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger');
    assert.equal(t.access, 'doorcamEvent');
    assert.equal(t.cmd, 'doorcamCmd');
    assert.equal(t.status, 'doorcamStatus');
    assert.equal(t.stranger, 'doorcamStranger');
});
test('主题降级映射：2 个字段时 f0 承担 ACCESS/STRANGER/STATUS', () => {
    const t = resolveTopics('evt,cmd');
    assert.equal(t.access, 'evt');
    assert.equal(t.cmd, 'cmd');
    assert.equal(t.status, 'evt');
    assert.equal(t.stranger, 'evt');
});
test('记录解析：末尾换行、boot:seq 键、iso/seq', () => {
    const parsed = parseUplink(LOG_LINE);
    assert.equal(parsed.ok, true);
    assert.equal(parsed.value.kind, 'log');
    assert.equal(parsed.value.record.key, '87:123');
    assert.equal(parsed.value.record.result, 'open');
    assert.equal(parsed.value.record.iso, '2026-11-07 14:13:54');
});
test('心跳解析：faces/rssi/ip', () => {
    const parsed = parseUplink(HEARTBEAT);
    assert.equal(parsed.value.kind, 'heartbeat');
    assert.equal(parsed.value.heartbeat.faces, 3);
    assert.equal(parsed.value.heartbeat.rssi, -52);
});
test('去重：同 boot:seq 同内容判重复', () => {
    const store = new RecordStore(10);
    const record = parseUplink(LOG_LINE).value.record;
    assert.equal(store.add(record).added, true);
    assert.equal(store.add(record).duplicate, true);
    assert.equal(store.size(), 1);
});
test('命令校验：set_threshold 0.55 通过、0.51 拒绝', () => {
    assert.equal(validateCommand({ cmd: 'set_threshold', value: 0.55 }).ok, true);
    assert.equal(validateCommand({ cmd: 'set_threshold', value: 0.51 }).code, 'BAD_STEP');
});
test('命令校验：活体只收 0/1，未知命令拒绝', () => {
    assert.equal(validateCommand({ cmd: 'set_liveness', value: 1 }).ok, true);
    assert.equal(validateCommand({ cmd: 'set_liveness', value: 0.5 }).code, 'BAD_VALUE');
    assert.equal(validateCommand({ cmd: 'clear_all' }).code, 'UNKNOWN_CMD');
});
test('载荷：token 注入且不超过 256 字节', () => {
    const payload = buildCommandPayload({ cmd: 'open' }, '1234');
    assert.equal(payload, '{"cmd":"open","token":"1234"}');
    assert.equal(payloadFitsLimit(payload), true);
    assert.equal(utf8ByteLength('中文'), 6);
});

function makeTransport() {
    let clock = 1_700_000_000_000;
    const state = {
        published: [],
        status: null,
        event: null,
        online: true,
        withQuery: false,
        queryFlipAt: 0
    };
    const transport = {
        snapshot: async () => {
            if (state.queryFlipAt > 0 && clock >= state.queryFlipAt) {
                state.queryFlipAt = 0;
                state.status = {
                    msg: JSON.stringify({ type: 'query', seq: 9, items: [LOG_LINE, '{"seq":125}'] }),
                    unix: Math.floor(clock / 1000)
                };
            }
            return {
                doorcamStatus: state.status
                    ? { msg: state.status.msg, unix: state.status.unix, time: '', online: state.online }
                    : { msg: '', unix: 0, time: '', online: state.online },
                doorcamEvent: state.event
                    ? { msg: state.event.msg, unix: state.event.unix, time: '', online: true }
                    : { msg: '', unix: 0, time: '', online: true },
                doorcamStranger: { msg: '', unix: 0, time: '', online: true }
            };
        },
        publish: async (topic, msg) => {
            state.published.push({ topic, msg });
            if (topic === 'doorcamCmd') {
                clock += 1500;
                const cmd = JSON.parse(msg).cmd;
                state.status = {
                    msg: JSON.stringify({ type: 'ack', cmd, ok: 1 }),
                    unix: Math.floor(clock / 1000)
                };
                if (cmd === 'query_log' && state.withQuery) {
                    state.queryFlipAt = clock + 1500;
                }
            }
        }
    };
    const now = () => clock;
    const sleep = async (ms) => {
        clock += ms;
    };
    return { transport, state, now, sleep };
}

test('记录聚合：event 最新一条被解析入库', async () => {
    const { transport, state, now, sleep } = makeTransport();
    state.event = { msg: LOG_LINE, unix: 1762491234 };
    const settings = { topics: resolveTopics('doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger'), topicsCsv: '', suffix: '', uid: 'x' };
    const service = new DirectService(settings, transport, { now, sleep });
    const view = await service.getRecords(20);
    assert.equal(view.items.length, 1);
    assert.equal(view.items[0].seq, 123);
});

test('状态：心跳最新一条 + 在线', async () => {
    const { transport, state, now, sleep } = makeTransport();
    state.status = { msg: HEARTBEAT, unix: 1762491234 };
    const settings = { topics: resolveTopics('doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger'), topicsCsv: '', suffix: '', uid: 'x' };
    const service = new DirectService(settings, transport, { now, sleep });
    const status = await service.getDeviceStatus();
    assert.equal(status.online, true);
    assert.equal(status.lastHeartbeat.fw, '0.1.0');
    assert.equal(status.lastHeartbeat.faces, 3);
});

test('命令：open 收到 ack 且载荷带 token', async () => {
    const { transport, state, now, sleep } = makeTransport();
    const settings = { topics: resolveTopics('doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger'), topicsCsv: '', suffix: '', uid: 'x' };
    const service = new DirectService(settings, transport, { now, sleep });
    const result = await service.sendCommand({ cmd: 'open' }, '1234');
    assert.equal(state.published.length, 1);
    assert.equal(state.published[0].msg, '{"cmd":"open","token":"1234"}');
    assert.equal(result.ackReceived, true);
    assert.equal(result.ok, true);
});
test('命令：PIN 格式不对直接拒绝', async () => {
    const { transport, now, sleep } = makeTransport();
    const settings = { topics: resolveTopics('doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger'), topicsCsv: '', suffix: '', uid: 'x' };
    const service = new DirectService(settings, transport, { now, sleep });
    await assert.rejects(() => service.sendCommand({ cmd: 'open' }, '12a4'), (error) => {
        assert.equal(error.code, 'PIN_REQUIRED');
        return true;
    });
});
test('命令：离线不发', async () => {
    const { transport, state, now, sleep } = makeTransport();
    state.online = false;
    const settings = { topics: resolveTopics('doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger'), topicsCsv: '', suffix: '', uid: 'x' };
    const service = new DirectService(settings, transport, { now, sleep });
    await assert.rejects(() => service.sendCommand({ cmd: 'open' }, '1234'), (error) => {
        assert.equal(error.code, 'DEVICE_OFFLINE');
        return true;
    });
});
test('命令：第 4 次 open 触发本机限速', async () => {
    const { transport, now, sleep } = makeTransport();
    const settings = { topics: resolveTopics('doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger'), topicsCsv: '', suffix: '', uid: 'x' };
    const service = new DirectService(settings, transport, { now, sleep });
    for (let i = 0; i < 3; i += 1) {
        const result = await service.sendCommand({ cmd: 'open' }, '1234');
        assert.equal(result.ok, true);
    }
    await assert.rejects(() => service.sendCommand({ cmd: 'open' }, '1234'), (error) => {
        assert.equal(error.code, 'OPEN_RATE_LIMITED');
        return true;
    });
});
test('命令：query_log 解析 items 并跳过坏行', async () => {
    const { transport, state, now, sleep } = makeTransport();
    state.withQuery = true;
    const settings = { topics: resolveTopics('doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger'), topicsCsv: '', suffix: '', uid: 'x' };
    const service = new DirectService(settings, transport, { now, sleep });
    const result = await service.sendCommand({ cmd: 'query_log' }, '1234');
    assert.equal(result.ok, true);
    assert.equal(result.query.seq, 9);
    assert.equal(result.query.items.length, 1);
    assert.equal(result.query.skipped, 1);
});

void run();
