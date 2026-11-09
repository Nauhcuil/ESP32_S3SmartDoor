import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadConfig } from '../src/config.js';
import { MockService } from '../src/service/mockService.js';
import { ServiceError } from '../src/service/errors.js';
function mockConfig() {
    return loadConfig({
        MOCK: '1',
        ADMIN_PIN: '1234',
        SESSION_SECRET: 'm'.repeat(40)
    });
}
test('演示模式：明确标注 mock，数据不会伪装成真实设备', async ()=>{
    const service = new MockService(mockConfig());
    assert.equal(service.mode, 'mock');
    const status = await service.getDeviceStatus();
    assert.equal(status.online, true);
    assert.match(status.message, /演示模式/);
    const records = await service.getRecords(20);
    assert.match(records.notice ?? '', /演示数据/);
    assert.equal(records.items.length, 6);
});
test('演示模式：开门会追加一条远程记录，且限速仍生效', async ()=>{
    const service = new MockService(mockConfig());
    await service.sendCommand({
        cmd: 'open'
    });
    const afterOpen = await service.getRecords(20);
    assert.equal(afterOpen.items[0]?.reason, 'remote');
    assert.equal(afterOpen.items[0]?.extra, 'bemfa');
    await service.sendCommand({
        cmd: 'open'
    });
    await service.sendCommand({
        cmd: 'open'
    });
    await assert.rejects(()=>service.sendCommand({
            cmd: 'open'
        }), (error)=>error instanceof ServiceError && error.code === 'OPEN_RATE_LIMITED');
});
test('演示模式：查询返回最近 3 条，文案标注未真正下发', async ()=>{
    const service = new MockService(mockConfig());
    const result = await service.sendCommand({
        cmd: 'query_log'
    });
    assert.equal(result.query?.items.length, 3);
    assert.match(result.message, /演示模式/);
});
