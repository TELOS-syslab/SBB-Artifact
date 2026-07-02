#include <stdio.h>
#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>
#include <netinet/in.h>

#include "common.h"
#include "applications/memcached/protocol.h"
#include "network/net.h"

/* Reference processing time for memcached RX slowdown normalization (1 µs). */
#define STANDARD_PROCESSING_TIME 1000

int memcached_rx_process(struct rte_mbuf *m, FILE *log, __rte_unused uint64_t rx_burst_tsc)
{
	uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	uint16_t ip_hdr_len;
	uint16_t udp_payload_len;
	const struct memcached_payload_header *mc_hdr;

	if (m->pkt_len < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr))
		return 0;

	ipv4_hdr = (struct rte_ipv4_hdr *)(data + sizeof(struct rte_ether_hdr));
	if (ipv4_hdr->next_proto_id != IPPROTO_UDP)
		return 0;

	ip_hdr_len = (uint16_t)((ipv4_hdr->version_ihl & 0xF) * 4);
	udp_hdr = (struct rte_udp_hdr *)((uint8_t *)ipv4_hdr + ip_hdr_len);

	if (rte_be_to_cpu_16(udp_hdr->src_port) != MEMCACHED_DEFAULT_PORT)
		return 0;

	udp_payload_len = rte_be_to_cpu_16(udp_hdr->dgram_len);
	if (udp_payload_len < sizeof(struct rte_udp_hdr) + sizeof(struct memcached_payload_header))
		return 0;
	udp_payload_len = (uint16_t)(udp_payload_len - sizeof(struct rte_udp_hdr));

	mc_hdr = (const struct memcached_payload_header *)(udp_hdr + 1);
	uint64_t rx_burst_time = rte_be_to_cpu_64(mc_hdr->rx_burst_time);
	uint64_t tx_before_time = rte_be_to_cpu_64(mc_hdr->tx_before_time);
	uint64_t total_duration_cycles = tx_before_time - rx_burst_time;
	double total_ns = (double)total_duration_cycles * 1e9 / SERVER_HZ;
	double slowdown = total_ns / (double)STANDARD_PROCESSING_TIME;

	network_rx_log_slowdown(log, slowdown);
	return 0;
}
