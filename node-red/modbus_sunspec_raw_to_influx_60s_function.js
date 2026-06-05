const FLUSH_INTERVAL_MS = 60 * 1000;
const START_ADDR = msg.startAddr || 40069;
const REGISTER_COUNT = 39;

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

let regs;
if (Buffer.isBuffer(msg.payload)) {
    regs = [];
    for (let i = 0; i < msg.payload.length; i += 2) {
        regs.push(msg.payload.readUInt16BE(i));
    }
} else if (Array.isArray(msg.payload)) {
    regs = msg.payload;
} else {
    node.error("Unsupported payload type, expected Buffer or Array", msg);
    return null;
}

if (regs.length < REGISTER_COUNT) {
    node.error(`Expected at least ${REGISTER_COUNT} registers starting at ${START_ADDR}, got ${regs.length}`, msg);
    return null;
}

const receivedAt = Date.now();
const sampleTime = parseMessageTimestamp(msg);

const fields = {
    C_SunSpec_DID: regs[0],
    C_SunSpec_Length: regs[1],

    I_AC_Current: regs[2],
    I_AC_CurrentA: regs[3],
    I_AC_CurrentB: regs[4],
    I_AC_CurrentC: regs[5],
    I_AC_Current_SF: regs[6],

    I_AC_VoltageAB: regs[7],
    I_AC_VoltageBC: regs[8],
    I_AC_VoltageCA: regs[9],
    I_AC_VoltageAN: regs[10],
    I_AC_VoltageBN: regs[11],
    I_AC_VoltageCN: regs[12],
    I_AC_Voltage_SF: regs[13],

    I_AC_Power: regs[14],
    I_AC_Power_SF: regs[15],

    I_AC_Frequency: regs[16],
    I_AC_Frequency_SF: regs[17],

    I_AC_VA: regs[18],
    I_AC_VA_SF: regs[19],
    I_AC_VAR: regs[20],
    I_AC_VAR_SF: regs[21],

    I_AC_PF: regs[22],
    I_AC_PF_SF: regs[23],

    I_AC_Energy_WH_HI: regs[24],
    I_AC_Energy_WH_LO: regs[25],
    I_AC_Energy_WH_SF: regs[26],

    I_DC_Current: regs[27],
    I_DC_Current_SF: regs[28],
    I_DC_Voltage: regs[29],
    I_DC_Voltage_SF: regs[30],
    I_DC_Power: regs[31],
    I_DC_Power_SF: regs[32],

    I_Temp_Sink: regs[34],
    I_Temp_SF: regs[37],
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
