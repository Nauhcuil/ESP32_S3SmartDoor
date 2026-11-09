import { BemfaHttpTransport } from './bemfa/httpClient.js';
import { ConfigError, loadConfig } from './config.js';
import { describeTopics, resolveTopics } from './protocol/topics.js';
import { DeviceService } from './service/deviceService.js';
import { MockService } from './service/mockService.js';
import { createBridgeServer } from './server.js';
import { log } from './util/log.js';
function main() {
    let config;
    try {
        config = loadConfig();
    } catch (error) {
        if (error instanceof ConfigError) {
            console.error(`[配置错误] ${error.message}`);
            process.exitCode = 1;
            return;
        }
        throw error;
    }
    const service = config.mock ? new MockService(config) : new DeviceService(config, new BemfaHttpTransport({
        uid: config.bemfa.uid,
        baseUrl: config.bemfa.baseUrl,
        timeoutMs: config.bemfa.timeoutMs,
        publishSuffix: config.bemfa.publishSuffix
    }));
    const bridge = createBridgeServer(config, service);
    bridge.listen(config.port).then((port)=>{
        const topics = resolveTopics(config.bemfa.topicsCsv);
        log.info('server.started', {
            port,
            mode: service.mode,
            topicFields: topics.fieldCount,
            topics: config.mock ? '(演示模式不读取)' : describeTopics(topics),
            publishSuffix: config.bemfa.publishSuffix || '(无)',
            openPerMinute: config.openLimitPerMinute
        });
        if (service.mode === 'mock') {
            console.log('[提示] 当前为演示模式（MOCK=1），不会访问巴法云，也不会操作真实门禁。');
        }
    }).catch((error)=>{
        log.error('server.listen.fail', {
            message: error instanceof Error ? error.message : 'unknown'
        });
        process.exitCode = 1;
    });
    const shutdown = (signal)=>{
        log.info('server.shutdown', {
            signal
        });
        void bridge.close().finally(()=>process.exit(0));
    };
    process.on('SIGINT', ()=>shutdown('SIGINT'));
    process.on('SIGTERM', ()=>shutdown('SIGTERM'));
}
main();
