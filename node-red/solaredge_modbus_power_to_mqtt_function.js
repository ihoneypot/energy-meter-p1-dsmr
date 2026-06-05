// Node-RED Function node
// Input:
// - msg.payload from a Modbus node as Buffer, Array, or { data: [] }
// - SunSpec inverter model starting at register 40069
// Output:
// - MQTT message with only inverter AC power

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

const regs = readRegisters(msg.payload);
if (regs === null) {
    return null;
}

if (regs.length < 16) {
    node.error("Expected at least 16 SunSpec registers starting at 40069", msg);
    return null;
}

const power = toInt16(regs[14]) * Math.pow(10, toInt16(regs[15]));

msg.topic = "solar/inverter/power";
msg.payload = power;
return msg;
