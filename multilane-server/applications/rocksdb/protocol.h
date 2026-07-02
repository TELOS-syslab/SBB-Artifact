#ifndef APPLICATIONS_ROCKSDB_PROTOCOL_H
#define APPLICATIONS_ROCKSDB_PROTOCOL_H

/*
 * RocksDB library usage:
 *
 * The server side (multilane-server/applications/rocksdb/store.c) calls the RocksDB C API from <rocksdb/c.h>:
 *
 *   rocksdb_options_create, rocksdb_options_set_create_if_missing,
 *   rocksdb_options_set_compression, rocksdb_options_set_max_background_jobs,
 *   rocksdb_options_set_disable_auto_compactions, rocksdb_open,
 *   rocksdb_readoptions_create, rocksdb_writeoptions_create,
 *   rocksdb_writebatch_create, rocksdb_writebatch_put, rocksdb_write,
 *   rocksdb_writebatch_clear, rocksdb_writebatch_count,
 *   rocksdb_writebatch_destroy, rocksdb_get, rocksdb_create_iterator,
 *   rocksdb_iter_seek, rocksdb_iter_seek_to_first, rocksdb_iter_valid,
 *   rocksdb_iter_key, rocksdb_iter_value, rocksdb_iter_next,
 *   rocksdb_iter_get_error
 */

#include "common.h"

#define ROCKSDB_APP_DEFAULT_PORT        12321
#define ROCKSDB_APP_PRELOAD_ENTRIES     2000
#define ROCKSDB_APP_MAX_KEY_LEN         250
#define ROCKSDB_APP_MAX_VALUE_LEN       2048
#define ROCKSDB_APP_MAX_TEXT_LINE       (ROCKSDB_APP_MAX_KEY_LEN + ROCKSDB_APP_MAX_VALUE_LEN + 128)
#define ROCKSDB_APP_SCAN_RESP_TEXT_CAP  1000
#define ROCKSDB_APP_MAX_SCAN_ENTRIES    500
#define ROCKSDB_APP_MAX_SCAN_LIMIT      1024

/*
 * UDP application payload prefix for the RocksDB workload (APP_TYPE_ROCKSDB).
 * Must stay bitwise-compatible with multilane-client.
 */
struct rocksdb_payload_header {
	rte_be16_t request_id;
	rte_be16_t sequence_number;
	rte_be16_t total_datagrams;
	rte_be16_t reserved;
	rte_be64_t rx_burst_time;
	rte_be64_t tx_before_time;
} __attribute__((__packed__));

enum rocksdb_command {
	ROCKSDB_CMD_INVALID = 0,
	ROCKSDB_CMD_GET,
	ROCKSDB_CMD_SCAN,
};

enum rocksdb_response_type {
	ROCKSDB_RESP_END = 0,        /* GET miss or SCAN empty; reply is END\r\n */
	ROCKSDB_RESP_VALUE,          /* GET hit; VALUE line + payload + \r\n END\r\n */
	ROCKSDB_RESP_SCAN_RESULT,    /* SCAN hit; SCAN_ENTRY lines + END\r\n / HAS_MORE\r\n */
	ROCKSDB_RESP_ERROR,          /* Reply is ERROR\r\n */
};

struct scan_entry {
	char key[ROCKSDB_APP_MAX_KEY_LEN];
	uint16_t key_len;
	char value[ROCKSDB_APP_MAX_VALUE_LEN];
	uint32_t value_len;
};

struct rocksdb_request {
	bool valid;
	enum rocksdb_command cmd;
	char key[ROCKSDB_APP_MAX_KEY_LEN + 1];
	uint16_t key_len;
	char start_key[ROCKSDB_APP_MAX_KEY_LEN + 1];
	uint16_t start_key_len;
	char end_key[ROCKSDB_APP_MAX_KEY_LEN + 1];
	uint16_t end_key_len;
	uint32_t limit;
};

struct rocksdb_response {
	enum rocksdb_response_type type;
	struct rocksdb_payload_header header;
	const char *key;
	uint16_t key_len;
	const char *value;
	uint32_t value_len;
	struct scan_entry *entries;
	uint32_t entry_count;
	bool has_more;
};

bool rocksdb_parse_request(const struct packet_analysis_result *analysis_result,
	struct rocksdb_payload_header *payload_hdr, struct rocksdb_request *req);

#endif /* APPLICATIONS_ROCKSDB_PROTOCOL_H */
