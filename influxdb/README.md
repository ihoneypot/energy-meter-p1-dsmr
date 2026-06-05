# InfluxDB Rollups

This folder contains Flux tasks for two long-term rollups:
- `HomeEnergy_15m` for trend charts
- `HomeEnergy_1d` for exact daily totals

Files:
- `downsample_15m.flux`: Flux task for InfluxDB 2.x / Flux tasks
- `backfill_15m.flux`: one-shot Flux query to rebuild the `HomeEnergy_15m` bucket from raw data
- `daily_rollup_1d.flux`: Flux task that writes exact daily totals from cumulative meter counters
- `backfill_1d.flux`: one-shot Flux query to rebuild the `HomeEnergy_1d` bucket from raw data

Assumptions:
- raw bucket: `HomeEnergy`
- downsampled bucket: `HomeEnergy_15m`
- daily totals bucket: `HomeEnergy_1d`
- measurement: `energy_meter_nl`
- raw bucket should keep 1-second data for `365d`
- downsampled bucket should keep 15-minute data indefinitely
- daily totals bucket should keep exact 1-day totals indefinitely

Create the long-term buckets with infinite retention:

```bash
influx bucket create \
  --name HomeEnergy_15m \
  --description "15-minute downsampled energy meter data"
```

```bash
influx bucket create \
  --name HomeEnergy_1d \
  --description "Daily exact energy meter totals"
```

Find the raw bucket ID and set raw retention to one year:

```bash
influx bucket list --name HomeEnergy
```

```bash
influx bucket update \
  --id <HOMEENERGY_BUCKET_ID> \
  --retention 365d
```

Create the task:

```bash
influx task create \
  --org <YOUR_ORG> \
  --file influxdb/downsample_15m.flux
```

```bash
influx task create \
  --org <YOUR_ORG> \
  --file influxdb/daily_rollup_1d.flux
```

Reset and rebuild the downsample bucket:

```bash
influx task list --name energy-meter-nl-downsample-15m
```

```bash
influx task delete --id <TASK_ID>
```

```bash
influx delete \
  --org <YOUR_ORG> \
  --bucket HomeEnergy_15m \
  --start 1970-01-01T00:00:00Z \
  --stop 2100-01-01T00:00:00Z
```

```bash
influx query \
  --org <YOUR_ORG> \
  --file influxdb/backfill_15m.flux
```

```bash
influx task create \
  --org <YOUR_ORG> \
  --file influxdb/downsample_15m.flux
```

Reset and rebuild the daily totals bucket:

```bash
influx task list --name energy-meter-nl-rollup-1d
```

```bash
influx task delete --id <TASK_ID>
```

```bash
influx delete \
  --org <YOUR_ORG> \
  --bucket HomeEnergy_1d \
  --start 1970-01-01T00:00:00Z \
  --stop 2100-01-01T00:00:00Z
```

```bash
influx query \
  --org <YOUR_ORG> \
  --file influxdb/backfill_1d.flux
```

```bash
influx task create \
  --org <YOUR_ORG> \
  --file influxdb/daily_rollup_1d.flux
```

Aggregation policy:
- task runs every `15m` with `1m` offset and aligns to the previous fully closed `15m` boundary
- `mean` for fast-changing instantaneous fields like power, voltage, and current
- `last` for cumulative counters and state-like fields such as tariff, kWh totals, gas total, and power failure counters
- output timestamps are taken from each window `_stop`, so rebuilt data lands on clean quarter-hour boundaries
- daily task runs every day with `15m` offset
- daily task writes exact counter deltas for `import_t1_kwh`, `import_t2_kwh`, `export_t1_kwh`, `export_t2_kwh`, `gas_m3`

If data sometimes arrives more than 1 minute late, increase the task `offset`
in `downsample_15m.flux`.

Validation tip:
- compare one exact raw 15-minute window in `HomeEnergy` against the corresponding single point in `HomeEnergy_15m`
- a raw `mean()` includes real `0` power samples, which is normally correct for DSMR import/export power
- for year-over-year daily comparisons, query `HomeEnergy_1d` and use `energy_meter_daily`
- build total daily import/export in the query by summing `import_t1_kwh + import_t2_kwh` or `export_t1_kwh + export_t2_kwh`
