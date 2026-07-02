#ifndef MULTILANE_CLIENT_APPLICATIONS_MEMCACHED_PROTOCOL_H
#define MULTILANE_CLIENT_APPLICATIONS_MEMCACHED_PROTOCOL_H

#include <rte_byteorder.h>

#define MEMCACHED_DEFAULT_PORT 11211
#define MEMCACHED_MAX_ENTRIES 100

struct memcached_payload_header {
	rte_be16_t request_id;
	rte_be16_t sequence_number;
	rte_be16_t total_datagrams;
	rte_be16_t reserved;
	rte_be64_t rx_burst_time;
	rte_be64_t tx_before_time;
} __attribute__((__packed__));

#endif
