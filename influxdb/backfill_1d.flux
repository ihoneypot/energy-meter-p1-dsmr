sourceBucket = "HomeEnergy"
destinationBucket = "HomeEnergy_1d"
sourceMeasurement = "energy_meter_nl"
destinationMeasurement = "energy_meter_daily"

// One-shot historical rebuild for the full raw retention period.
// Narrow startTime if you only want to rebuild a smaller range.
startTime = 1970-01-01T00:00:00Z
stopTime = now()

counterFields = [
    "import_t1_kwh",
    "import_t2_kwh",
    "export_t1_kwh",
    "export_t2_kwh",
    "gas_m3",
]

windowedCounters =
    from(bucket: sourceBucket)
        |> range(start: startTime, stop: stopTime)
        |> filter(fn: (r) => r._measurement == sourceMeasurement)
        |> filter(fn: (r) =>
            r._field == "import_t1_kwh" or
            r._field == "import_t2_kwh" or
            r._field == "export_t1_kwh" or
            r._field == "export_t2_kwh" or
            r._field == "gas_m3"
        )
        |> aggregateWindow(every: 1d, fn: last, createEmpty: false)
        |> difference(nonNegative: true)

windowedCounters
    |> set(key: "_measurement", value: destinationMeasurement)
    |> to(bucket: destinationBucket)
