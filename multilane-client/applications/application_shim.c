#include "common.h"
#include "applications/application.h"

void application_parse_init(void)
{
	if (global_app_type == APP_TYPE_MEMCACHED) {
		double r = global_app_get_ratio_init ? global_app_get_ratio : 0.8;

		memcached_tx_init_traffic(r);
	} else if (global_app_type == APP_TYPE_ROCKSDB) {
		double r = global_app_get_ratio_init ? global_app_get_ratio : 0.8;

		rocksdb_tx_init_traffic(r);
	}
}

struct rte_mbuf *
application_tx_build_packet(uint16_t current_src_port, uint32_t seq_num, uint32_t tx_packet_index)
{
	switch (global_app_type) {
	case APP_TYPE_MEMCACHED:
		return memcached_tx_build_packet(current_src_port, seq_num, tx_packet_index);
	case APP_TYPE_ROCKSDB:
		return rocksdb_tx_build_packet(current_src_port, seq_num, tx_packet_index);
	case APP_TYPE_SYNTHETIC:
	default:
		return synthetic_tx_build_packet(current_src_port, seq_num);
	}
}

int application_rx_process_packet(struct rte_mbuf *m, FILE *log, uint64_t rx_burst_tsc)
{
	switch (global_app_type) {
	case APP_TYPE_MEMCACHED:
		return memcached_rx_process(m, log, rx_burst_tsc);
	case APP_TYPE_ROCKSDB:
		return rocksdb_rx_process(m, log, rx_burst_tsc);
	case APP_TYPE_SYNTHETIC:
	default:
		return synthetic_rx_process(m, log, rx_burst_tsc);
	}
}
