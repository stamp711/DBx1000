import os, csv
import job

cs = ["job"]
thread_cnt = [1, 2, 4, 6, 8, 10, 12, 14, 16, 18]
job_name = list(job.workloads.keys())

file = open("results.csv", "w", newline="")
writer = csv.DictWriter(file, fieldnames=cs + thread_cnt)
writer.writeheader()

throughput, latency = {}, {}
job_i = 0
for line in open("run-results.txt"):
    if "==>" in line:
        throughput, latency = {}, {}
        cnt, i = 0, 0
    elif "=>" in line:
        if cnt < 1:
            cnt += 1
        else:
            throughput[thread_cnt[i]] = float(line.split()[1])
            latency[thread_cnt[i]] = round(float(line.split()[2]), 3)
            cnt = 0
            i += 1
            if i == 10:
                print(throughput)
                print(latency)
                jobname = {"job": job_name[job_i]}
                writer.writerows([{**jobname, **throughput}, {**jobname, **latency}])
                job_i += 1
