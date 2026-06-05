# SolarEdge Influx Tasks

These files are for the SolarEdge inverter data, not the DSMR energy meter.

Assumptions:
- raw bucket: `SolarEdge`
- raw measurement: `solar_edge`
- 15-minute bucket: `SolarEdge_15m`
- daily bucket: `SolarEdge_1d`

Files:
- [downsample_15m.flux](./EnergyMeter/solarEdge/influx/downsample_15m.flux:1): recurring 15-minute task
- [backfill_15m.flux](./EnergyMeter/solarEdge/influx/backfill_15m.flux:1): one-time historical rebuild for the 15-minute bucket
- [daily_rollup_1d.flux](./EnergyMeter/solarEdge/influx/daily_rollup_1d.flux:1): recurring daily production task
- [backfill_1d.flux](./EnergyMeter/solarEdge/influx/backfill_1d.flux:1): one-time historical rebuild for the daily bucket

The 15-minute bucket is for trend charts:
- instantaneous fields are downsampled with `mean`
- counter/status/static fields are carried with `last`

The daily bucket is for exact production totals:
- it uses the cumulative inverter energy counter `I_AC_Energy_WH`
- each stored point in `solar_edge_daily` is the daily energy delta in `Wh`

## Create Buckets

```bash
influx bucket create \
  --org <YOUR_ORG> \
  --name SolarEdge_15m \
  --description "15-minute SolarEdge inverter rollups"
```

```bash
influx bucket create \
  --org <YOUR_ORG> \
  --name SolarEdge_1d \
  --description "Daily SolarEdge inverter production"
```

## Rebuild 15-Minute History

If you already have a bad `SolarEdge_15m` bucket, clear it first:

```bash
influx delete \
  --org <YOUR_ORG> \
  --bucket SolarEdge_15m \
  --start 1970-01-01T00:00:00Z \
  --stop 2100-01-01T00:00:00Z
```

Then backfill it:

```bash
influx query \
  --org <YOUR_ORG> \
  --file solarEdge/influx/backfill_15m.flux
```

Create the recurring task:

```bash
influx task create \
  --org <YOUR_ORG> \
  --file solarEdge/influx/downsample_15m.flux
```

## Rebuild Daily History

If you already have a bad `SolarEdge_1d` bucket, clear it first:

```bash
influx delete \
  --org <YOUR_ORG> \
  --bucket SolarEdge_1d \
  --start 1970-01-01T00:00:00Z \
  --stop 2100-01-01T00:00:00Z
```

Then backfill it:

```bash
influx query \
  --org <YOUR_ORG> \
  --file solarEdge/influx/backfill_1d.flux
```

Create the recurring daily task:

```bash
influx task create \
  --org <YOUR_ORG> \
  --file solarEdge/influx/daily_rollup_1d.flux
```

## Query Daily Production

The daily bucket stores production in `Wh`. For a Grafana bar chart in `kWh`:

```flux
from(bucket: "SolarEdge_1d")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "solar_edge_daily")
  |> filter(fn: (r) => r._field == "I_AC_Energy_WH")
  |> map(fn: (r) => ({ r with _value: r._value / 1000.0 }))
```

## Suggested Retention

A balanced policy is:
- `SolarEdge` raw data: keep `365d`
- `SolarEdge_15m`: keep long-term or forever for trends
- `SolarEdge_1d`: keep forever for exact daily production
