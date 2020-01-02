workloads = {}

workloads["YCSB+RH"] = {
    "WORKLOAD": "YCSB",
    "READ_PERC": 0.9,
    "WRITE_PERC": 0.1,
}
workloads["YCSB+RW"] = {
    "WORKLOAD": "YCSB",
    "READ_PERC": 0.5,
    "WRITE_PERC": 0.5,
}
workloads["YCSB+WH"] = {
    "WORKLOAD": "YCSB",
    "READ_PERC": 0.1,
    "WRITE_PERC": 0.9,
}

workloads["TPCC+1WH"] = {"WORKLOAD": "TPCC", "NUM_WH": 1}
workloads["TPCC+4WH"] = {"WORKLOAD": "TPCC", "NUM_WH": 1}
workloads["TPCC+XWH"] = {"WORKLOAD": "TPCC", "NUM_WH": "THREAD_CNT"}


threads = {}
threads["01"] = {"THREAD_CNT": 1, "NUM_LOGGER": 1}
threads["02"] = {"THREAD_CNT": 2, "NUM_LOGGER": 2}
threads["04"] = {"THREAD_CNT": 4, "NUM_LOGGER": 4}
threads["06"] = {"THREAD_CNT": 6, "NUM_LOGGER": 4}
threads["08"] = {"THREAD_CNT": 8, "NUM_LOGGER": 4}
threads["10"] = {"THREAD_CNT": 10, "NUM_LOGGER": 4}
threads["12"] = {"THREAD_CNT": 12, "NUM_LOGGER": 4}
threads["14"] = {"THREAD_CNT": 14, "NUM_LOGGER": 4}
threads["16"] = {"THREAD_CNT": 16, "NUM_LOGGER": 4}
threads["18"] = {"THREAD_CNT": 18, "NUM_LOGGER": 4}
