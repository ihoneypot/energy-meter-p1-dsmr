sourceBucket = "SolarEdge"
destinationBucket = "SolarEdge_15m"
measurement = "solar_edge"

// One-shot historical rebuild for the full raw retention period.
// Narrow startTime if you only want to rebuild a smaller range.
startTime = 1970-01-01T00:00:00Z
stopTime = now()

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
        |> range(start: startTime, stop: stopTime)
        |> filter(fn: (r) => r._measurement == measurement)

base
    |> filter(fn: (r) => contains(value: r._field, set: meanFields))
    |> aggregateWindow(every: 15m, fn: mean, createEmpty: false, timeSrc: "_stop")
    |> to(bucket: destinationBucket)

base
    |> filter(fn: (r) => contains(value: r._field, set: lastFields))
    |> aggregateWindow(every: 15m, fn: last, createEmpty: false, timeSrc: "_stop")
    |> to(bucket: destinationBucket)
