# Grafana Dashboard

Import [energymeter_dashboard.json](./EnergyMeter/grafana/energymeter_dashboard.json:1) into Grafana.

For SolarEdge inverter status, import [inverter_status_dashboard.json](./EnergyMeter/grafana/inverter_status_dashboard.json:1) as a separate dashboard.

This dashboard assumes:
- datasource type: `InfluxDB` with `Flux` query language
- bucket name: `HomeEnergy`
- measurement name: `energy_meter_nl`
- fields are written by the Node-RED function from `node-red/energymeter_to_influx_function.js`

Included panels:
- current delivered power
- current returned power
- gas total
- current tariff
- active power over time
- per-phase voltages
- per-phase currents
- per-phase delivered/returned power
- cumulative import/export totals
- power failure counters

After import:
- pick your real InfluxDB datasource when Grafana asks for `DS_INFLUXDB`
- make sure that datasource is configured for `Flux`
- make sure the `HomeEnergy` bucket exists in that datasource
- keep the dashboard time range wide enough to see cumulative fields like kWh and gas
- if you changed the measurement name in Node-RED, update the queries from `energy_meter_nl`

For the inverter status dashboard:
- the default measurement is `inverter_sunspec`
- the queried field is `I_Status`
- if your inverter measurement name is different, edit the Flux query in the imported panel
- the panel keeps the numeric `I_Status` in Influx and maps it in Grafana as:
  - `1` -> `Off`
  - `2` -> `Sleeping`
  - `3` -> `Starting`
  - `4` -> `MPPT`
  - `5` -> `Throttled`
  - `6` -> `Shutting down`
  - `7` -> `Fault`
  - `8` -> `Standby`
