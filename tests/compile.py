import os, re
import job


def replace(filename, pattern, replacement):
    f = open(filename)
    s = f.read()
    f.close()
    s = re.sub(pattern, replacement, s)
    f = open(filename, "w")
    f.write(s)
    f.close()


def compile(job_name, job_config):
    os.system("cp config-std.h config.h")
    for (param, value) in job_config.items():
        pattern = r"\#define\s*" + re.escape(param) + r".*"
        replacement = "#define " + param + " " + str(value)
        replace("config.h", pattern, replacement)
    os.system("make clean > /dev/null 2>&1")
    ret = os.system("make -j > /dev/null 2>&1")
    if ret != 0:
        print("ERROR in compiling job ", job_name)
        print(job_config)
        exit(-1)
    else:
        os.system("rm config.h")
        os.system("mv rundb bin/rundb_" + job_name)


os.makedirs("bin", exist_ok=True)

for (workload_name, workload_config) in job.workloads.items():
    print("=> Workload", workload_name, workload_config)
    for (threads_name, threads_config) in job.threads.items():
        job_name = workload_name + "+" + threads_name
        job_config = {**workload_config, **threads_config}
        print(" | Job", job_name, job_config)
        compile(job_name, job_config)

os.system("make clean > /dev/null 2>&1")
