#ifndef APPLICATIONS_SYNTHETIC_WORKLOAD_H
#define APPLICATIONS_SYNTHETIC_WORKLOAD_H

#include "common.h"

/* request_payload.processing_time can only represent integer microseconds.
 * For non-integer microsecond test cases, MultiLane uses these magic values below.
 */
#define MAGIC_100_NS   555
#define MAGIC_500_NS   666
#define MAGIC_1250_NS  777
#define MAGIC_5700_NS  888

/*
 * UDP application payload layout for the synthetic workload (APP_TYPE_SYNTHETIC).
 * Must stay bitwise-compatible with multilane-client.
 */
 struct request_payload {
	/* Request type (reserved). */
	uint8_t request_type;
	/* Reserved byte to keep payload layout aligned. */
	uint8_t padding;
	/* Requested processing time from client (1-65535 us or magic code). */
	rte_be16_t processing_time;
	/* Sequence number (reserved). */
	rte_be32_t sequence_number;
	/* Client-side send timestamp in TSC cycles. */
	rte_be64_t timestamp_send;
};

struct response_payload {
	/* RX queue ID corresponding to core_id. */
	uint8_t queue_id;
	/* Core ID that processed this packet. */
	uint8_t core_id;
	/* Processing time echoed from request payload. */
	rte_be16_t processing_time;
	/* Sequence number echoed from request payload. */
	rte_be32_t sequence_number;
	/* Client send timestamp echoed by server. */
	rte_be64_t timestamp_send;
	/* Timestamp captured right after rte_eth_rx_burst (TSC cycles). */
	rte_be64_t rx_burst_time;
	/* Timestamp when packet analysis starts (TSC cycles). */
	rte_be64_t analysis_start_time;
	/* Timestamp when packet analysis ends (TSC cycles). */
	rte_be64_t analysis_end_time;
	/* Timestamp when sending this response packet (TSC cycles). */
	rte_be64_t processing_complete_time;
};

uint64_t busy_loop_ns(uint64_t ns);
bool synthetic_parse_request(struct packet_analysis_result *analysis_result);
uint64_t synthetic_scheduled_delay_ns(uint32_t remaining_time);

#endif /* APPLICATIONS_SYNTHETIC_WORKLOAD_H */
