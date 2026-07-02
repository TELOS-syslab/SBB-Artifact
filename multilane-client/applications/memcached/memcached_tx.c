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
#include <rte_cycles.h>
#include <netinet/in.h>

#include "common.h"
#include "applications/memcached/protocol.h"
#include "network/net.h"

static double memcached_get_ratio = 0.8;

void memcached_tx_init_traffic(double get_ratio)
{
	if (get_ratio < 0.0)
		get_ratio = 0.0;
	if (get_ratio > 1.0)
		get_ratio = 1.0;
	memcached_get_ratio = get_ratio;
}

static int memcached_next_request_is_get(void)
{
	double r = (double)rte_rand() / (double)UINT64_MAX;

	return (r < memcached_get_ratio) ? 1 : 0;
}

static size_t u64_to_dec(char *dst, size_t dst_size, uint64_t value)
{
	char buf[32];
	size_t len = 0;

	do {
		buf[len++] = (char)('0' + (value % 10));
		value /= 10;
	} while (value && len < sizeof(buf));

	if (len + 1 > dst_size)
		len = dst_size - 1;

	for (size_t i = 0; i < len; i++)
		dst[i] = buf[len - 1 - i];

	dst[len] = '\0';
	return len;
}

static void format_memcached_set_value(char *dst, size_t dst_size, uint32_t sent_idx, uint64_t now_tsc)
{
	const char prefix[] = "value_";
	const char mid[] = "_";
	size_t offset = 0;

	if (dst_size == 0)
		return;

	size_t prefix_len = sizeof(prefix) - 1;
	size_t copy_len = (prefix_len < dst_size - 1) ? prefix_len : dst_size - 1;

	memcpy(dst, prefix, copy_len);
	offset += copy_len;

	if (offset < dst_size - 1)
		offset += u64_to_dec(dst + offset, dst_size - offset, sent_idx);

	if (offset < dst_size - 1) {
		size_t mid_len = sizeof(mid) - 1;
		size_t mid_copy = (mid_len < dst_size - 1 - offset) ?
			mid_len : dst_size - 1 - offset;

		memcpy(dst + offset, mid, mid_copy);
		offset += mid_copy;
	}

	if (offset < dst_size - 1)
		u64_to_dec(dst + offset, dst_size - offset, now_tsc);
	else
		dst[dst_size - 1] = '\0';
}

static struct rte_mbuf *
construct_memcached_get(uint16_t current_src_port, uint32_t key_idx, uint32_t seq_num)
{
	struct rte_mbuf *m;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct memcached_payload_header *mc_hdr;
	char *text;
	static const char base_cmd[] = "get key00\r\n";
	const int cmd_len = (int)(sizeof(base_cmd) - 1);
	uint16_t pkt_len = (uint16_t)(sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_udp_hdr) + sizeof(struct memcached_payload_header) + cmd_len);

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
	udp_hdr->dst_port = rte_cpu_to_be_16(MEMCACHED_DEFAULT_PORT);
	udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) +
		sizeof(struct memcached_payload_header) + cmd_len);
	udp_hdr->dgram_cksum = rte_cpu_to_be_16(0);

	mc_hdr = (struct memcached_payload_header *)(udp_hdr + 1);
	mc_hdr->request_id = rte_cpu_to_be_16(1);
	mc_hdr->sequence_number = rte_cpu_to_be_16(0);
	mc_hdr->total_datagrams = rte_cpu_to_be_16(1);
	mc_hdr->reserved = rte_cpu_to_be_16(0);
	mc_hdr->rx_burst_time = rte_cpu_to_be_64(0);
	mc_hdr->tx_before_time = rte_cpu_to_be_64(0);

	text = (char *)(mc_hdr + 1);
	memcpy(text, base_cmd, (size_t)cmd_len);

	key_idx %= MEMCACHED_MAX_ENTRIES;
	text[7] = (char)('0' + (key_idx / 10));
	text[8] = (char)('0' + (key_idx % 10));

	return m;
}

static struct rte_mbuf *
construct_memcached_set(uint16_t current_src_port, uint32_t key_idx, uint32_t seq_num,
	const char *value, uint32_t flags, uint32_t exptime)
{
	struct rte_mbuf *m;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct memcached_payload_header *mc_hdr;
	char *text;
	char cmd_buf[512];
	char key_buf[8] = "key00";
	int value_len = (int)strlen(value);
	int cmd_len;
	uint16_t pkt_len;

	key_idx %= MEMCACHED_MAX_ENTRIES;
	key_buf[3] = (char)('0' + (key_idx / 10));
	key_buf[4] = (char)('0' + (key_idx % 10));

	cmd_len = snprintf(cmd_buf, sizeof(cmd_buf), "set %s %u %u %d\r\n%s\r\n",
		key_buf, flags, exptime, value_len, value);
	if (cmd_len < 0 || cmd_len >= (int)sizeof(cmd_buf))
		return NULL;

	pkt_len = (uint16_t)(sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_udp_hdr) + sizeof(struct memcached_payload_header) + cmd_len);
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
	udp_hdr->dst_port = rte_cpu_to_be_16(MEMCACHED_DEFAULT_PORT);
	udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) +
		sizeof(struct memcached_payload_header) + cmd_len);
	udp_hdr->dgram_cksum = rte_cpu_to_be_16(0);

	mc_hdr = (struct memcached_payload_header *)(udp_hdr + 1);
	mc_hdr->request_id = rte_cpu_to_be_16(1);
	mc_hdr->sequence_number = rte_cpu_to_be_16(0);
	mc_hdr->total_datagrams = rte_cpu_to_be_16(1);
	mc_hdr->reserved = rte_cpu_to_be_16(0);
	mc_hdr->rx_burst_time = rte_cpu_to_be_64(0);
	mc_hdr->tx_before_time = rte_cpu_to_be_64(0);

	text = (char *)(mc_hdr + 1);
	memcpy(text, cmd_buf, (size_t)cmd_len);

	return m;
}

struct rte_mbuf *
memcached_tx_build_packet(uint16_t current_src_port, uint32_t seq_num, uint32_t tx_packet_index)
{
	uint32_t key_idx = (uint32_t)rte_rand() % MEMCACHED_MAX_ENTRIES;
	uint64_t now_tsc = rte_get_tsc_cycles();

	if (memcached_next_request_is_get())
		return construct_memcached_get(current_src_port, key_idx, seq_num);
	else {
		char value_buf[128];

		format_memcached_set_value(value_buf, sizeof(value_buf), tx_packet_index, now_tsc);
		return construct_memcached_set(current_src_port, key_idx, seq_num, value_buf, 0, 60);
	}
}
