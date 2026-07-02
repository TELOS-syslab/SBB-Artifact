#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rocksdb/c.h>
#include <rte_lcore.h>

#include "common.h"
#include "applications/rocksdb/store.h"

static rocksdb_t *db;
static rocksdb_options_t *db_options;
static rocksdb_readoptions_t *read_options;
static rocksdb_writeoptions_t *write_options;
static const char db_path[] = "/tmp/rocksdb_dpdk_server";

static struct scan_entry scan_results[ROCKSDB_APP_MAX_SCAN_ENTRIES];

struct scan_task_state {
	struct rte_mbuf *pkt;
	char start_key[ROCKSDB_APP_MAX_KEY_LEN];
	uint16_t start_key_len;
	bool has_start_key;
	char end_key[ROCKSDB_APP_MAX_KEY_LEN];
	uint16_t end_key_len;
	bool has_end_key;
	uint32_t limit;
	rocksdb_iterator_t *iter;
	uint32_t logical_matches;
	uint32_t materialized;
	bool reached_end_bound;
	bool has_more;
	bool done;
	bool failed;
	uint32_t preempt_count;
	struct scan_entry results[ROCKSDB_APP_MAX_SCAN_ENTRIES];
};

static inline bool key_exceeds_end_bound(const struct scan_task_state *state,
	const char *key, size_t key_len)
{
	if (!state->has_end_key)
		return false;

	size_t cmp_len = (key_len < state->end_key_len) ? key_len : state->end_key_len;
	int cmp = memcmp(key, state->end_key, cmp_len);
	return (cmp > 0 || (cmp == 0 && key_len > state->end_key_len));
}

static inline void scan_preempt_window_enter(void)
{
	if (active_worker_ctx == NULL)
		return;

	active_worker_ctx->packet_preempted = false;

	if (active_worker_ctx->timer_registered && active_worker_ctx->lapic != NULL) {
		active_worker_ctx->lapic[APIC_LVTT / 4] =
			active_worker_ctx->lapic_lvtt_unmask_value;
		active_worker_ctx->lapic[APIC_TMICT / 4] =
			active_worker_ctx->lapic_tmict_value;
		active_worker_ctx->app_running = true;
	}
}

static inline void scan_preempt_window_exit(void)
{
	if (active_worker_ctx == NULL)
		return;

	if (active_worker_ctx->timer_registered &&
	    active_worker_ctx->lapic != NULL &&
	    active_worker_ctx->app_running) {
		active_worker_ctx->lapic[APIC_LVTT / 4] =
			active_worker_ctx->lapic_lvtt_mask_value;
		active_worker_ctx->app_running = false;
	}
}

void rocksdb_store_init(void)
{
	char *err = NULL;

	if (db != NULL)
		return;

	db_options = rocksdb_options_create();
	rocksdb_options_set_create_if_missing(db_options, 1);
	rocksdb_options_set_compression(db_options, rocksdb_no_compression);
	rocksdb_options_set_max_background_jobs(db_options, 0);
	rocksdb_options_set_disable_auto_compactions(db_options, 1);

	db = rocksdb_open(db_options, db_path, &err);
	if (err != NULL) {
		fprintf(stderr, "Error opening RocksDB: %s\n", err);
		free(err);
		rocksdb_options_destroy(db_options);
		db_options = NULL;
		rte_exit(EXIT_FAILURE, "Cannot initialize RocksDB\n");
	}

	read_options = rocksdb_readoptions_create();
	write_options = rocksdb_writeoptions_create();
	printf("RocksDB: initialized successfully at %s\n", db_path);
}

void rocksdb_preload(uint32_t count)
{
	rocksdb_writebatch_t *batch;
	char *err = NULL;
	const char charset[] =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	const size_t charset_size = sizeof(charset) - 1;

	if (db == NULL) {
		fprintf(stderr, "RocksDB: error: not initialized\n");
		return;
	}
	if (count == 0)
		count = 1000;

	printf("RocksDB: preloading %u entries...\n", count);
	batch = rocksdb_writebatch_create();
	for (uint32_t i = 0; i < count; i++) {
		char key[32];
		char value[128];
		int key_len = snprintf(key, sizeof(key), "user%06u", i);
		int value_len = 20 + (rte_rand() % 80);

		for (int j = 0; j < value_len; j++)
			value[j] = charset[rte_rand() % charset_size];

		rocksdb_writebatch_put(batch, key, (size_t)key_len, value, (size_t)value_len);
		if ((i + 1) % 100 == 0) {
			rocksdb_write(db, write_options, batch, &err);
			if (err != NULL) {
				fprintf(stderr, "RocksDB: write error: %s\n", err);
				free(err);
				err = NULL;
			}
			rocksdb_writebatch_clear(batch);
		}
	}

	if (rocksdb_writebatch_count(batch) > 0) {
		rocksdb_write(db, write_options, batch, &err);
		if (err != NULL) {
			fprintf(stderr, "RocksDB: write error: %s\n", err);
			free(err);
		}
	}

	rocksdb_writebatch_destroy(batch);
	printf("RocksDB: preloaded %u entries successfully\n", count);
}

static struct scan_task_state *
scan_task_create(const struct rocksdb_request *request, struct rte_mbuf *pkt)
{
	struct scan_task_state *state;

	if (db == NULL || request == NULL || request->cmd != ROCKSDB_CMD_SCAN)
		return NULL;

	state = calloc(1, sizeof(*state));
	if (state == NULL)
		return NULL;

	state->pkt = pkt;
	state->limit = request->limit;
	if (state->limit == 0 || state->limit > ROCKSDB_APP_MAX_SCAN_LIMIT)
		state->limit = ROCKSDB_APP_MAX_SCAN_LIMIT;

	if (request->start_key_len > 0 &&
	    request->start_key_len <= ROCKSDB_APP_MAX_KEY_LEN) {
		memcpy(state->start_key, request->start_key, request->start_key_len);
		state->start_key_len = request->start_key_len;
		state->has_start_key = true;
	}

	if (request->end_key_len > 0 &&
	    request->end_key_len <= ROCKSDB_APP_MAX_KEY_LEN) {
		memcpy(state->end_key, request->end_key, request->end_key_len);
		state->end_key_len = request->end_key_len;
		state->has_end_key = true;
	}

	state->iter = rocksdb_create_iterator(db, read_options);
	if (state->iter == NULL) {
		free(state);
		return NULL;
	}

	if (state->has_start_key)
		rocksdb_iter_seek(state->iter, state->start_key, (size_t)state->start_key_len);
	else
		rocksdb_iter_seek_to_first(state->iter);

	return state;
}

static void scan_task_destroy(struct scan_task_state *state)
{
	if (state == NULL)
		return;

	if (state->iter != NULL) {
		rocksdb_iter_destroy(state->iter);
		state->iter = NULL;
	}
	free(state);
}

static bool scan_task_run_slice(struct scan_task_state *state, bool *done)
{
	bool preempted = false;

	if (done != NULL)
		*done = false;
	if (state == NULL || state->iter == NULL)
		return false;
	if (state->done) {
		if (done != NULL)
			*done = true;
		return !state->failed;
	}

	scan_preempt_window_enter();

	while (rocksdb_iter_valid(state->iter) && state->logical_matches < state->limit) {
		if (active_worker_ctx != NULL && active_worker_ctx->packet_preempted) {
			preempted = true;
			state->preempt_count++;
			break;
		}

		size_t key_len_read = 0;
		size_t value_len_read = 0;
		const char *key = rocksdb_iter_key(state->iter, &key_len_read);
		const char *value = rocksdb_iter_value(state->iter, &value_len_read);

		if (key_exceeds_end_bound(state, key, key_len_read)) {
			state->reached_end_bound = true;
			break;
		}

		if (key_len_read <= ROCKSDB_APP_MAX_KEY_LEN &&
		    value_len_read <= ROCKSDB_APP_MAX_VALUE_LEN) {
			if (state->materialized < ROCKSDB_APP_MAX_SCAN_ENTRIES) {
				struct scan_entry *entry = &state->results[state->materialized];
				entry->key_len = (uint16_t)key_len_read;
				memcpy(entry->key, key, key_len_read);
				entry->value_len = (uint32_t)value_len_read;
				memcpy(entry->value, value, value_len_read);
				state->materialized++;
			}
			state->logical_matches++;
		}

		rocksdb_iter_next(state->iter);
	}

	scan_preempt_window_exit();

	{
		char *err = NULL;
		rocksdb_iter_get_error(state->iter, &err);
		if (err != NULL) {
			fprintf(stderr, "RocksDB: iterator error: %s\n", err);
			free(err);
			state->failed = true;
			state->done = true;
			if (done != NULL)
				*done = true;
			return false;
		}
	}

	if (preempted && rocksdb_iter_valid(state->iter) &&
	    state->logical_matches < state->limit && !state->reached_end_bound) {
		if (done != NULL)
			*done = false;
		return true;
	}

	{
		bool iter_more = rocksdb_iter_valid(state->iter) && !state->reached_end_bound;
		if (iter_more && state->has_end_key) {
			size_t key_len_read = 0;
			const char *key = rocksdb_iter_key(state->iter, &key_len_read);
			if (key_exceeds_end_bound(state, key, key_len_read))
				iter_more = false;
		}
		state->has_more = iter_more || (state->logical_matches > state->materialized);
	}

	state->done = true;
	if (done != NULL)
		*done = true;
	return true;
}

static bool scan_task_fill_response(const struct scan_task_state *state,
	struct rocksdb_response *resp)
{
	if (state == NULL || resp == NULL)
		return false;

	memset(resp, 0, sizeof(*resp));
	if (state->failed) {
		resp->type = ROCKSDB_RESP_ERROR;
		return true;
	}

	if (state->materialized > 0) {
		memcpy(scan_results, state->results,
			state->materialized * sizeof(struct scan_entry));
		resp->type = ROCKSDB_RESP_SCAN_RESULT;
		resp->entries = scan_results;
		resp->entry_count = state->materialized;
		resp->has_more = state->has_more;
	} else {
		resp->type = ROCKSDB_RESP_END;
	}

	return true;
}

bool rocksdb_store_get(const struct rocksdb_request *req, struct rocksdb_response *resp)
{
	char *err = NULL;
	size_t read_len = 0;
	char *rocksdb_value;

	if (req == NULL || resp == NULL)
		return false;
	if (req->key_len == 0 || req->key_len > ROCKSDB_APP_MAX_KEY_LEN)
		return false;
	if (db == NULL)
		return false;

	rocksdb_value = rocksdb_get(db, read_options, req->key, (size_t)req->key_len,
		&read_len, &err);
	if (err != NULL) {
		fprintf(stderr, "RocksDB: Get error: %s\n", err);
		free(err);
		resp->type = ROCKSDB_RESP_END;
		return true;
	}
	if (rocksdb_value == NULL) {
		resp->type = ROCKSDB_RESP_END;
		return true;
	}

	resp->type = ROCKSDB_RESP_VALUE;
	resp->key = req->key;
	resp->key_len = req->key_len;
	resp->value = rocksdb_value;
	resp->value_len = (uint32_t)read_len;
	return true;
}

bool rocksdb_store_scan(struct rte_mbuf *m, uint64_t rx_burst_time,
	uint32_t processed_time_us, void **scan_ctx_io,
	const struct rocksdb_request *req, struct rocksdb_response *resp,
	bool *completed)
{
	struct scan_task_state *scan_state = NULL;
	bool scan_done = false;
	bool success;

	if (req == NULL || resp == NULL || completed == NULL)
		return false;

	if (scan_ctx_io != NULL)
		scan_state = (struct scan_task_state *)(*scan_ctx_io);

	if (scan_state == NULL) {
		scan_state = scan_task_create(req, m);
		if (scan_ctx_io != NULL)
			*scan_ctx_io = scan_state;
		if (scan_state == NULL) {
			resp->type = ROCKSDB_RESP_ERROR;
			*completed = true;
			return true;
		}
	}

	success = scan_task_run_slice(scan_state, &scan_done);
	if (!success) {
		resp->type = ROCKSDB_RESP_ERROR;
		*completed = true;
		scan_task_destroy(scan_state);
		if (scan_ctx_io != NULL)
			*scan_ctx_io = NULL;
		return true;
	}

	if (!scan_done) {
		unsigned int lcore_id = rte_lcore_id();
		int requeue_rc = deferred_buffer_enqueue_packet(lcore_id, m, rx_burst_time,
			processed_time_us, scan_state);

		if (requeue_rc < 0) {
			RTE_LOG(WARNING, ML,
				"Core %u failed to requeue preempted SCAN packet (rc=%d)\n",
				lcore_id, requeue_rc);
			scan_task_destroy(scan_state);
			rte_pktmbuf_free(m);
		}
		if (scan_ctx_io != NULL)
			*scan_ctx_io = NULL;
		*completed = false;
		return true;
	}

	if (!scan_task_fill_response(scan_state, resp))
		resp->type = ROCKSDB_RESP_ERROR;

	scan_task_destroy(scan_state);
	if (scan_ctx_io != NULL)
		*scan_ctx_io = NULL;
	return false;
}
