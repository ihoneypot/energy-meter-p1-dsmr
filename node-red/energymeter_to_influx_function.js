const payload = msg.payload;
const flushIntervalMs = 60 * 1000;

if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    return null;
}

if (typeof msg.topic === "string" && !msg.topic.endsWith("/state")) {
    return null;
}

const fieldNames = [
    "tariff",
    "power_failures",
    "long_power_failures",
    "import_t1_kwh",
    "import_t2_kwh",
    "export_t1_kwh",
    "export_t2_kwh",
    "power_delivered_kw",
    "power_returned_kw",
    "gas_m3",
    "voltage_l1_v",
    "voltage_l2_v",
    "voltage_l3_v",
    "current_l1_a",
    "current_l2_a",
    "current_l3_a",
    "power_delivered_l1_kw",
    "power_delivered_l2_kw",
    "power_delivered_l3_kw",
    "power_returned_l1_kw",
    "power_returned_l2_kw",
    "power_returned_l3_kw"
];

function addNumericField(target, source, key) {
    if (!(key in source)) {
        return;
    }

    const value = Number(source[key]);
    if (Number.isFinite(value)) {
        target[key] = value;
    }
}

function parseDsmrTimestamp(raw) {
    if (typeof raw !== "string") {
        return null;
    }

    const match = raw.trim().match(/^(\d{2})(\d{2})(\d{2})(\d{2})(\d{2})(\d{2})([SW])$/);
    if (!match) {
        return null;
    }

    const year = 2000 + Number(match[1]);
    const month = match[2];
    const day = match[3];
    const hour = match[4];
    const minute = match[5];
    const second = match[6];
    const offset = match[7] === "S" ? "+02:00" : "+01:00";

    const parsed = new Date(
        `${year.toString().padStart(4, "0")}-${month}-${day}T${hour}:${minute}:${second}${offset}`
    );

    if (Number.isNaN(parsed.getTime())) {
        return null;
    }

    return parsed;
}

const fields = {};
for (const key of fieldNames) {
    addNumericField(fields, payload, key);
}

if (Object.keys(fields).length === 0) {
    return null;
}

const receivedAt = Date.now();
const meterTime = parseDsmrTimestamp(payload.timestamp);
fields.time = meterTime ? meterTime.getTime() : receivedAt;

const tags = {};
if (typeof payload.meter_id === "string" && payload.meter_id.trim() !== "") {
    tags.meter_id = payload.meter_id.trim();
}

const bufferState = context.get("influxPointBuffer") || {
    startedAt: receivedAt,
    points: []
};

if (!Array.isArray(bufferState.points) || typeof bufferState.startedAt !== "number") {
    bufferState.startedAt = receivedAt;
    bufferState.points = [];
}

bufferState.points.push([fields, tags]);

if ((receivedAt - bufferState.startedAt) < flushIntervalMs) {
    context.set("influxPointBuffer", bufferState);
    return null;
}

msg.payload = bufferState.points;
context.set("influxPointBuffer", {
    startedAt: receivedAt,
    points: []
});
return msg;
