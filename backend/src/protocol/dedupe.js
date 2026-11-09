export class RecordStore {
    cap;
    items = new Map();
    constructor(cap = 200){
        this.cap = cap;
        if (cap < 1) {
            throw new Error('RecordStore 容量必须大于 0');
        }
    }
    add(record) {
        const existing = this.items.get(record.key);
        if (existing === undefined) {
            this.items.set(record.key, record);
            this.trim();
            return {
                added: true,
                duplicate: false,
                collision: false
            };
        }
        const sameContent = existing.ts === record.ts && existing.result === record.result && existing.reason === record.reason;
        if (sameContent) {
            return {
                added: false,
                duplicate: true,
                collision: false
            };
        }
        const existingTs = existing.ts ?? -1;
        const incomingTs = record.ts ?? -1;
        if (incomingTs > existingTs) {
            this.items.set(record.key, record);
        }
        return {
            added: false,
            duplicate: false,
            collision: true
        };
    }
    addMany(records) {
        let added = 0;
        let duplicates = 0;
        let collisions = 0;
        for (const record of records){
            const outcome = this.add(record);
            if (outcome.added) {
                added += 1;
            } else if (outcome.duplicate) {
                duplicates += 1;
            } else if (outcome.collision) {
                collisions += 1;
            }
        }
        return {
            added,
            duplicates,
            collisions
        };
    }
    list(limit) {
        const sorted = [
            ...this.items.values()
        ].sort((a, b)=>{
            if (b.seq !== a.seq) {
                return b.seq - a.seq;
            }
            return (b.ts ?? -1) - (a.ts ?? -1);
        });
        return limit === undefined ? sorted : sorted.slice(0, Math.max(0, limit));
    }
    latestSeq() {
        const sorted = this.list(1);
        return sorted.length > 0 ? sorted[0].seq : null;
    }
    size() {
        return this.items.size;
    }
    clear() {
        this.items.clear();
    }
    trim() {
        if (this.items.size <= this.cap) {
            return;
        }
        const sorted = this.list();
        for (const record of sorted.slice(this.cap)){
            this.items.delete(record.key);
        }
    }
}
