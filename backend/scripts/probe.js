import { BemfaHttpTransport } from '../src/bemfa/httpClient.js';
import { ConfigError, loadConfig } from '../src/config.js';
import { parseAccessRecord, parseHeartbeat, parseUplink } from '../src/protocol/parse.js';
import { describeTopics, resolveTopics } from '../src/protocol/topics.js';
import { buildCommandPayload } from '../src/protocol/validate.js';
const sleep = (ms)=>new Promise((resolve)=>setTimeout(resolve, ms));
async function main() {
    let config;
    try {
        config = loadConfig();
    } catch (error) {
        if (error instanceof ConfigError) {
            console.error(`[配置错误] ${error.message}`);
            process.exitCode = 2;
            return;
        }
        throw error;
    }
    if (config.mock) {
        console.error('[提示] 当前 MOCK=1，探针只用于真实巴法云联调，请先设置 MOCK=0');
        process.exitCode = 2;
        return;
    }
    const topics = resolveTopics(config.bemfa.topicsCsv);
    const transport = new BemfaHttpTransport({
        uid: config.bemfa.uid,
        baseUrl: config.bemfa.baseUrl,
        timeoutMs: config.bemfa.timeoutMs,
        publishSuffix: config.bemfa.publishSuffix
    });
    console.log('=== 1. 主题映射（必须与设备配网页完全一致）===');
    console.log(`字段数：${topics.fieldCount}`);
    console.log(describeTopics(topics));
    console.log(`命令发布目标：${transport.publishTarget(topics.cmd) || '(未配置)'}`);
    console.log('\n=== 2. 设备在线（以 CMD 主题判定）===');
    const online = await transport.online(topics.cmd);
    console.log(`online = ${online}`);
    if (!online) {
        console.log('提示：设备离线时巴法云仍可能接受推送但不会补发，先确认设备已连 Wi-Fi 与 MQTT。');
    }
    console.log('\n=== 3. 记录主题历史深度（P2）===');
    const accessMessages = topics.access === '' ? [] : await transport.history(topics.access, 50);
    console.log(`ACCESS 主题返回 ${accessMessages.length} 条（请求 50）`);
    if (accessMessages.length > 0) {
        const newest = accessMessages[0];
        const parsed = newest === undefined ? null : parseAccessRecord(newest.msg);
        console.log(`最新一条：unix=${newest?.unix ?? '-'}，可解析=${parsed?.ok === true}` + (parsed?.ok === true ? `，seq=${parsed.value.seq}，type=${parsed.value.type}` : ''));
    }
    console.log(accessMessages.length <= 1 ? '结论：云端可能只保留每个主题的最新 1 条消息 → 记录页必须以“查询设备最近 3 条”为主，轮询只能看增量。' : `结论：云端至少保留最近 ${accessMessages.length} 条 → 记录页可以按时间窗口增量拉取，但不要假设可分页。`);
    console.log('\n=== 4. STATUS 主题混合消息（心跳/回执/查询回执）===');
    const statusMessages = topics.status === '' ? [] : await transport.history(topics.status, 30);
    console.log(`STATUS 主题返回 ${statusMessages.length} 条（请求 30）`);
    const counts = {
        heartbeat: 0,
        ack: 0,
        query: 0,
        log: 0,
        bad: 0
    };
    for (const message of statusMessages){
        const parsed = parseUplink(message.msg);
        if (!parsed.ok) {
            counts.bad += 1;
        } else {
            counts[parsed.value.kind] += 1;
        }
    }
    console.log(`分类统计：${JSON.stringify(counts)}`);
    const latestHeartbeat = statusMessages.map((message)=>parseHeartbeat(message.msg)).find((parsed)=>parsed.ok);
    if (latestHeartbeat?.ok === true) {
        const hb = latestHeartbeat.value;
        console.log(`最新心跳：seq=${hb.seq ?? '-'} fw=${hb.fw || '-'} rssi=${hb.rssi ?? '-'} ip=${hb.ip || '-'} faces=${hb.faces ?? '-'} uptime=${hb.uptime ?? '-'} tsValid=${hb.tsValid}`);
    } else {
        console.log('未在返回窗口里找到心跳消息（可能窗口太短，或云端只保留最新一条）。');
    }
    const publish = process.argv.includes('--publish');
    console.log('\n=== 5. 命令投递（P1）===');
    if (!publish) {
        console.log('跳过：默认只读。需要验证投递请加 --publish（只发 query_log，无开门副作用）。');
        return;
    }
    if (!online) {
        console.log('跳过：设备离线，投递测试无意义。');
        process.exitCode = 1;
        return;
    }
    if (topics.cmd === '' || topics.status === '') {
        console.log('跳过：CMD/STATUS 主题未配置。');
        process.exitCode = 1;
        return;
    }
    const payload = buildCommandPayload({
        cmd: 'query_log'
    }, config.adminPin);
    const baselineMessages = await transport.history(topics.status, 20);
    const baselineAcks = baselineMessages.map((message)=>({
            msg: message,
            parsed: parseUplink(message.msg)
        })).filter((item)=>item.parsed.ok && item.parsed.value.kind === 'ack' && item.parsed.value.ack.cmd === 'query_log');
    const baselineUnix = baselineAcks.length > 0 ? Math.max(...baselineAcks.map((item)=>item.msg.unix)) : 0;
    const sentUnix = Math.floor(Date.now() / 1000);
    await transport.publish(topics.cmd, payload);
    console.log(`已推送（目标主题 ${transport.publishTarget(topics.cmd)}，${Buffer.byteLength(payload, 'utf8')} 字节）`);
    let ack = null;
    let queryItems = 0;
    for(let i = 0; i < 10 && ack === null; i += 1){
        await sleep(900);
        const after = await transport.history(topics.status, 20);
        for (const message of after){
            const parsed = parseUplink(message.msg);
            if (!parsed.ok || message.unix < sentUnix - 1) {
                continue;
            }
            if (parsed.value.kind === 'ack' && parsed.value.ack.cmd === 'query_log') {
                ack = {
                    ok: parsed.value.ack.ok,
                    unix: message.unix,
                    cmd: parsed.value.ack.cmd
                };
            }
            if (parsed.value.kind === 'query') {
                queryItems = parsed.value.query.items.length;
            }
        }
    }
    if (ack === null) {
        console.log('结论：8 秒内没有回执 → P1 未通过。');
        console.log('下一步：把 BEMFA_PUBLISH_SUFFIX 改为 /set 再试；若仍失败，改用后端以唯一 clientId + appID/secretKey 连接 MQTT 发布（计划中的备选 C）。');
        process.exitCode = 1;
        return;
    }
    console.log(`收到回执：cmd=${ack.cmd} ok=${ack.ok} unix=${ack.unix}（基线 unix=${baselineUnix}，下发 unix=${sentUnix}）`);
    console.log(`查询回执条目数：${queryItems}`);
    if (ack.ok && queryItems > 0) {
        console.log('结论：P1 通过 —— HTTP 推送可直达设备，且能读回记录。');
    } else if (ack.ok) {
        console.log('结论：P1 部分通过 —— 回执可达，但没读到查询结果（可能窗口太短，重跑一次）。');
    } else {
        console.log('结论：回执 ok=0 —— 投递可达，但设备拒绝执行：核对 ADMIN_PIN 与设备 admin_pin 是否一致。');
    }
    console.log('若上面收到的是旧回执（unix 早于下发时间），说明巴法云只保留最新消息，命令结果需靠“回执时间 > 下发时间”判定。');
}
void main().catch((error)=>{
    console.error(`[探针异常] ${error instanceof Error ? error.message : '未知错误'}`);
    process.exitCode = 1;
});
