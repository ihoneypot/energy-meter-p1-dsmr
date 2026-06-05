sourceBucket = "SolarEdge"
destinationBucket = "SolarEdge_1d"
sourceMeasurement = "solar_edge"
destinationMeasurement = "solar_edge_daily"

// One-shot historical rebuild for the full raw retention period.
// Narrow startTime if you only want to rebuild a smaller range.
startTime = 1970-01-01T00:00:00Z
stopTime = now()

windowedEnergy =
    from(bucket: sourceBucket)
        |> range(start: startTime, stop: stopTime)
        |> filter(fn: (r) => r._measurement == sourceMeasurement)
        |> filter(fn: (r) => r._field == "I_AC_Energy_WH")
        |> aggregateWindow(every: 1d, fn: last, createEmpty: false)
        |> difference(nonNegative: true)

windowedEnergy
    |> set(key: "_measurement", value: destinationMeasurement)
    |> to(bucket: destinationBucket)
