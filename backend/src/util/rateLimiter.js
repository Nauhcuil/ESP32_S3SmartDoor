export class SlidingWindowLimiter {
    limit;
    windowMs;
    hits = [];
    constructor(limit, windowMs){
        this.limit = limit;
        this.windowMs = windowMs;
        if (limit < 1 || windowMs < 1) {
            throw new Error('限速参数必须为正');
        }
    }
    tryAcquire(nowMs = Date.now()) {
        this.prune(nowMs);
        if (this.hits.length >= this.limit) {
            return false;
        }
        this.hits.push(nowMs);
        return true;
    }
    retryAfterMs(nowMs = Date.now()) {
        this.prune(nowMs);
        if (this.hits.length < this.limit) {
            return 0;
        }
        const oldest = this.hits[0];
        return Math.max(0, this.windowMs - (nowMs - oldest));
    }
    remaining(nowMs = Date.now()) {
        this.prune(nowMs);
        return Math.max(0, this.limit - this.hits.length);
    }
    prune(nowMs) {
        const cutoff = nowMs - this.windowMs;
        while(this.hits.length > 0 && this.hits[0] <= cutoff){
            this.hits.shift();
        }
    }
}
export class FailureLimiter {
    maxFailures;
    windowMs;
    state = new Map();
    constructor(maxFailures, windowMs){
        this.maxFailures = maxFailures;
        this.windowMs = windowMs;
    }
    isBlocked(key, nowMs = Date.now()) {
        const entry = this.state.get(key);
        if (entry === undefined) {
            return false;
        }
        if (nowMs - entry.windowStart > this.windowMs) {
            this.state.delete(key);
            return false;
        }
        return entry.count >= this.maxFailures;
    }
    recordFailure(key, nowMs = Date.now()) {
        const entry = this.state.get(key);
        if (entry === undefined || nowMs - entry.windowStart > this.windowMs) {
            this.state.set(key, {
                count: 1,
                windowStart: nowMs
            });
            return;
        }
        entry.count += 1;
    }
    reset(key) {
        this.state.delete(key);
    }
}
