#include "txn.h"
#include "row.h"
#include "wl.h"
#include "ycsb.h"
#include "thread.h"
#include "mem_alloc.h"
#include "occ.h"
#include "table.h"
#include "catalog.h"
#include "index_btree.h"
#include "index_hash.h"
#include "log.h"
#include "serial_log.h"
#include "parallel_log.h"
#include "log_recover_table.h"
#include "log_pending_table.h"
#include "free_queue.h"
#include "manager.h"
#include <fcntl.h>


#if LOG_ALGORITHM == LOG_BATCH
pthread_mutex_t * txn_man::_log_lock;
#endif

void txn_man::init(thread_t * h_thd, workload * h_wl, uint64_t thd_id) {
	this->h_thd = h_thd;
	this->h_wl = h_wl;
	pthread_mutex_init(&txn_lock, NULL);
	lock_ready = false;
	ready_part = 0;
	row_cnt = 0;
	wr_cnt = 0;
	insert_cnt = 0;
	accesses = (Access **) _mm_malloc(sizeof(Access *) * MAX_ROW_PER_TXN, 64);
	write_set = (uint32_t *) _mm_malloc(sizeof(uint32_t) * MAX_ROW_PER_TXN, 64);
	for (int i = 0; i < MAX_ROW_PER_TXN; i++)
		accesses[i] = NULL;
	num_accesses_alloc = 0;
#if CC_ALG == TICTOC || CC_ALG == SILO
	_pre_abort = g_pre_abort; 
	_validation_no_wait = true;
#endif
#if CC_ALG == TICTOC
	_max_wts = 0;
	_min_cts = 0;
	_write_copy_ptr = false; //(g_write_copy_form == "ptr");
	_atomic_timestamp = g_atomic_timestamp;
#elif CC_ALG == SILO
	_cur_tid = 0;
#endif
	_last_epoch_time = 0;	
	_log_entry_size = 0;

#if LOG_ALGORITHM == LOG_PARALLEL
	_num_raw_preds = 0;
	_num_waw_preds = 0;
//	_predecessor_info = new PredecessorInfo;	
//	for (uint32_t i = 0; i < 4; i++)
//		aggregate_pred_vector[i] = 0;
#endif
	_log_entry = new char [MAX_LOG_ENTRY_SIZE];
	_log_entry_size = 0;
	_txn_state_queue = new queue<TxnState> * [g_thread_cnt];
	for (uint32_t i = 0; i < g_thread_cnt; i++) {
		_txn_state_queue[i] = (queue<TxnState> *) _mm_malloc(sizeof(queue<TxnState>), 64);
		new (_txn_state_queue[i]) queue<TxnState>();
	}
}

void txn_man::set_txn_id(txnid_t txn_id) {
	this->txn_id = txn_id;
}

txnid_t txn_man::get_txn_id() {
	return this->txn_id;
}

workload * txn_man::get_wl() {
	return h_wl;
}

uint64_t txn_man::get_thd_id() {
	return h_thd->get_thd_id();
}

void txn_man::set_ts(ts_t timestamp) {
	this->timestamp = timestamp;
}

ts_t txn_man::get_ts() {
	return this->timestamp;
}

RC txn_man::cleanup(RC in_rc) 
{
	RC rc = in_rc;
#if CC_ALG == HEKATON
	row_cnt = 0;
	wr_cnt = 0;
	insert_cnt = 0;
	return;
#endif
	for (int rid = row_cnt - 1; rid >= 0; rid --) {
		row_t * orig_r = accesses[rid]->orig_row;
		access_t type = accesses[rid]->type;
		if (type == WR && rc == Abort)
			type = XP;

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
		if (type == RD) {
			accesses[rid]->data = NULL;
			continue;
		}
#endif
		if (ROLL_BACK && type == XP &&
					(CC_ALG == DL_DETECT || 
					CC_ALG == NO_WAIT || 
					CC_ALG == WAIT_DIE)) 
		{
			orig_r->return_row(type, this, accesses[rid]->orig_data);
		} else {
			orig_r->return_row(type, this, accesses[rid]->data);
		}
#if CC_ALG != TICTOC && CC_ALG != SILO
		accesses[rid]->data = NULL;
#endif
	}

	if (rc == Abort) {
		for (UInt32 i = 0; i < insert_cnt; i ++) {
			row_t * row = insert_rows[i];
			assert(g_part_alloc == false);
#if CC_ALG != HSTORE && CC_ALG != OCC
			mem_allocator.free(row->manager, 0);
#endif
			row->free_row();
			mem_allocator.free(row, sizeof(row));
		}
	}
	// Logging
	//printf("log??\n");
#if LOG_ALGORITHM != LOG_NO
	if (rc == RCOK)
	{
//		if (wr_cnt > 0) {
	    {
			uint64_t before_log_time = get_sys_clock();
			//uint32_t size = _log_entry_size;
			assert(_log_entry_size != 0);
  #if LOG_ALGORITHM == LOG_SERIAL
			//	_max_lsn: max LSN for predecessors
			//  _cur_tid: LSN for the log record of the current txn 
  			uint64_t max_lsn = max(_max_lsn, _cur_tid);
			if (max_lsn <= log_manager->get_persistent_lsn()) {
				INC_INT_STATS(num_latency_count, 1);
				INC_FLOAT_STATS(latency, get_sys_clock() - _txn_start_time);
			} else {
				queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
				TxnState state;
				state.max_lsn = max_lsn;
				state.start_time = _txn_start_time;
				state.wait_start_time = get_sys_clock();
				state_queue->push(state);
			}
  #elif LOG_ALGORITHM == LOG_PARALLEL
			bool success = true;
			// check own log record  
			uint32_t logger_id = _cur_tid >> 48;
			uint64_t lsn = (_cur_tid << 16) >> 16;
			if (lsn > log_manager[logger_id]->get_persistent_lsn())
				success = false;
			if (success) {
				for (uint32_t i=0; i < _num_raw_preds; i++)  {
					if (_raw_preds_tid[i] == (uint64_t)-1) continue;
					logger_id = _raw_preds_tid[i] >> 48;
					lsn = (_raw_preds_tid[i] << 16) >> 16;
					if (lsn > log_manager[logger_id]->get_persistent_lsn()) { 
						success = false;
						break;
					}
				} 
			}
			if (success) {
				for (uint32_t i=0; i < _num_waw_preds; i++)  {
					if (_waw_preds_tid[i] == (uint64_t)-1) continue;
					logger_id = _waw_preds_tid[i] >> 48;
					lsn = (_waw_preds_tid[i] << 16) >> 16;
					if (lsn > log_manager[logger_id]->get_persistent_lsn()) { 
						success = false;
						break;
					}
				} 		
			}
			if (success) { 
				INC_INT_STATS(num_latency_count, 1);
				INC_FLOAT_STATS(latency, get_sys_clock() - _txn_start_time);
			} else {
				queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
				TxnState state;
				for (uint32_t i = 0; i < g_num_logger; i ++)
					state.preds[i] = 0;
				// calculate the compressed preds
				uint32_t logger_id = _cur_tid >> 48;
				uint64_t lsn = (_cur_tid << 16) >> 16;
				if (lsn > state.preds[logger_id])
					state.preds[logger_id] = lsn;
				for (uint32_t i=0; i < _num_raw_preds; i++)  {
					if (_raw_preds_tid[i] == (uint64_t)-1) continue;
					logger_id = _raw_preds_tid[i] >> 48;
					lsn = (_raw_preds_tid[i] << 16) >> 16;
					if (lsn > state.preds[logger_id])
						state.preds[logger_id] = lsn;
				} 
				for (uint32_t i=0; i < _num_waw_preds; i++)  {
					if (_waw_preds_tid[i] == (uint64_t)-1) continue;
					logger_id = _waw_preds_tid[i] >> 48;
					lsn = (_waw_preds_tid[i] << 16) >> 16;
					if (lsn > state.preds[logger_id])
						state.preds[logger_id] = lsn;
				} 
				state.start_time = _txn_start_time;
				//memcpy(state.preds, _preds, sizeof(uint64_t) * g_num_logger);
				state.wait_start_time = get_sys_clock();
				state_queue->push(state);
			}
  #elif LOG_ALGORITHM == LOG_BATCH
  			uint64_t flushed_epoch = (uint64_t)-1;

			for (uint32_t i = 0; i < g_num_logger; i ++) {
				uint64_t max_epoch = glob_manager->get_persistent_epoch(i);
				if (max_epoch < flushed_epoch)
					flushed_epoch = max_epoch; 
			}
			//printf("flushed_epoch= %ld\n", flushed_epoch);
			if (_epoch <= flushed_epoch) {
				INC_INT_STATS(num_latency_count, 1);
				INC_FLOAT_STATS(latency, get_sys_clock() - _txn_start_time);
			} else {
				queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
				TxnState state;
				state.epoch = _epoch;
				state.start_time = _txn_start_time;
				state.wait_start_time = get_sys_clock();
				state_queue->push(state);
			}
  #endif
			uint64_t after_log_time = get_sys_clock();
			INC_FLOAT_STATS(time_log, after_log_time - before_log_time);
		}
	}
	try_commit_txn();
#else // LOG_ALGORITHM == LOG_NO
	INC_INT_STATS(num_latency_count, 1);
	INC_FLOAT_STATS(latency, get_sys_clock() - _txn_start_time);
#endif
	_log_entry_size = 0;
	row_cnt = 0;
	wr_cnt = 0;
	insert_cnt = 0;
#if LOG_ALGORITHM == LOG_PARALLEL
	_num_raw_preds = 0;
	_num_waw_preds = 0;
#elif LOG_ALGORITHM == LOG_SERIAL
	_max_lsn = 0;
#endif
#if CC_ALG == DL_DETECT
	dl_detector.clear_dep(get_txn_id());
#endif
	return rc;
}

void 			
txn_man::try_commit_txn()
{
#if LOG_ALGORITHM == LOG_SERIAL
	bool success = true;
	queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
	while (!state_queue->empty() && success) 
	{
		TxnState state = state_queue->front();
		if (state.max_lsn > log_manager->get_persistent_lsn()) { 
			success = false;
			break;
		}
		if (success) {
			uint64_t lat = get_sys_clock() - state.start_time;
			INC_FLOAT_STATS(latency, lat);
			INC_INT_STATS(num_latency_count, 1);
			state_queue->pop();
		}
	}
#elif LOG_ALGORITHM == LOG_PARALLEL
	bool success = true;
	queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
	while (!state_queue->empty() && success) 
	{
		TxnState state = state_queue->front();
		for (uint32_t i=0; i < g_num_logger; i++)  {
			if (state.preds[i] > log_manager[i]->get_persistent_lsn()) { 
				success = false;
				break;
			}
		}
		if (success) {
			INC_INT_STATS(num_latency_count, 1);
			INC_FLOAT_STATS(latency, get_sys_clock() - state.start_time);
			state_queue->pop();
		}
	}
#elif LOG_ALGORITHM == LOG_BATCH
  	uint64_t flushed_epoch = (uint64_t)-1;
	for (uint32_t i = 0; i < g_num_logger; i ++) {
		uint64_t max_epoch = glob_manager->get_persistent_epoch(i);
		if (max_epoch < flushed_epoch)
			flushed_epoch = max_epoch; 
	}
	bool success = true;
	queue<TxnState> * state_queue = _txn_state_queue[GET_THD_ID];
	while (!state_queue->empty() && success) 
	{
		TxnState state = state_queue->front();
		if (state.epoch > flushed_epoch) { 
			success = false;
			break;
		}
		if (success) {
			INC_INT_STATS(num_latency_count, 1);
			INC_FLOAT_STATS(latency, get_sys_clock() - state.start_time);
			state_queue->pop();
		}
	}
#endif
}


RC txn_man::get_row(row_t * row, access_t type, char * &data) {
	// NOTE. 
	// For recovery, no need to go through concurrncy control
	if (g_log_recover) {
		data = row->get_data(this, type);
		return RCOK;
	}

	if (CC_ALG == HSTORE) {
		data = row->get_data();
		return RCOK;
	}
	uint64_t starttime = get_sys_clock();
	RC rc = RCOK;
	if (accesses[row_cnt] == NULL) {
		Access * access = (Access *) _mm_malloc(sizeof(Access), 64);
		accesses[row_cnt] = access;
#if (CC_ALG == SILO || CC_ALG == TICTOC)
		access->data = new char [MAX_TUPLE_SIZE];
		access->orig_data = NULL;
#elif (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
		access->orig_data = (row_t *) _mm_malloc(sizeof(row_t), 64);
		access->orig_data->init(MAX_TUPLE_SIZE);
#endif
		num_accesses_alloc ++;
	}
	
	rc = row->get_row(type, this, accesses[ row_cnt ]->data);
	if (rc == Abort) {
		return Abort;
	}
	accesses[row_cnt]->type = type;
	accesses[row_cnt]->orig_row = row;
#if CC_ALG == TICTOC
	accesses[row_cnt]->wts = last_wts;
	accesses[row_cnt]->rts = last_rts;
#elif CC_ALG == SILO
	accesses[row_cnt]->tid = last_tid;
#elif CC_ALG == HEKATON
	accesses[row_cnt]->history_entry = history_entry;
#endif

#if ROLL_BACK && (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
	// orig_data should be char *
	assert(false);
	if (type == WR) {
		accesses[row_cnt]->orig_data->table = row->get_table();
		accesses[row_cnt]->orig_data->copy(row);
	}
#endif

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
	if (type == RD)
		row->return_row(type, this, accesses[ row_cnt ]->data);
#endif
	
	row_cnt ++;
	if (type == WR)
		wr_cnt ++;

	uint64_t timespan = get_sys_clock() - starttime;
	INC_FLOAT_STATS(time_man, timespan);
	data = accesses[row_cnt - 1]->data;
	return RCOK;
}

void txn_man::insert_row(row_t * row, table_t * table) {
	if (CC_ALG == HSTORE)
		return;
	assert(insert_cnt < MAX_ROW_PER_TXN);
	insert_rows[insert_cnt ++] = row;
}

itemid_t *
txn_man::index_read(INDEX * index, idx_key_t key, int part_id) {
	itemid_t * item;
	index->index_read(key, item, part_id, get_thd_id());
	return item;
}

void 
txn_man::index_read(INDEX * index, idx_key_t key, int part_id, itemid_t *& item) {
	uint64_t starttime = get_sys_clock();
	index->index_read(key, item, part_id, get_thd_id());
	INC_FLOAT_STATS(time_index, get_sys_clock() - starttime);
}

RC txn_man::finish(RC rc) {
	assert(!g_log_recover);
#if CC_ALG == HSTORE
	return RCOK;
#endif
	uint64_t starttime = get_sys_clock();
#if CC_ALG == OCC
	if (rc == RCOK)
		rc = occ_man.validate(this);
	else 
		cleanup(rc);
#elif CC_ALG == TICTOC
	if (rc == RCOK) {
		rc = validate_tictoc();
	} else 
		rc = cleanup(rc);
#elif CC_ALG == SILO
	if (rc == RCOK)
		rc = validate_silo();
	else 
		cleanup(rc);
#elif CC_ALG == HEKATON
	rc = validate_hekaton(rc);
	cleanup(rc);
#else 
	cleanup(rc);
#endif
	uint64_t timespan = get_sys_clock() - starttime;
	INC_FLOAT_STATS(time_man, timespan);
	INC_FLOAT_STATS(time_cleanup,  timespan);
	return rc;
}

void
txn_man::release() {
	for (int i = 0; i < num_accesses_alloc; i++)
		mem_allocator.free(accesses[i], 0);
	mem_allocator.free(accesses, 0);
}

// Recovery for data logging
void 
txn_man::recover() {
#if LOG_ALGORITHM == LOG_SERIAL
	serial_recover();
#elif LOG_ALGORITHM == LOG_PARALLEL
	parallel_recover();
#elif LOG_ALGORITHM == LOG_BATCH
	batch_recover();
#endif
}

#if LOG_ALGORITHM == LOG_SERIAL 
void 
txn_man::serial_recover() {
	char default_entry[MAX_LOG_ENTRY_SIZE];
	// right now, only a single thread does the recovery job.
	if (GET_THD_ID > 0)
		return;
	uint32_t count = 0;
	while (true) {
		char * entry = default_entry;
		uint64_t tt = get_sys_clock();
		uint64_t lsn = log_manager->get_next_log_entry(entry);
		if (entry == NULL) {
			if (log_manager->iseof()) {
				lsn = log_manager->get_next_log_entry(entry);
				if (entry == NULL)
					break;
			}
			else { 
				PAUSE //usleep(50);
				INC_FLOAT_STATS(time_io, get_sys_clock() - tt);
				continue;
			}
		}
		INC_FLOAT_STATS(time_io, get_sys_clock() - tt);
		// Format for serial logging
		// | checksum | size | ... |
		assert(*(uint32_t*)entry == 0xdead);
        recover_txn(entry + sizeof(uint32_t) * 2);
		//printf("size=%d lsn=%ld\n", *(uint32_t*)(entry+4), lsn);
		COMPILER_BARRIER
		log_manager->set_gc_lsn(lsn);
		INC_INT_STATS(num_commits, 1);
		count ++;
	}
}

#elif LOG_ALGORITHM == LOG_PARALLEL

void 
txn_man::parallel_recover() {
	// Execution thread.
	// Phase 1: Construct the dependency graph from the log records. 
	//   Phase 1.1. read in all log records, each record only having predecessor info.   
	if (GET_THD_ID == 0)
		printf("Phase 1.1 starts\n");
	uint64_t tt = get_sys_clock();
	uint32_t logger = GET_THD_ID % g_num_logger;
	while (true) {
		char * buffer = NULL;
		uint64_t file_size = 0;
		uint64_t base_lsn = 0;
		uint64_t tt = get_sys_clock();
		uint32_t chunk_num = log_manager[logger]->get_next_log_chunk(buffer, file_size, base_lsn);
		INC_FLOAT_STATS(time_io, get_sys_clock() - tt);
		INC_FLOAT_STATS(log_bytes, file_size);
		if (chunk_num == (uint32_t)-1) 
			break;
	
		// Format of log record 
		// | checksum | size | ... 
		uint32_t offset = 0;
		uint64_t lsn = base_lsn;
		tt = get_sys_clock();
		while (offset < file_size) {
			// read entries from buffer
			uint32_t checksum;
			uint32_t size = 0; 
			uint32_t start = offset;
			if (UNLIKELY(start + sizeof(uint32_t) * 2 >= file_size)) {
//				printf("[1] logger=%d. chunknum=%d LSN=%ld. offset=%d, size=%d, file_size=%ld\n", 
//					logger, chunk_num, lsn, offset, size, file_size);
				break;
			}
			UNPACK(buffer, checksum, offset);
			UNPACK(buffer, size, offset);
			if (UNLIKELY(start + size > file_size)) {
//				printf("[2] logger=%d. chunk=%d LSN=%ld. offset=%d, size=%d, file_size=%ld\n", 
//					logger, chunk_num, lsn, offset, size, file_size);
				break;
			}
			if (UNLIKELY(checksum != 0xdead)) { 
//				printf("logger=%d. chunk=%d LSN=%ld. txn lost\n", logger, chunk_num, lsn);
				break;
			}
			M_ASSERT(size > 0 && size <= MAX_LOG_ENTRY_SIZE, "size=%d\n", size);
			uint64_t tid = ((uint64_t)logger << 48) | lsn;
			log_recover_table->addTxn(tid, buffer + start);
		
			//COMPILER_BARRIER
			offset = start + size;
			lsn += size; 
			M_ASSERT(offset <= file_size, "offset=%d, file_size=%ld\n", offset, file_size);
		}
		INC_FLOAT_STATS(time_phase1_add_graph, get_sys_clock() - tt);
		log_manager[logger]->return_log_chunk(buffer, chunk_num);
	}

	INC_FLOAT_STATS(time_phase1_1_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);
	INC_FLOAT_STATS(time_phase1_1, get_sys_clock() - tt);
/*	tt = get_sys_clock();
	if (GET_THD_ID == 0)
		printf("Phase 1.2 starts\n");
	// Phase 1.2. add in the successor info to the graph.
	log_recover_table->buildSucc();
	
	INC_FLOAT_STATS(time_phase1_2_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);
	INC_FLOAT_STATS(time_phase1_2, get_sys_clock() - tt);
*/
	tt = get_sys_clock();
	
	if (GET_THD_ID == 0)
		printf("Phase 2 starts\n");
	// Phase 2. Infer WAR edges.   
	log_recover_table->buildWARSucc(); 
	
	INC_FLOAT_STATS(time_phase2_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);
	INC_FLOAT_STATS(time_phase2, get_sys_clock() - tt);
	tt = get_sys_clock();

	if (GET_THD_ID == 0)
		printf("Phase 3 starts\n");
	// Phase 3. Recover transactions
	// XXX the following termination detection is a HACK
	// Basically if no thread has seen a new txn in 100 us,
	// the program is terminated.
	bool vote_done = false;
	uint64_t last_idle_time = 0; //get_sys_clock();
	while (true) { 
		char * log_entry = NULL;
		void * node = log_recover_table->get_txn(log_entry);		
		if (log_entry) {
			if (vote_done) {
		        ATOM_SUB_FETCH(GET_WORKLOAD->sim_done, 1);
				vote_done = false;
			}
			last_idle_time = 0;
			do {
            	recover_txn(log_entry);
				void * next = NULL;
				log_entry = NULL;
				log_recover_table->remove_txn(node, log_entry, next);
				node = next;
				INC_INT_STATS(num_commits, 1);
			} while (log_entry);
		} else { //if (log_recover_table->is_recover_done()) {
			if (last_idle_time == 0)
				last_idle_time = get_sys_clock();
			PAUSE
			if (!vote_done && get_sys_clock() - last_idle_time > 1 * 1000 * 1000) {
				vote_done = true;
		       	ATOM_ADD_FETCH(GET_WORKLOAD->sim_done, 1);
			}
			if (GET_WORKLOAD->sim_done == g_thread_cnt)
				break;
		}
	}

	INC_FLOAT_STATS(time_phase3_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);
	INC_FLOAT_STATS(time_phase3, get_sys_clock() - tt);
}
#elif LOG_ALGORITHM == LOG_BATCH
void
txn_man::batch_recover()
{
//	if (GET_THD_ID == 0) {
//		_log_lock = new pthread_mutex_t;
//		pthread_mutex_init(_log_lock, NULL);
//	}
	pthread_barrier_wait(&worker_bar);
	
	uint64_t tt = get_sys_clock();
	uint32_t logger = GET_THD_ID % g_num_logger;
	while (true) {
		char * buffer = NULL;
		uint64_t file_size = 0;
		uint64_t base_lsn = 0;
		uint64_t tt = get_sys_clock();
		uint32_t chunk_num = log_manager[logger]->get_next_log_chunk(buffer, file_size, base_lsn);
		INC_FLOAT_STATS(time_io, get_sys_clock() - tt);
		//INC_FLOAT_STATS(time_debug1, get_sys_clock() - tt);
		INC_FLOAT_STATS(log_bytes, file_size);
		if (chunk_num == (uint32_t)-1) 
			break;
		assert(buffer);
		// Format of log record 
		// | checksum | size | ... 
		uint32_t offset = 0;
		tt = get_sys_clock();
		while (offset < file_size) {
			// read entries from buffer
			uint32_t checksum;
			uint32_t size; 
			uint64_t tid;
			uint32_t start = offset;
			UNPACK(buffer, checksum, offset);
			UNPACK(buffer, size, offset);
			UNPACK(buffer, tid, offset);
			if (checksum != 0xdead) {
				//printf("checksum=%x, offset=%d, fsize=%d\n", checksum, offset, fsize);
				break;
			}
			
			recover_txn(buffer + offset);
			INC_INT_STATS(num_commits, 1);
			
			offset = start + size;
		}
		INC_FLOAT_STATS(time_debug2, get_sys_clock() - tt);
		log_manager[logger]->return_log_chunk(buffer, chunk_num);
	}
	INC_FLOAT_STATS(time_phase1_1_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);



/*
	///////////////////////////////////
	uint64_t tt = get_sys_clock();
	uint32_t logger = GET_THD_ID % g_num_logger;
	while (true) {
		int32_t next_epoch = ATOM_FETCH_SUB(*next_log_file_epoch[logger], 1);
		if (next_epoch <= 0) 
			break;

		string path;
		if (logger == 0) 		path = "/f0/yxy/silo/";
		else if (logger == 1)	path = "/f1/yxy/silo/";
		else if (logger == 2)	path = "/f2/yxy/silo/";
		else if (logger == 3)	path = "/data/yxy/silo/";
		
		//if (logger == 3) 
		//	pthread_mutex_lock(_log_lock);

		string bench = (WORKLOAD == YCSB)? "YCSB" : "TPCC";
		path += "BD_log" + to_string(logger) + "_" + bench + ".log." + to_string(next_epoch);
		if (logger == 3)
			pthread_mutex_lock(_log_lock);
		int fd = open(path.c_str(), O_RDONLY | O_DIRECT);
		uint32_t fsize = lseek(fd, 0, SEEK_END);
		char * buffer = new char [fsize];
		lseek(fd, 0, SEEK_SET);
		uint32_t bytes = read(fd, buffer, fsize);
		assert(bytes == fsize);
		if (logger == 3) 
			pthread_mutex_unlock(_log_lock);
		
		INC_FLOAT_STATS(log_bytes, fsize);
		// Format for batch logging 
		// | checksum | size | TID | N | (table_id | primary_key | data_length | data) * N
		uint32_t offset = 0;
		while (offset < fsize) {
			// read entries from buffer
			uint32_t checksum;
			uint32_t size; 
			uint64_t tid;
			uint32_t start = offset;
			UNPACK(buffer, checksum, offset);
			UNPACK(buffer, size, offset);
			UNPACK(buffer, tid, offset);
			if (checksum != 0xdead) {
				//printf("checksum=%x, offset=%d, fsize=%d\n", checksum, offset, fsize);
				break;
			}
			
			recover_txn(buffer + offset);
			INC_INT_STATS(num_commits, 1);
			
			offset = start + size;
		}
		//printf("logger = %d. Epoch %d done\n", logger, next_epoch);
	}

	INC_FLOAT_STATS(time_phase1_1_raw, get_sys_clock() - tt);
	pthread_barrier_wait(&worker_bar);

	// each worker thread picks a log file and process it.
	//  
	//	 read from log file to buffer.
	//   process in arbitrary TID order.
	*/
}
#endif

uint32_t
txn_man::get_log_entry_size()
{
	assert(false);
	return 0;
/*
#if LOG_TYPE == LOG_DATA
	uint32_t buffsize = 0;
  	// size, txn_id and wr_cnt
	buffsize += sizeof(uint32_t) + sizeof(txn_id) + sizeof(wr_cnt);
  	// for table names
  	// TODO. right now, only store tableID. No column ID.
  	buffsize += sizeof(uint32_t) * wr_cnt;
  	// for keys
  	buffsize += sizeof(uint64_t) * wr_cnt;
  	// for data length
  	buffsize += sizeof(uint32_t) * wr_cnt; 
  	// for data
  	for (uint32_t i=0; i < wr_cnt; i++) {
		if (WORKLOAD == TPCC)
	    	buffsize += accesses[i]->orig_row->get_tuple_size();
		else 
			// TODO. For YCSB, only log 100 bytes of a tuple.  
	    	buffsize += 100; 

		//printf("tuple size=%ld\n", accesses[i]->orig_row->get_tuple_size());
	}
  	return buffsize; 
#elif LOG_TYPE == LOG_COMMAND
	// Format:
	//   size | txn_id | cmd_log_size
	return sizeof(uint32_t) + sizeof(txn_id) + get_cmd_log_size();
#else
	assert(false);
#endif
*/
}

void 
txn_man::create_log_entry()
{
	// TODO. in order to have a fair comparison with SiloR, Taurus only supports Silo at the moment 
	assert(CC_ALG == SILO);
#if LOG_TYPE == LOG_DATA
	// Format for serial logging
	// | checksum | size | N | (table_id | primary_key | data_length | data) * N
	// Format for parallel logging
	// | checksum | size | predecessor_info | N | (table_id | primary_key | data_length | data) * N
	//
	// predecessor_info has the following format
	// if TRACK_WAR_DEPENDENCY
	//   | num_raw_preds | TID * num_raw_preds | key * num_raw_preds | table * ...
	//   | num_waw_preds | TID * num_waw_preds | key * num_waw_preds | table * ...
	// else 
	//   | num_raw_preds | TID * num_raw_preds 
	//   | num_waw_preds | TID * num_waw_preds
	//
	// Format for batch logging 
	// | checksum | size | TID | N | (table_id | primary_key | data_length | data) * N
	// 
	// Assumption: every write is actually an update. 
	// predecessors store the TID of predecessor transactions. 
	uint32_t offset = 0;
	uint32_t checksum = 0xdead;
	uint32_t size = 0;
	PACK(_log_entry, checksum, offset);
	PACK(_log_entry, size, offset);
  #if LOG_ALGORITHM == LOG_PARALLEL 
	uint32_t start = offset;
    PACK(_log_entry, _num_raw_preds, offset);
	PACK_SIZE(_log_entry, _raw_preds_tid, _num_raw_preds * sizeof(uint64_t), offset);
	#if TRACK_WAR_DEPENDENCY
	PACK_SIZE(_log_entry, _raw_preds_key, _num_raw_preds * sizeof(uint64_t), offset);
	PACK_SIZE(_log_entry, _raw_preds_table, _num_raw_preds * sizeof(uint32_t), offset);
	//for (uint32_t i = 0; i < _num_raw_preds; i++)
	//	if (_raw_preds_key[i] == 1 && _raw_preds_table[i] == 0)
	//	printf("tid=%ld, key=%ld, table=%d\n", _raw_preds_tid[i], _raw_preds_key[i], _raw_preds_table[i]);

	#endif
    PACK(_log_entry, _num_waw_preds, offset);
	PACK_SIZE(_log_entry, _waw_preds_tid, _num_waw_preds * sizeof(uint64_t), offset);
	#if TRACK_WAR_DEPENDENCY
	PACK_SIZE(_log_entry, _waw_preds_key, _num_waw_preds * sizeof(uint64_t), offset);
	PACK_SIZE(_log_entry, _waw_preds_table, _num_waw_preds * sizeof(uint32_t), offset);
	#endif
	uint32_t dep_size = offset - start;	
	INC_FLOAT_STATS(log_dep_size, dep_size);
	//for (uint32_t i = 0; i < _num_waw_preds; i++)
	//	if (_waw_preds_key[i] == 1 && _waw_preds_table[i] == 0)
	//		printf("tid=%ld, key=%ld, table=%d\n", _waw_preds_tid[i], _waw_preds_key[i], _waw_preds_table[i]);
  #elif LOG_ALGORITHM == LOG_BATCH
    PACK(_log_entry, _cur_tid, offset);
  #endif

	PACK(_log_entry, wr_cnt, offset);
	for (uint32_t i = 0; i < wr_cnt; i ++) {
		row_t * orig_row = accesses[write_set[i]]->orig_row; 
		uint32_t table_id = orig_row->get_table()->get_table_id();
		uint64_t key = orig_row->get_primary_key();
		uint32_t tuple_size = orig_row->get_tuple_size();
		char * tuple_data = accesses[write_set[i]]->data;

		PACK(_log_entry, table_id, offset);
		PACK(_log_entry, key, offset);
		PACK(_log_entry, tuple_size, offset);
		PACK_SIZE(_log_entry, tuple_data, tuple_size, offset);
	}
	// TODO checksum is ignored. 
	_log_entry_size = offset;
	assert(_log_entry_size < MAX_LOG_ENTRY_SIZE);
	// update size. 
	memcpy(_log_entry + sizeof(uint32_t), &_log_entry_size, sizeof(uint32_t));
	INC_FLOAT_STATS(log_total_size, _log_entry_size);

#elif LOG_TYPE == LOG_COMMAND
	// Format for serial logging
	// 	| checksum | size | benchmark_specific_command | 
	// Format for parallel logging
	// 	| checksum | size | predecessor_info | benchmark_specific_command | 
	uint32_t offset = 0;
	uint32_t checksum = 0xdead;
	uint32_t size = 0;
	PACK(_log_entry, checksum, offset);
	PACK(_log_entry, size, offset);
  #if LOG_ALGORITHM == LOG_PARALLEL 
	uint32_t start = offset;
    PACK(_log_entry, _num_raw_preds, offset);
	PACK_SIZE(_log_entry, _raw_preds_tid, _num_raw_preds * sizeof(uint64_t), offset);
	#if TRACK_WAR_DEPENDENCY
	PACK_SIZE(_log_entry, _raw_preds_key, _num_raw_preds * sizeof(uint64_t), offset);
	PACK_SIZE(_log_entry, _raw_preds_table, _num_raw_preds * sizeof(uint32_t), offset);
	#endif
    PACK(_log_entry, _num_waw_preds, offset);
	PACK_SIZE(_log_entry, _waw_preds_tid, _num_waw_preds * sizeof(uint64_t), offset);
	#if TRACK_WAR_DEPENDENCY
	PACK_SIZE(_log_entry, _waw_preds_key, _num_waw_preds * sizeof(uint64_t), offset);
	PACK_SIZE(_log_entry, _waw_preds_table, _num_waw_preds * sizeof(uint32_t), offset);
	#endif
	uint32_t dep_size = offset - start;	
	INC_FLOAT_STATS(log_dep_size, dep_size);
  #endif
    _log_entry_size = offset;
	// internally, the following function will update _log_entry_size and _log_entry
	get_cmd_log_entry();
	
	assert(_log_entry_size < MAX_LOG_ENTRY_SIZE);
	assert(_log_entry_size > sizeof(uint32_t) * 2);
	memcpy(_log_entry + sizeof(uint32_t), &_log_entry_size, sizeof(uint32_t));
	INC_FLOAT_STATS(log_total_size, _log_entry_size);
#else
	assert(false);
#endif
}
