#ifndef MULTILANE_CLIENT_APPLICATIONS_ROCKSDB_PROTOCOL_H
#define MULTILANE_CLIENT_APPLICATIONS_ROCKSDB_PROTOCOL_H

#include <rte_byteorder.h>

#define ROCKSDB_APP_DEFAULT_PORT 12321
#define ROCKSDB_MAX_ENTRIES      2000

#define ROCKSDB_REQ_ID_GET  1
#define ROCKSDB_REQ_ID_SCAN 2

/* Default SCAN limit */
#define ROCKSDB_CLIENT_SCAN_LIMIT 40

struct rocksdb_payload_header {
	rte_be16_t request_id;
	rte_be16_t sequence_number;
	rte_be16_t total_datagrams;
	rte_be16_t reserved;
	rte_be64_t rx_burst_time;
	rte_be64_t tx_before_time;
} __attribute__((__packed__));

#endif
