#ifndef _MULTILANE_CLIENT_COMMON_H_
#define _MULTILANE_CLIENT_COMMON_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <rte_config.h>
#include <rte_byteorder.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_cycles.h>

/*********************************
* Constants
**********************************/

#define RTE_LOGTYPE_ML RTE_LOGTYPE_USER1

#define MAX_LCORES RTE_MAX_LCORE
#define MAX_WORKER_LCORES RTE_MAX_LCORE
#define BURST_SIZE 64

// MultiLane: Change this depending on the server's CPU frequency
#define SERVER_HZ 1.9e9

/* request_payload.processing_time can only represent integer microseconds.
 * For non-integer microsecond test cases, MultiLane uses these magic values below.
 */
#define MAGIC_100_NS   555
#define MAGIC_500_NS   666
#define MAGIC_1250_NS  777
#define MAGIC_5700_NS  888

/* Batch logging to reduce per-packet I/O overhead.
 * Client buffers raw uint64_t total_duration values in memory, and periodically
 * flush them to disk in binary form. Analysis is done offline by scripts.
 */
#define BATCH_FPIRNT 1024

/*********************************
* Structs
**********************************/

/* Application workload type */
enum app_type {
	APP_TYPE_SYNTHETIC = 0,
	APP_TYPE_MEMCACHED,
	APP_TYPE_ROCKSDB
};

/* Load distribution types */
enum load_distribution {
	DIST_FIXED_1,         /* Fixed(1us) */
	DIST_FIXED_10,        /* Fixed(10us) */
	DIST_EXPONENTIAL_10,  /* Exponential(mean=10us) */
	DIST_HIGH_BIMODAL,    /* Bimodal(50:1, 50:100) */
	DIST_EXTREME_BIMODAL, /* Bimodal(99.5:0.5, 0.5:500) */
	DIST_ROCKSDB_HT,      /* Bimodal(50:1.25, 50:613) */
	DIST_ROCKSDB_LT,      /* Fixed(1.25us) */
	DIST_TPCC             /* TPC-C mix (44:5.7, 4:6, 44:20, 4:88, 4:100) */
};

/* Consistent with server */
struct request_payload {
	/* Request type reserved for application. */
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

/* Consistent with server */
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
	/* Timestamp when request processing is complete (TSC cycles). */
	rte_be64_t processing_complete_time;
};

/* TX worker context structures */
struct tx_worker_context {
	uint16_t queue_id;         /* TX queue ID bound to this worker */
	uint32_t packets_to_send;  /* Number of packets this worker should send */
	uint64_t delay_cycles;     /* Delay cycles between packets for rate limiting */
	uint32_t worker_index;     /* Index of this TX worker [0, total_workers-1] */
	uint32_t start_flow;       /* Initial flow index */
};

/* RX worker context structures */
struct rx_worker_context {
	uint16_t queue_id;         /* RX queue ID bound to this worker */
	char filename[256];        /* Output filename with unified timestamp */
	FILE *log;                 /* Opened output stream for this RX worker */
};

/*********************************
* Global variables
**********************************/

extern volatile bool global_quit_signal;
extern uint64_t global_tsc_hz;
extern uint32_t global_enabled_port_mask;
extern uint16_t global_nic_port_id;
extern char global_unified_timestamp[32];
extern struct rte_mempool *pktmbuf_pool;

/* Application arguments (parsed into globals) */
extern uint32_t global_max_pkt_count;
extern enum load_distribution global_load_dist;
extern uint32_t global_tx_lcores[MAX_WORKER_LCORES];
extern uint32_t global_rx_lcores[MAX_WORKER_LCORES];
extern uint32_t global_nb_tx_lcores;
extern uint32_t global_nb_rx_lcores;
extern uint32_t global_target_rps;
extern enum app_type global_app_type;
extern bool global_app_get_ratio_init;
extern double global_app_get_ratio;

/*********************************
* Functions
**********************************/

/* args.c */
int parse_portmask(const char *portmask);
int parse_lcore_range(const char *lcore_str, uint32_t *lcore_array, uint32_t *nb_lcores);
int parse_get_ratio(const char *s, double *ratio);
void init_default_args(void);
int parse_args(int argc, char **argv);

/* load_generator.c */
void load_generator_init(enum load_distribution dist);
uint16_t load_generator_get_processing_time(void);
const char *load_generator_get_name(void);

/* network/ */
struct rte_mbuf *network_tx_build_packet(uint16_t current_src_port, uint32_t seq_num,
	uint32_t tx_packet_index);
int network_rx_process_packet(struct rte_mbuf *m, FILE *log, uint64_t rx_burst_tsc);
void network_flush_log(FILE *log);
uint64_t tsc_now(void);

/* worker.c */
void run_tx_loop(void *arg);
void run_rx_loop(void *arg);
void worker_run(void);
void tx_worker_run(void);
void rx_worker_run(void);

#endif /* _MULTILANE_CLIENT_COMMON_H_ */
