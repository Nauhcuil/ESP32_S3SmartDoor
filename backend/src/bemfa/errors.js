import { redact } from '../util/log.js';
import { BemfaError } from './httpClient.js';
export function expectBemfaError(error) {
    if (error instanceof BemfaError) {
        return error;
    }
    const message = error instanceof Error ? error.message : '未知错误';
    return new BemfaError('BEMFA_UPSTREAM', redact(message));
}
