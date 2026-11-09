export function splitTopicCsv(csv) {
    const fields = [];
    for (const part of csv.split(',')){
        if (fields.length >= 4) {
            break;
        }
        const trimmed = part.trim();
        if (trimmed.length > 0) {
            fields.push(trimmed);
        }
    }
    return fields;
}
export function resolveTopics(csv) {
    const fields = splitTopicCsv(csv ?? '');
    const empty = {
        access: '',
        stranger: '',
        status: '',
        cmd: '',
        fieldCount: 0
    };
    if (fields.length === 0) {
        return empty;
    }
    const f0 = fields[0];
    if (fields.length === 1) {
        return {
            access: f0,
            stranger: f0,
            status: f0,
            cmd: f0,
            fieldCount: 1
        };
    }
    if (fields.length === 2) {
        const f1 = fields[1];
        return {
            access: f0,
            stranger: f0,
            status: f0,
            cmd: f1,
            fieldCount: 2
        };
    }
    if (fields.length === 3) {
        const f1 = fields[1];
        const f2 = fields[2];
        return {
            access: f0,
            stranger: f0,
            status: f2,
            cmd: f1,
            fieldCount: 3
        };
    }
    const f1 = fields[1];
    const f2 = fields[2];
    const f3 = fields[3];
    return {
        access: f0,
        stranger: f3,
        status: f2,
        cmd: f1,
        fieldCount: 4
    };
}
export function describeTopics(topics) {
    return [
        `ACCESS=${topics.access || '(未配置)'}`,
        `CMD=${topics.cmd || '(未配置)'}`,
        `STATUS=${topics.status || '(未配置)'}`,
        `STRANGER=${topics.stranger || '(未配置)'}`
    ].join(' ');
}
