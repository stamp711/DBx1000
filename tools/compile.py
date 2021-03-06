import os, sys, re, os.path
import platform
import subprocess

def replace(filename, pattern, replacement):
	f = open(filename)
	s = f.read()
	f.close()
	s = re.sub(pattern,replacement,s)
	f = open(filename,'w')
	f.write(s)
	f.close()

dbms_cfg = ["config-std.h", "config.h"]
algs = ['no', 'serial', 'parallel'] 

def insert_his(alg, workload='YCSB', log_type='LOG_DATA', recovery='false', 
  			   gc='false', ramdisc='true', max_txn=100000):
	if alg == 'no':
		name = 'NO'
	else :
		name = 'S' if alg == 'serial' else 'P'
		if alg == 'serial': name = 'S'
		elif alg == 'parallel': name = 'P'
		elif alg == 'batch': name = 'B'
		else:  assert(False)
		
		if log_type == 'LOG_DATA':
			name += 'D'
		elif log_type == 'LOG_COMMAND':
			name += 'C'
		else:
			assert(False)
	name += '_%s' % workload
	jobs[name] = {}
	jobs[name]["LOG_ALGORITHM"] = "LOG_%s" % alg.upper()
	jobs[name]["WORKLOAD"] = workload
	jobs[name]["LOG_TYPE"] = log_type
	#jobs[name]["LOG_RECOVER"] = recovery
	#jobs[name]["LOG_GARBAGE_COLLECT"] = gc
	#jobs[name]["LOG_RAM_DISC"] = ramdisc
	#jobs[name]["MAX_TXN_PER_THREAD"] = max_txn

jobs = {}
benchmarks = ['YCSB']
benchmarks = ['TPCC']
benchmarks = ['YCSB', 'TPCC']
for bench in benchmarks:
	#insert_his('serial', bench, 'LOG_DATA')
	#insert_his('serial', bench, 'LOG_COMMAND')
	insert_his('parallel', bench, 'LOG_DATA')
	insert_his('parallel', bench, 'LOG_COMMAND')
	insert_his('batch', bench, 'LOG_DATA')
	#insert_his('no', bench)
	
for (jobname, v) in jobs.iteritems():
	os.system("cp "+ dbms_cfg[0] +' ' + dbms_cfg[1])
	for (param, value) in v.iteritems():
		pattern = r"\#define\s*" + re.escape(param) + r'.*'
		replacement = "#define " + param + ' ' + str(value)
		replace(dbms_cfg[1], pattern, replacement)
	
	command = "make clean; make -j8; cp rundb rundb_%s" % (jobname)
	print "start to compile " + jobname
	proc = subprocess.Popen(command, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
	while proc.poll() is None:
		#print proc.stdout.readline() 
		commandResult = proc.wait() #catch return code
		#print commandResult
		if commandResult != 0:
			print "Error in job. " + jobname 
			print "Please run 'make' to debug."
			exit(0)
		else:
			print jobname + " compile done!"
