# EnergyMeter Firmware

ESP-IDF firmware for reading a DSMR 5.x / P1 smart meter on an ESP32 and publishing parsed values over MQTT.

This firmware:
- connects to Wi-Fi as a station
- reads P1 / DSMR telegrams from UART
- optionally validates the DSMR CRC
- parses the most useful OBIS values
- publishes a JSON state message to MQTT
- optionally publishes the raw telegram too

The current project is built with PlatformIO + ESP-IDF.

## Build Environment

Project type:
- PlatformIO
- framework: `espidf`

Current PlatformIO environment:
- `esp32-c6-devkitm-1`

Open the firmware configuration menu with:

```bash
pio run -e esp32-c6-devkitm-1 -t menuconfig
```

Build:

```bash
pio run -e esp32-c6-devkitm-1
```

Upload:

```bash
pio run -e esp32-c6-devkitm-1 -t upload
```

Monitor:

```bash
pio device monitor -b 115200
```

## How It Works

At boot the firmware:

1. initializes NVS
2. prepares MQTT topics from the configured base topic
3. starts Wi-Fi station mode
4. waits for an IP address
5. starts the MQTT client
6. initializes the P1 request pin and UART
7. listens for DSMR telegrams starting with `/`
8. parses a complete telegram when it ends with `!` plus CRC
9. publishes the parsed reading to MQTT

Runtime behavior:
- if Wi-Fi disconnects, the firmware keeps retrying
- if MQTT disconnects, the ESP-IDF MQTT client reconnects automatically
- if no P1 data is seen for a while, the firmware logs a warning
- a status task logs Wi-Fi, MQTT, and P1 state every few seconds

## Menu Configuration

All firmware options are under the `Energy Meter` menu in `menuconfig`.

### Wi-Fi

- `Wi-Fi SSID`
- `Wi-Fi password`
- `Wi-Fi reconnect retries before steady retry loop`

Notes:
- the firmware uses station mode only
- power save is disabled to keep Wi-Fi behavior predictable

### MQTT

- `MQTT broker URI`
- `Enable MQTT TLS certificate bundle`
- `MQTT username`
- `MQTT password`
- `MQTT client ID`
- `MQTT base topic`
- `MQTT QoS`
- `Publish raw DSMR telegram to <base>/raw`

Notes:
- if `MQTT client ID` is empty, the ESP-IDF client auto-generates one
- TLS support uses the ESP-IDF certificate bundle
- if `Wi-Fi SSID` or `MQTT broker URI` is empty, the firmware still boots but logs a warning

### P1 / UART

- `P1 UART port`
- `P1 UART baudrate`
- `P1 RX GPIO`
- `Enable internal pull-up on P1 RX`
- `P1 request GPIO (-1 to disable)`
- `P1 request signal active high`
- `Invert P1 RX signal`
- `Require valid DSMR CRC`
- `UART RX buffer size`
- `Telegram buffer size`
- `Reader task stack size`

Notes:
- `P1 request GPIO = -1` disables the request pin completely
- `P1 request signal active high = n` means the configured request GPIO is driven low when active
- many P1 interfaces need RX inversion enabled
- CRC validation is recommended for normal use

## MQTT Protocol

The firmware publishes under the configured base topic:

- `<base>/state`
- `<base>/availability`
- `<base>/raw` if raw telegram publishing is enabled

With the default configuration:

- `energymeter/t210/state`
- `energymeter/t210/availability`
- `energymeter/t210/raw`

### Availability Topic

Topic:

```text
<base>/availability
```

Payload:

- `online` when MQTT connects
- `offline` through the MQTT last-will message if the device disappears unexpectedly

Behavior:
- QoS: `1`
- retained: `yes`

### State Topic

Topic:

```text
<base>/state
```

Payload type:
- JSON object

Behavior:
- QoS: configured by `MQTT QoS`
- retained: `no`

Fields published by the firmware:

- `header`
- `crc_ok`
- `meter_id`
- `timestamp`
- `gas_timestamp`
- `tariff`
- `power_failures`
- `long_power_failures`
- `import_t1_kwh`
- `import_t2_kwh`
- `export_t1_kwh`
- `export_t2_kwh`
- `power_delivered_kw`
- `power_returned_kw`
- `voltage_l1_v`
- `voltage_l2_v`
- `voltage_l3_v`
- `current_l1_a`
- `current_l2_a`
- `current_l3_a`
- `power_delivered_l1_kw`
- `power_delivered_l2_kw`
- `power_delivered_l3_kw`
- `power_returned_l1_kw`
- `power_returned_l2_kw`
- `power_returned_l3_kw`
- `gas_m3`

Important details:
- not every field is always present
- fields are only included when the parser actually extracted a valid value
- `crc_ok` is always present
- if `Require valid DSMR CRC` is enabled, telegrams with bad CRC are ignored and never published

Example state payload:

```json
{
  "header": "/XMX5LGBBFFB231234567",
  "crc_ok": true,
  "meter_id": "453030343833303330303031363835334393138",
  "timestamp": "240530211530S",
  "gas_timestamp": "240530210000S",
  "tariff": 1,
  "import_t1_kwh": 2345.678,
  "import_t2_kwh": 1234.567,
  "power_delivered_kw": 0.742,
  "voltage_l1_v": 231.4,
  "current_l1_a": 3.21,
  "gas_m3": 987.654
}
```

### Raw Telegram Topic

Topic:

```text
<base>/raw
```

Payload type:
- plain text DSMR telegram

Behavior:
- published only when `Publish raw DSMR telegram to <base>/raw` is enabled
- QoS: `0`
- retained: `no`

## Typical Broker Configuration

Examples for `MQTT broker URI`:

```text
mqtt://192.168.1.1:1883
mqtt://mqtt-broker.local:1883
mqtts://broker.example.com:8883
```

If your broker does not require authentication:
- leave `MQTT username` empty
- leave `MQTT password` empty

## Serial Logs

Useful log lines:

- `Wi-Fi station started, connecting to SSID "..."`
- `Wi-Fi connected to "..."`
- `Starting MQTT client topic=...`
- `MQTT connected`
- `P1 request pin set to GPIO...`
- `P1 UART ready port=... rx_gpio=... baud=... invert_rx=...`
- `First P1 UART bytes received`
- `P1 telegram start detected`
- `Telegram ok meter=... power=... gas=... crc=...`
- `Publishing meter reading to MQTT`

If something is wrong you may also see:

- `Wi-Fi disconnected`
- `MQTT disconnected`
- `No P1 data seen on UART...`
- `Ignoring telegram because CRC check failed`
- `Telegram buffer overflow, dropping frame`

## Hardware Notes

The firmware expects a proper P1 electrical interface.

Do not assume a bare UART wire is enough:
- P1 data is often open-collector / inverted
- some setups need a request line
- many meters or interfaces need pull-up / inversion / level adaptation

So the firmware settings for:
- RX inversion
- internal pull-up
- request GPIO
- request polarity

must match your actual hardware interface.

## Related Files

- firmware source: [src/main.cpp](./EnergyMeter/src/main.cpp:1)
- menu config options: [src/Kconfig.projbuild](./EnergyMeter/src/Kconfig.projbuild:1)
- Node-RED helpers: [node-red/README.md](./EnergyMeter/node-red/README.md:1)
- Grafana dashboards: [grafana/README.md](./EnergyMeter/grafana/README.md:1)
- Influx rollups: [influxdb/README.md](./EnergyMeter/influxdb/README.md:1)
