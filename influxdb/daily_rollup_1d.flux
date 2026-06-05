option task = {
    name: "energy-meter-nl-rollup-1d",
    every: 1d,
    offset: 15m,
}

sourceBucket = "HomeEnergy"
destinationBucket = "HomeEnergy_1d"
sourceMeasurement = "energy_meter_nl"
destinationMeasurement = "energy_meter_daily"

counterFields = [
    "import_t1_kwh",
    "import_t2_kwh",
    "export_t1_kwh",
    "export_t2_kwh",
    "gas_m3",
]

windowedCounters =
    from(bucket: sourceBucket)
        // With a 15-minute task offset, stop at -15m so the current partial
        // day is excluded and only the previous fully closed day is rolled up.
        |> range(start: -3d, stop: -15m)
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
    |> tail(n: 1)
    |> set(key: "_measurement", value: destinationMeasurement)
    |> to(bucket: destinationBucket)
