const FLUSH_INTERVAL_MS = 60 * 1000;
const START_ADDR = Number.isInteger(msg.startAddr) ? msg.startAddr : 40069;
const REGISTER_COUNT = 39;

function readRegisters(payload) {
    if (Buffer.isBuffer(payload)) {
        const words = [];
        for (let i = 0; i < payload.length; i += 2) {
            words.push(payload.readUInt16BE(i));
        }
        return words;
    }

    if (Array.isArray(payload)) {
        return payload;
    }

    if (payload && Array.isArray(payload.data)) {
        return payload.data;
    }

    node.error("Unsupported payload type, expected Buffer or Array", msg);
    return null;
}

function toInt16(value) {
    return value > 0x7FFF ? value - 0x10000 : value;
}

function toUInt32(hiIndex, loIndex, regs) {
    return (regs[hiIndex] << 16) + regs[loIndex];
}

function applySF(raw, scaleFactor) {
    return raw * Math.pow(10, scaleFactor);
}

function parseMessageTimestamp(message) {
    const candidate = message.timestamp ?? message.ts ?? message.sampleTime;

    if (typeof candidate === "number" && Number.isFinite(candidate)) {
        return candidate;
    }

    if (candidate instanceof Date) {
        return candidate.getTime();
    }

    if (typeof candidate === "string") {
        const parsed = Date.parse(candidate);
        if (!Number.isNaN(parsed)) {
            return parsed;
        }
    }

    return Date.now();
}

const regs = readRegisters(msg.payload);
if (regs === null) {
    return null;
}

if (regs.length < REGISTER_COUNT) {
    node.error(`Expected at least ${REGISTER_COUNT} registers starting at ${START_ADDR}, got ${regs.length}`, msg);
    return null;
}

const sf = {
    I_AC_Current: toInt16(regs[6]),
    I_AC_Voltage: toInt16(regs[13]),
    I_AC_Power: toInt16(regs[15]),
    I_AC_Frequency: toInt16(regs[17]),
    I_AC_VA: toInt16(regs[19]),
    I_AC_VAR: toInt16(regs[21]),
    I_AC_PF: toInt16(regs[23]),
    I_AC_Energy: regs[26],
    I_DC_Current: toInt16(regs[28]),
    I_DC_Voltage: toInt16(regs[30]),
    I_DC_Power: toInt16(regs[32]),
    I_Temp: toInt16(regs[37]),
};

const receivedAt = Date.now();
const sampleTime = parseMessageTimestamp(msg);

const fields = {
    C_SunSpec_DID: regs[0],
    C_SunSpec_Length: regs[1],

    I_AC_Current: applySF(regs[2], sf.I_AC_Current),
    I_AC_CurrentA: applySF(regs[3], sf.I_AC_Current),
    I_AC_CurrentB: applySF(regs[4], sf.I_AC_Current),
    I_AC_CurrentC: applySF(regs[5], sf.I_AC_Current),
    I_AC_Current_SF: sf.I_AC_Current,

    I_AC_VoltageAB: applySF(regs[7], sf.I_AC_Voltage),
    I_AC_VoltageBC: applySF(regs[8], sf.I_AC_Voltage),
    I_AC_VoltageCA: applySF(regs[9], sf.I_AC_Voltage),
    I_AC_VoltageAN: applySF(regs[10], sf.I_AC_Voltage),
    I_AC_VoltageBN: applySF(regs[11], sf.I_AC_Voltage),
    I_AC_VoltageCN: applySF(regs[12], sf.I_AC_Voltage),
    I_AC_Voltage_SF: sf.I_AC_Voltage,

    I_AC_Power: applySF(toInt16(regs[14]), sf.I_AC_Power),
    I_AC_Power_SF: sf.I_AC_Power,

    I_AC_Frequency: applySF(regs[16], sf.I_AC_Frequency),
    I_AC_Frequency_SF: sf.I_AC_Frequency,

    I_AC_VA: applySF(toInt16(regs[18]), sf.I_AC_VA),
    I_AC_VA_SF: sf.I_AC_VA,
    I_AC_VAR: applySF(toInt16(regs[20]), sf.I_AC_VAR),
    I_AC_VAR_SF: sf.I_AC_VAR,

    I_AC_PF: applySF(toInt16(regs[22]), sf.I_AC_PF),
    I_AC_PF_SF: sf.I_AC_PF,

    I_AC_Energy_WH: applySF(toUInt32(24, 25, regs), sf.I_AC_Energy),
    I_AC_Energy_WH_SF: sf.I_AC_Energy,

    I_DC_Current: applySF(regs[27], sf.I_DC_Current),
    I_DC_Current_SF: sf.I_DC_Current,
    I_DC_Voltage: applySF(regs[29], sf.I_DC_Voltage),
    I_DC_Voltage_SF: sf.I_DC_Voltage,
    I_DC_Power: applySF(toInt16(regs[31]), sf.I_DC_Power),
    I_DC_Power_SF: sf.I_DC_Power,

    I_Temp_Sink: applySF(toInt16(regs[34]), sf.I_Temp),
    I_Temp_SF: sf.I_Temp,

    I_Status: regs[38],
    time: sampleTime,
};

const tags = {};
if (typeof msg.device_id === "string" && msg.device_id.trim() !== "") {
    tags.device_id = msg.device_id.trim();
} else if (typeof msg.deviceId === "string" && msg.deviceId.trim() !== "") {
    tags.device_id = msg.deviceId.trim();
}

const bufferState = context.get("influxPointBuffer") || {
    startedAt: receivedAt,
    points: [],
};

if (!Array.isArray(bufferState.points) || typeof bufferState.startedAt !== "number") {
    bufferState.startedAt = receivedAt;
    bufferState.points = [];
}

bufferState.points.push([fields, tags]);

if ((receivedAt - bufferState.startedAt) < FLUSH_INTERVAL_MS) {
    context.set("influxPointBuffer", bufferState);
    return null;
}

msg.payload = bufferState.points;
context.set("influxPointBuffer", {
    startedAt: receivedAt,
    points: [],
});
return msg;
