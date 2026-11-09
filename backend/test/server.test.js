import { after, test } from 'node:test';
import assert from 'node:assert/strict';
import { loadConfig } from '../src/config.js';
import { MockService } from '../src/service/mockService.js';
import { createBridgeServer } from '../src/server.js';
const config = loadConfig({
    MOCK: '1',
    ADMIN_PIN: '1234',
    SESSION_SECRET: 'h'.repeat(40)
});
const bridge = createBridgeServer(config, new MockService(config));
let baseUrl = '';
test('启动测试服务器', async ()=>{
    const port = await bridge.listen(0, '127.0.0.1');
    baseUrl = `http://127.0.0.1:${port}`;
    assert.ok(port > 0);
});
after(async ()=>{
    await bridge.close();
});
async function call(path, init = {}) {
    const headers = {};
    if (init.body !== undefined) {
        headers['content-type'] = 'application/json';
    }
    if (init.token !== undefined) {
        headers.authorization = `Bearer ${init.token}`;
    }
    const response = await fetch(`${baseUrl}${path}`, {
        method: init.method ?? 'GET',
        headers,
        body: init.body === undefined ? undefined : JSON.stringify(init.body)
    });
    const json = await response.json();
    return {
        status: response.status,
        json
    };
}
test('/api/meta 公开且标明演示模式', async ()=>{
    const { status, json } = await call('/api/meta');
    assert.equal(status, 200);
    assert.equal(json.mode, 'mock');
    assert.match(String(json.demoNotice), /演示模式/);
});
test('未登录访问设备状态返回 401', async ()=>{
    const { status, json } = await call('/api/device');
    assert.equal(status, 401);
    assert.equal(json.error.code, 'UNAUTHORIZED');
});
test('错误 PIN：401 且不泄露正确 PIN', async ()=>{
    const { status, json } = await call('/api/session/login', {
        method: 'POST',
        body: {
            pin: '9999'
        }
    });
    assert.equal(status, 401);
    const error = json.error;
    assert.equal(error.code, 'PIN_INVALID');
    assert.equal(String(error.message).includes('1234'), false);
});
test('正确 PIN 登录后可以读取设备状态与记录', async ()=>{
    const login = await call('/api/session/login', {
        method: 'POST',
        body: {
            pin: '1234'
        }
    });
    assert.equal(login.status, 200);
    const token = String(login.json.token);
    assert.ok(token.length > 20);
    const device = await call('/api/device', {
        token
    });
    assert.equal(device.status, 200);
    assert.equal(device.json.online, true);
    const records = await call('/api/records?limit=10', {
        token
    });
    assert.equal(records.status, 200);
    const items = records.json.items;
    assert.ok(items.length >= 6, `演示模式应有种子记录，实际 ${items.length}`);
});
test('伪造令牌被拒绝', async ()=>{
    const forged = await call('/api/device', {
        token: 'abc.def'
    });
    assert.equal(forged.status, 401);
});
test('授权命令：open 返回成功；非法阈值被 400 拒绝', async ()=>{
    const login = await call('/api/session/login', {
        method: 'POST',
        body: {
            pin: '1234'
        }
    });
    const token = String(login.json.token);
    const open = await call('/api/commands', {
        method: 'POST',
        body: {
            cmd: 'open'
        },
        token
    });
    assert.equal(open.status, 200);
    assert.equal(open.json.ok, true);
    const badStep = await call('/api/commands', {
        method: 'POST',
        body: {
            cmd: 'set_threshold',
            value: 0.51
        },
        token
    });
    assert.equal(badStep.status, 400);
    assert.equal(badStep.json.error.code, 'BAD_STEP');
    const unknown = await call('/api/commands', {
        method: 'POST',
        body: {
            cmd: 'clear_all'
        },
        token
    });
    assert.equal(unknown.status, 400);
    assert.equal(unknown.json.error.code, 'UNKNOWN_CMD');
});
test('未登录不能下发任何命令', async ()=>{
    const { status, json } = await call('/api/commands', {
        method: 'POST',
        body: {
            cmd: 'open'
        }
    });
    assert.equal(status, 401);
    assert.equal(json.error.code, 'UNAUTHORIZED');
});
test('设备查询：返回条数与回执标记', async ()=>{
    const login = await call('/api/session/login', {
        method: 'POST',
        body: {
            pin: '1234'
        }
    });
    const token = String(login.json.token);
    const query = await call('/api/records/query', {
        method: 'POST',
        token
    });
    assert.equal(query.status, 200);
    assert.equal(query.json.items.length, 3);
    assert.equal(query.json.ackReceived, true);
});
test('请求体超限返回 413', async ()=>{
    const huge = {
        cmd: 'open',
        pad: 'x'.repeat(20_000)
    };
    const { status, json } = await call('/api/session/login', {
        method: 'POST',
        body: huge
    });
    assert.equal(status, 413);
    assert.equal(json.error.code, 'BODY_TOO_LARGE');
});
