import "date"

option task = {
    name: "solaredge-downsample-15m",
    every: 15m,
    offset: 1m,
}

sourceBucket = "SolarEdge"
destinationBucket = "SolarEdge_15m"
measurement = "solar_edge"

// Align the task to the previous fully closed 15-minute window so partial
// writes from the current quarter-hour are never mixed into the rollup.
windowStop = date.truncate(t: date.sub(from: now(), d: task.offset), unit: task.every)
windowStart = date.sub(from: windowStop, d: task.every)

meanFields = [
    "I_AC_Current",
    "I_AC_CurrentA",
    "I_AC_CurrentB",
    "I_AC_CurrentC",
    "I_AC_VoltageAB",
    "I_AC_VoltageBC",
    "I_AC_VoltageCA",
    "I_AC_VoltageAN",
    "I_AC_VoltageBN",
    "I_AC_VoltageCN",
    "I_AC_Power",
    "I_AC_Frequency",
    "I_AC_VA",
    "I_AC_VAR",
    "I_AC_PF",
    "I_DC_Current",
    "I_DC_Voltage",
    "I_DC_Power",
    "I_Temp_Sink",
]

lastFields = [
    "C_SunSpec_DID",
    "C_SunSpec_Length",
    "I_AC_Current_SF",
    "I_AC_Voltage_SF",
    "I_AC_Power_SF",
    "I_AC_Frequency_SF",
    "I_AC_VA_SF",
    "I_AC_VAR_SF",
    "I_AC_PF_SF",
    "I_AC_Energy_WH",
    "I_AC_Energy_WH_SF",
    "I_DC_Current_SF",
    "I_DC_Voltage_SF",
    "I_DC_Power_SF",
    "I_Temp_SF",
    "I_Status",
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
