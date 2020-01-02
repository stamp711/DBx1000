import os, platform, shutil
import job

optane_workdir = "/mnt/pmem0/stamp/DBx1000/workdir"


def run(job_name, cnt):

    sum_throughput = 0.0
    sum_latency = 0.0

    for i in range(0, cnt):
        # Remove old DB files
        if platform.node() == "optane":
            shutil.rmtree(optane_workdir, ignore_errors=True)
            os.makedirs(optane_workdir, exist_ok=True)
        else:
            os.system("rm *_log*.log* > /dev/null 2>&1")
        # Run once
        log_file = "results/" + job_name + "_" + str(i)
        os.system("./bin/rundb_" + job_name + " > " + log_file)
        throughput, latency = parse(log_file)
        print("   " + str(i) + ":", throughput, latency)
        # Incr counter
        sum_throughput += throughput
        sum_latency += latency

    # Remove DB files
    if platform.node() == "optane":
        shutil.rmtree(optane_workdir, ignore_errors=True)
    else:
        os.system("rm *_log*.log* > /dev/null 2>&1")

    avg_throughput = sum_throughput / cnt
    avg_latency = sum_latency / cnt
    print("   =>", avg_throughput, avg_latency)


def parse(file):
    for line in open(file):
        if "Throughput:" in line:
            throughput = float(line.split()[1])
        if "latency:" in line:
            latency = float(line.split()[1])
    return (throughput, latency)


os.makedirs("results", exist_ok=True)

for (workload_name, workload_config) in job.workloads.items():
    print("==> Workload", workload_name, workload_config)
    for (threads_name, threads_config) in job.threads.items():
        job_name = workload_name + "+" + threads_name
        job_config = {**workload_config, **threads_config}
        print(" =>", job_name, job_config)
        run(job_name, 5)
