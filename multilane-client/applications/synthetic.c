#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>
#include <netinet/in.h>

#include "common.h"
#include "network/net.h"

#define ML_SYNTHETIC_UDP_DST_PORT 9

struct rte_mbuf *synthetic_tx_build_packet(uint16_t current_src_port, uint32_t seq_num)
{
	struct rte_mbuf *m;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	uint8_t *payload;
	uint16_t pkt_len = 14 + 20 + 8 + sizeof(struct request_payload);

	m = rte_pktmbuf_alloc(pktmbuf_pool);
	if (m == NULL)
		return NULL;

	m->data_len = pkt_len;
	m->pkt_len = pkt_len;
	m->ol_flags = 0;

	uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);

	/* Construct Ethernet header */
	eth_hdr = (struct rte_ether_hdr *)data;
	rte_memcpy(&eth_hdr->dst_addr, ml_client_dst_mac, RTE_ETHER_ADDR_LEN);
	rte_memcpy(&eth_hdr->src_addr, ml_client_src_mac, RTE_ETHER_ADDR_LEN);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	/* Construct IPv4 header */
	ipv4_hdr = (struct rte_ipv4_hdr *)(data + sizeof(struct rte_ether_hdr));
	// Version 4, IHL 5
	ipv4_hdr->version_ihl = (4 << 4) | 5;
	ipv4_hdr->type_of_service = 0;
	ipv4_hdr->total_length = rte_cpu_to_be_16(20 + 8 + sizeof(struct request_payload));
	ipv4_hdr->packet_id = rte_cpu_to_be_16(0);
	ipv4_hdr->fragment_offset = rte_cpu_to_be_16(0);
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->hdr_checksum = rte_cpu_to_be_16(0);
	ipv4_hdr->src_addr = rte_cpu_to_be_32(ml_client_src_ip_h);
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(ml_client_dst_ip_h);

	/* Construct UDP header */
	udp_hdr = (struct rte_udp_hdr *)((uint8_t *)ipv4_hdr + ((ipv4_hdr->version_ihl & 0xF) * 4));
	udp_hdr->src_port = rte_cpu_to_be_16(current_src_port);
	udp_hdr->dst_port = rte_cpu_to_be_16(ML_SYNTHETIC_UDP_DST_PORT);
	udp_hdr->dgram_len = rte_cpu_to_be_16(8 + sizeof(struct request_payload));
	udp_hdr->dgram_cksum = rte_cpu_to_be_16(0);

	/* Fill payload */
	payload = (uint8_t *)(udp_hdr + 1);
	struct request_payload *req = (struct request_payload *)payload;
	req->request_type = 0;
	req->padding = 0;
	req->processing_time = rte_cpu_to_be_16(load_generator_get_processing_time());
	req->sequence_number = rte_cpu_to_be_32(seq_num);

	uint16_t udp_payload_len = rte_be_to_cpu_16(udp_hdr->dgram_len) - sizeof(struct rte_udp_hdr);
	if (udp_payload_len > sizeof(struct request_payload)) {
		memset(payload + sizeof(struct request_payload), 0,
		       udp_payload_len - sizeof(struct request_payload));
	}

	req->timestamp_send = rte_cpu_to_be_64(tsc_now());
	return m;
}

int synthetic_rx_process(struct rte_mbuf *m, FILE *log, __rte_unused uint64_t rx_burst_tsc)
{
	uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
	uint64_t rx_burst_time;
	uint64_t processing_complete_time;
	uint64_t total_duration;
	double total_ns;
	uint16_t processing_time_raw;
	double processing_time_ns;
	double slowdown;

	/* Parse packet headers */
	struct rte_ipv4_hdr *ipv4_hdr =
		(struct rte_ipv4_hdr *)(data + sizeof(struct rte_ether_hdr));
	struct rte_udp_hdr *udp_hdr =
		(struct rte_udp_hdr *)((uint8_t *)ipv4_hdr + ((ipv4_hdr->version_ihl & 0xF) * 4));
	const struct response_payload *resp = (const struct response_payload *)(udp_hdr + 1);

	rx_burst_time = rte_be_to_cpu_64(resp->rx_burst_time);
	processing_complete_time = rte_be_to_cpu_64(resp->processing_complete_time);
	total_duration = processing_complete_time - rx_burst_time;
	total_ns = (double)total_duration * 1e9 / SERVER_HZ;

	processing_time_raw = rte_be_to_cpu_16(resp->processing_time);

	/* Calculate processing time in nanoseconds.
	 * Magic numbers:
	 * MAGIC_100_NS   555
	 * MAGIC_500_NS   666
	 * MAGIC_1250_NS  777
	 * MAGIC_5700_NS  888
	 */
	if (processing_time_raw == MAGIC_100_NS) {
		processing_time_ns = 100.0;     /* 0.1us = 100ns */
	} else if (processing_time_raw == MAGIC_500_NS) {
		processing_time_ns = 500.0;     /* 0.5us = 500ns */
	} else if (processing_time_raw == MAGIC_1250_NS) {
		processing_time_ns = 1250.0;    /* 1.25us = 1250ns */
	} else if (processing_time_raw == MAGIC_5700_NS) {
		processing_time_ns = 5700.0;    /* 5.7us = 5700ns */
	} else {
		processing_time_ns = (double)processing_time_raw * 1e3;
	}
	slowdown = (processing_time_ns > 0.0) ? (total_ns / processing_time_ns) : 0.0;

	network_rx_log_slowdown(log, slowdown);
	return 0;
}
