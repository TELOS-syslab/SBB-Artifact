#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>
#include <rte_random.h>
#include <netinet/in.h>

#include "common.h"
#include "applications/rocksdb/protocol.h"
#include "network/net.h"

static double rocksdb_get_ratio = 0.8;

void rocksdb_tx_init_traffic(double get_ratio)
{
	if (get_ratio < 0.0)
		get_ratio = 0.0;
	if (get_ratio > 1.0)
		get_ratio = 1.0;
	rocksdb_get_ratio = get_ratio;
}

static int rocksdb_next_request_is_get(void)
{
	double r = (double)rte_rand() / (double)UINT64_MAX;

	return (r < rocksdb_get_ratio) ? 1 : 0;
}

static struct rte_mbuf *
alloc_rocksdb_packet(uint16_t current_src_port, const char *cmd_buf, int cmd_len, uint16_t request_id)
{
	struct rte_mbuf *m;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct rocksdb_payload_header *rdb_hdr;
	char *text;
	uint16_t pkt_len = (uint16_t)(sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_udp_hdr) + sizeof(struct rocksdb_payload_header) + (uint16_t)cmd_len);

	if (pkt_len < RTE_ETHER_MIN_LEN)
		pkt_len = RTE_ETHER_MIN_LEN;

	m = rte_pktmbuf_alloc(pktmbuf_pool);
	if (m == NULL)
		return NULL;

	m->data_len = pkt_len;
	m->pkt_len = pkt_len;
	m->ol_flags = 0;

	uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
	memset(data, 0, pkt_len);

	eth_hdr = (struct rte_ether_hdr *)data;
	rte_memcpy(&eth_hdr->dst_addr, ml_client_dst_mac, RTE_ETHER_ADDR_LEN);
	rte_memcpy(&eth_hdr->src_addr, ml_client_src_mac, RTE_ETHER_ADDR_LEN);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	ipv4_hdr = (struct rte_ipv4_hdr *)(data + sizeof(struct rte_ether_hdr));
	ipv4_hdr->version_ihl = (4 << 4) | 5;
	ipv4_hdr->type_of_service = 0;
	ipv4_hdr->total_length = rte_cpu_to_be_16(pkt_len - sizeof(struct rte_ether_hdr));
	ipv4_hdr->packet_id = rte_cpu_to_be_16(0);
	ipv4_hdr->fragment_offset = rte_cpu_to_be_16(0);
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->hdr_checksum = rte_cpu_to_be_16(0);
	ipv4_hdr->src_addr = rte_cpu_to_be_32(ml_client_src_ip_h);
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(ml_client_dst_ip_h);

	udp_hdr = (struct rte_udp_hdr *)((uint8_t *)ipv4_hdr + 20);
	udp_hdr->src_port = rte_cpu_to_be_16(current_src_port);
	udp_hdr->dst_port = rte_cpu_to_be_16(ROCKSDB_APP_DEFAULT_PORT);
	udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) +
		sizeof(struct rocksdb_payload_header) + (uint16_t)cmd_len);
	udp_hdr->dgram_cksum = rte_cpu_to_be_16(0);

	rdb_hdr = (struct rocksdb_payload_header *)(udp_hdr + 1);
	rdb_hdr->request_id = rte_cpu_to_be_16(request_id);
	rdb_hdr->sequence_number = rte_cpu_to_be_16(0);
	rdb_hdr->total_datagrams = rte_cpu_to_be_16(1);
	rdb_hdr->reserved = rte_cpu_to_be_16(0);
	rdb_hdr->rx_burst_time = rte_cpu_to_be_64(0);
	rdb_hdr->tx_before_time = rte_cpu_to_be_64(0);

	text = (char *)(rdb_hdr + 1);
	memcpy(text, cmd_buf, (size_t)cmd_len);
	return m;
}

static struct rte_mbuf *construct_rocksdb_get(uint16_t current_src_port, uint32_t key_idx)
{
	char cmd_buf[48];
	int cmd_len = snprintf(cmd_buf, sizeof(cmd_buf), "GET user%06u\r\n",
		key_idx % ROCKSDB_MAX_ENTRIES);

	if (cmd_len < 0 || cmd_len >= (int)sizeof(cmd_buf))
		return NULL;

	return alloc_rocksdb_packet(current_src_port, cmd_buf, cmd_len, ROCKSDB_REQ_ID_GET);
}

static struct rte_mbuf *construct_rocksdb_scan(uint16_t current_src_port, uint32_t key_idx)
{
	char cmd_buf[128];
	uint32_t limit = ROCKSDB_CLIENT_SCAN_LIMIT;
	uint32_t max_start = (ROCKSDB_MAX_ENTRIES > limit) ?
		(ROCKSDB_MAX_ENTRIES - limit) : 0;
	uint32_t start_key = (max_start > 0) ? (key_idx % (max_start + 1)) : 0;
	uint32_t end_key = start_key + limit - 1;
	int cmd_len = snprintf(cmd_buf, sizeof(cmd_buf), "SCAN user%06u user%06u %u\r\n",
		start_key, end_key, limit);

	if (cmd_len < 0 || cmd_len >= (int)sizeof(cmd_buf))
		return NULL;

	return alloc_rocksdb_packet(current_src_port, cmd_buf, cmd_len, ROCKSDB_REQ_ID_SCAN);
}

struct rte_mbuf *rocksdb_tx_build_packet(uint16_t current_src_port, uint32_t seq_num,
	uint32_t tx_packet_index)
{
	uint32_t key_idx = (uint32_t)rte_rand() % ROCKSDB_MAX_ENTRIES;

	(void)seq_num;
	(void)tx_packet_index;

	if (rocksdb_next_request_is_get())
		return construct_rocksdb_get(current_src_port, key_idx);
	else
		return construct_rocksdb_scan(current_src_port, key_idx);
}
