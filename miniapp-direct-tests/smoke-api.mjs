import assert from 'node:assert/strict';

const LOG_LINE = '{"seq":123,"boot":87,"ts":1762491234567,"iso":"2026-11-07 14:13:54","type":"access","result":"open","reason":"remote","name":"","sim":-1.00,"live":-1.00,"extra":"bemfa"}\n';
const HB = '{"type":"status","seq":123,"ts":1762491234567,"fw":"0.1.0","rssi":-52,"ip":"192.168.1.23","faces":3,"uptime":9876}';

const storage = new Map();
const calls = [];
const state = {
    forceCredential: false,
    // 每个主题最新一条消息（模拟 allTopic）
    latest: {
        doorcamStatus: { msg: HB, time: '', unix: Math.floor(Date.now() / 1000) - 10, online: true },
        doorcamEvent: { msg: LOG_LINE, time: '', unix: Math.floor(Date.now() / 1000) - 5, online: true },
        doorcamStranger: { msg: '', time: '', unix: 0, online: true }
    },
    published: []
};

globalThis.wx = {
    getStorageSync: (key) => (storage.has(key) ? storage.get(key) : ''),
    setStorageSync: (key, value) => storage.set(key, value),
    removeStorageSync: (key) => storage.delete(key),
    request: (options) => {
        calls.push(options);
        const url = new URL(options.url);
        if (state.forceCredential) {
            options.success({ statusCode: 200, data: { code: 40004, message: '私钥或主题错误' } });
            return;
        }
        if (url.pathname === '/vb/api/v2/allTopic') {
            const list = Object.entries(state.latest)
                .filter(([, v]) => v && v.msg)
                .map(([topic, v]) => ({ topic, msg: v.msg, unix: v.unix, time: v.time, online: v.online }));
            options.success({ statusCode: 200, data: { code: 0, msg: 'success', data: { data: list } } });
            return;
        }
        if (url.pathname === '/va/postJsonMsg') {
            const body = typeof options.data === 'string' ? JSON.parse(options.data) : options.data;
            state.published.push({ topic: body.topic, msg: body.msg });
            const cmd = JSON.parse(body.msg).cmd;
            state.latest.doorcamStatus = {
                msg: JSON.stringify({ type: 'ack', cmd, ok: 1 }),
                time: '',
                unix: Math.floor(Date.now() / 1000),
                online: true
            };
            options.success({ statusCode: 200, data: { code: 0, data: true } });
            return;
        }
        options.success({ statusCode: 404, data: { code: 40000 } });
    }
};

const { saveSettings, getSettings, clearSettings } = await import('./services/config.js');
const { api, login, isLoggedIn, clearSession, errorText } = await import('./services/api.js');

const results = [];
async function check(name, fn) {
    await fn();
    results.push(`PASS ${name}`);
    console.log(`PASS ${name}`);
}

await check('未配置时 meta.mode=unconfigured 且 device 抛 NOT_CONFIGURED', async () => {
    clearSettings();
    const meta = await api.meta();
    assert.equal(meta.mode, 'unconfigured');
    assert.equal(meta.configured, false);
    await assert.rejects(() => api.device(), (error) => {
        assert.equal(error.code, 'NOT_CONFIGURED');
        return true;
    });
});

await check('配置校验：UID 太短 / 主题 5 个 被拒', () => {
    assert.equal(saveSettings({ uid: 'abc', topicsCsv: 'a', suffix: '' }).ok, false);
    assert.equal(saveSettings({ uid: 'a'.repeat(32), topicsCsv: 'a,b,c,d,e', suffix: '' }).ok, false);
    assert.equal(saveSettings({ uid: 'a'.repeat(32), topicsCsv: 'evt,cmd,status,stranger', suffix: '/up' }).ok, false);
});

await check('保存后 meta.mode=live 且主题解析正确', async () => {
    const saved = saveSettings({
        uid: 'b'.repeat(32),
        topicsCsv: 'doorcamEvent,doorcamCmd,doorcamStatus,doorcamStranger',
        suffix: ''
    });
    assert.equal(saved.ok, true);
    const settings = getSettings();
    assert.equal(settings.topics.stranger, 'doorcamStranger');
    const meta = await api.meta();
    assert.equal(meta.mode, 'live');
    assert.equal(meta.limits.openPerMinute, 3);
});

await check('device：在线 + 心跳字段', async () => {
    const status = await api.device();
    assert.equal(status.online, true);
    assert.equal(status.lastHeartbeat.fw, '0.1.0');
    assert.equal(status.lastHeartbeat.faces, 3);
    assert.equal(status.upstreamError, null);
});

await check('records：拉取最新一条 + 解析', async () => {
    const view = await api.records(20);
    assert.equal(view.items.length, 1);
    assert.equal(view.items[0].seq, 123);
    assert.equal(view.skipped, 0);
});

await check('login：1~8 位数字，1234 成功', () => {
    assert.equal(isLoggedIn(), false);
    login('1234');
    assert.equal(isLoggedIn(), true);
    assert.throws(() => login('12a4'), (error) => error.code === 'PIN_INVALID');
    clearSession();
});

await check('command 未登录 => UNAUTHORIZED(401)', async () => {
    await assert.rejects(() => api.command('open'), (error) => {
        assert.equal(error.code, 'UNAUTHORIZED');
        assert.equal(error.status, 401);
        return true;
    });
});

await check('command open：载荷带 token，收到 ack 成功', async () => {
    login('1234');
    const result = await api.command('open');
    assert.equal(state.published.length, 1);
    assert.equal(state.published[0].msg, '{"cmd":"open","token":"1234"}');
    assert.equal(result.ackReceived, true);
    assert.equal(result.ok, true);
});

await check('command set_threshold 0.51 => BAD_STEP(400) 且未发消息', async () => {
    const before = state.published.length;
    await assert.rejects(() => api.command('set_threshold', 0.51), (error) => {
        assert.equal(error.code, 'BAD_STEP');
        assert.equal(error.status, 400);
        return true;
    });
    assert.equal(state.published.length, before);
});

await check('command clear_all => UNKNOWN_CMD', async () => {
    await assert.rejects(() => api.command('clear_all'), (error) => {
        assert.equal(error.code, 'UNKNOWN_CMD');
        return true;
    });
});

await check('凭据错误：device 返回 upstreamError，records 抛 BEMFA_CREDENTIAL', async () => {
    state.forceCredential = true;
    const status = await api.device();
    assert.equal(status.online, false);
    assert.equal(status.upstreamError, 'BEMFA_CREDENTIAL');
    await assert.rejects(() => api.records(10), (error) => {
        assert.equal(error.code, 'BEMFA_CREDENTIAL');
        assert.equal(errorText(error).includes('40004'), true);
        return true;
    });
    state.forceCredential = false;
});

console.log(`${results.length}/${results.length} PASS`);
