import "date"

option task = {
    name: "energy-meter-nl-downsample-15m",
    every: 15m,
    offset: 1m,
}

sourceBucket = "HomeEnergy"
destinationBucket = "HomeEnergy_15m"
measurement = "energy_meter_nl"

// Align to the previous fully closed 15-minute boundary. This avoids partial
// windows if the task itself runs a few seconds late.
windowStop = date.truncate(t: date.sub(from: now(), d: task.offset), unit: task.every)
windowStart = date.sub(from: windowStop, d: task.every)

meanFields = [
    "power_delivered_kw",
    "power_returned_kw",
    "voltage_l1_v",
    "voltage_l2_v",
    "voltage_l3_v",
    "current_l1_a",
    "current_l2_a",
    "current_l3_a",
    "power_delivered_l1_kw",
    "power_delivered_l2_kw",
    "power_delivered_l3_kw",
    "power_returned_l1_kw",
    "power_returned_l2_kw",
    "power_returned_l3_kw",
]

lastFields = [
    "tariff",
    "power_failures",
    "long_power_failures",
    "import_t1_kwh",
    "import_t2_kwh",
    "export_t1_kwh",
    "export_t2_kwh",
    "gas_m3",
]

base =
    from(bucket: sourceBucket)
        |> range(start: windowStart, stop: windowStop)
        |> filter(fn: (r) => r._measurement == measurement)

base
    |> filter(fn: (r) => contains(value: r._field, set: meanFields))
    |> aggregateWindow(every: task.every, fn: mean, createEmpty: false, timeSrc: "_stop")
    |> to(bucket: destinationBucket)

base
    |> filter(fn: (r) => contains(value: r._field, set: lastFields))
    |> aggregateWindow(every: task.every, fn: last, createEmpty: false, timeSrc: "_stop")
    |> to(bucket: destinationBucket)
