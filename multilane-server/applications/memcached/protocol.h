#ifndef APPLICATIONS_MEMCACHED_PROTOCOL_H
#define APPLICATIONS_MEMCACHED_PROTOCOL_H

#include "common.h"

#define MEMCACHED_DEFAULT_PORT 11211
#define MEMCACHED_MAX_KEY_LEN 250
#define MEMCACHED_MAX_VALUE_LEN 2048
#define MEMCACHED_MAX_TEXT_LINE (MEMCACHED_MAX_KEY_LEN + MEMCACHED_MAX_VALUE_LEN + 128)

/*
 * UDP application payload layout for the memcached workload (APP_TYPE_MEMCACHED).
 * Must stay bitwise-compatible with multilane-client.
 */
struct memcached_payload_header {
	rte_be16_t request_id;
	rte_be16_t sequence_number;
	rte_be16_t total_datagrams;
	rte_be16_t reserved;
	rte_be64_t rx_burst_time;
	rte_be64_t tx_before_time;
} __attribute__((__packed__));

enum memcached_command {
	MEMCACHED_CMD_INVALID = 0,
	MEMCACHED_CMD_GET,
	MEMCACHED_CMD_SET
};

enum memcached_response_type {
	MEMCACHED_RESP_END = 0,   /* GET miss: no matching key; reply is END\r\n */
	MEMCACHED_RESP_VALUE,     /* GET hit: VALUE line + data + trailing END\r\n */
	MEMCACHED_RESP_STORED,    /* SET persisted successfully; reply is STORED\r\n */
	MEMCACHED_RESP_ERROR,     /* SET rejected or store failure; reply is ERROR\r\n */
};

struct memcached_request {
	bool valid;
	enum memcached_command cmd;
	char key[MEMCACHED_MAX_KEY_LEN + 1];
	uint16_t key_len;
	char value[MEMCACHED_MAX_VALUE_LEN];
	uint32_t value_len;
	uint32_t flags;
	uint32_t exptime;
};

struct memcached_response {
	enum memcached_response_type type;
	struct memcached_payload_header header;
	const char *key;
	uint16_t key_len;
	const char *value;
	uint32_t value_len;
	uint32_t flags;
};

bool memcached_parse_request(const struct packet_analysis_result *analysis_result,
	struct memcached_payload_header *payload_hdr, struct memcached_request *req);

#endif /* APPLICATIONS_MEMCACHED_PROTOCOL_H */
