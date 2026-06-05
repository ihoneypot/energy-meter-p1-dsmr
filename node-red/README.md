# Node-RED Influx Flow

Use the files in this folder with a Node-RED flow shaped like:

`mqtt in (energymeter/t210/state) -> json -> function -> influxdb out`

Files:
- `energymeter_to_influx_function.js`: paste this into a Node-RED `function` node
- `energymeter_to_influx_flow.json`: import this example flow into Node-RED
- `modbus_sunspec_to_influx_60s_function.js`: paste this into a Node-RED `function` node after a Modbus read node to decode and scale SunSpec inverter registers, then batch-write every 60 seconds
- `modbus_sunspec_raw_to_influx_60s_function.js`: paste this into a Node-RED `function` node after a Modbus read node to keep the raw SunSpec register names/values and batch-write every 60 seconds

After import, update:
- MQTT broker settings
- InfluxDB connection settings
- MQTT topic if you changed the firmware base topic

Configure the `influxdb out` node with:
- measurement: `energy_meter_nl`
- time precision: `ms`

The function:
- keeps only known numeric energy fields
- omits missing or invalid values
- adds `meter_id` as a tag when present
- converts DSMR timestamps like `YYMMDDhhmmssS` or `YYMMDDhhmmssW` into Influx timestamps
- buffers points for `60 seconds` in node context and writes them to InfluxDB as one batch
- falls back to the function receive time when the meter timestamp is missing or invalid

For the SunSpec Modbus function:
- input is `msg.payload` as a Modbus `Buffer` or an array of 16-bit registers
- it expects the SunSpec inverter model starting at register `40069` unless `msg.startAddr` overrides it
- `modbus_sunspec_to_influx_60s_function.js` scales the values into engineering units but keeps the original SunSpec-style field names
- `modbus_sunspec_raw_to_influx_60s_function.js` keeps the original SunSpec-style field names and raw register values, including the `_SF` scale-factor registers
- it stores sample time from `msg.timestamp`, `msg.ts`, or `msg.sampleTime` when available
- it keeps only one stable tag: `device_id` when present in `msg.device_id` or `msg.deviceId`
- it buffers points for `60 seconds` and then emits the whole batch for an `influxdb out` node
- configure the `influxdb out` measurement yourself, for example `inverter_sunspec`
