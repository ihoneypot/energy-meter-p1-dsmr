// Node-RED Function node
// Input:
// - msg.payload from modbus_sunspec_to_influx_60s_function.js
// Output:
// - MQTT message with only inverter AC power

if (!msg.payload || typeof msg.payload !== "object" || Array.isArray(msg.payload)) {
    node.error("Expected parsed SolarEdge payload object", msg);
    return null;
}

const power = Number(msg.payload.I_AC_Power);
if (!Number.isFinite(power)) {
    node.error("I_AC_Power is missing or invalid", msg);
    return null;
}

msg.topic = "solar/inverter/power";
msg.payload = power;
return msg;
