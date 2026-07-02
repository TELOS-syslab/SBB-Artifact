#ifndef MULTILANE_CLIENT_APPLICATIONS_APPLICATION_H
#define MULTILANE_CLIENT_APPLICATIONS_APPLICATION_H

#include <stdio.h>
#include <stdint.h>

struct rte_mbuf;

void application_parse_init(void);

struct rte_mbuf *application_tx_build_packet(uint16_t current_src_port, uint32_t seq_num,
	uint32_t tx_packet_index);
int application_rx_process_packet(struct rte_mbuf *m, FILE *log, uint64_t rx_burst_tsc);

struct rte_mbuf *synthetic_tx_build_packet(uint16_t current_src_port, uint32_t seq_num);
int synthetic_rx_process(struct rte_mbuf *m, FILE *log, uint64_t rx_burst_tsc);

void memcached_tx_init_traffic(double get_ratio);
struct rte_mbuf *memcached_tx_build_packet(uint16_t current_src_port, uint32_t seq_num,
	uint32_t tx_packet_index);
int memcached_rx_process(struct rte_mbuf *m, FILE *log, uint64_t rx_burst_tsc);

void rocksdb_tx_init_traffic(double get_ratio);
struct rte_mbuf *rocksdb_tx_build_packet(uint16_t current_src_port, uint32_t seq_num,
	uint32_t tx_packet_index);
int rocksdb_rx_process(struct rte_mbuf *m, FILE *log, uint64_t rx_burst_tsc);

#endif
