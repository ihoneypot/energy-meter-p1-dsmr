option task = {
    name: "solaredge-rollup-1d",
    every: 1d,
    offset: 15m,
}

sourceBucket = "SolarEdge"
destinationBucket = "SolarEdge_1d"
sourceMeasurement = "solar_edge"
destinationMeasurement = "solar_edge_daily"

windowedEnergy =
    from(bucket: sourceBucket)
        // Stop at -15m so the current partial day is excluded and only the
        // previous fully closed day is rolled up.
        |> range(start: -3d, stop: -15m)
        |> filter(fn: (r) => r._measurement == sourceMeasurement)
        |> filter(fn: (r) => r._field == "I_AC_Energy_WH")
        |> aggregateWindow(every: 1d, fn: last, createEmpty: false)
        |> difference(nonNegative: true)

windowedEnergy
    |> tail(n: 1)
    |> set(key: "_measurement", value: destinationMeasurement)
    |> to(bucket: destinationBucket)
