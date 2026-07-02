#include <stdio.h>
#include <string.h>

#include "common.h"
#include "applications/application.h"
#include "applications/memcached/protocol.h"
#include "applications/rocksdb/protocol.h"

static void tx_send_response(struct rte_mbuf *m, uint16_t pkt_len)
{
	uint16_t tx_queue_id = lcore_queue_map[rte_lcore_id()];

	m->data_len = pkt_len;
	m->pkt_len = pkt_len;
	if (rte_eth_tx_burst(global_nic_port_id, tx_queue_id, &m, 1) != 1) {
		printf("Core %u: failed to send response packet\n", rte_lcore_id());
		rte_pktmbuf_free(m);
	}
}

static struct rte_mbuf *tx_build_response(const struct response_params *rp,
	uint16_t *pkt_len_out)
{
	struct rte_mbuf *m;
	uint16_t l4_hdr_len;
	uint16_t ip_len;
	uint16_t pkt_len;
	uint16_t payload_len;
	size_t app_payload_len;
	size_t payload_cap;
	size_t header_len;

	if (rp->eth_hdr == NULL || rp->ipv4_hdr == NULL)
		return NULL;

	if (rp->l4_proto == IPPROTO_UDP) {
		if (rp->udp_hdr == NULL)
			return NULL;
		l4_hdr_len = sizeof(struct rte_udp_hdr);
	} else if (rp->l4_proto == IPPROTO_TCP) {
		printf("Warning: TCP response sending is not supported\n");
		return NULL;
	} else
		return NULL;

	m = rte_pktmbuf_alloc(pktmbuf_pool);
	if (m == NULL) {
		printf("Core %u: failed to allocate response packet buffer\n", rte_lcore_id());
		return NULL;
	}

	m->ol_flags = 0;

	uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
	struct rte_ether_hdr *resp_eth_hdr = (struct rte_ether_hdr *)data;
	struct rte_ipv4_hdr *resp_ipv4_hdr = (struct rte_ipv4_hdr *)(data + sizeof(struct rte_ether_hdr));
	uint8_t *l4_dest = (uint8_t *)resp_ipv4_hdr + sizeof(struct rte_ipv4_hdr);
	uint8_t *pay_dest = l4_dest + l4_hdr_len;
	header_len = (size_t)(pay_dest - data);
	if (rte_pktmbuf_tailroom(m) <= header_len) {
		rte_pktmbuf_free(m);
		return NULL;
	}
	payload_cap = rte_pktmbuf_tailroom(m) - header_len;

	app_payload_len = app_build_response_payload(rp, pay_dest, payload_cap);
	if (app_payload_len == 0 || app_payload_len > UINT16_MAX) {
		rte_pktmbuf_free(m);
		return NULL;
	}
	payload_len = (uint16_t)app_payload_len;

	ip_len = (uint16_t)(sizeof(struct rte_ipv4_hdr) + l4_hdr_len + payload_len);
	pkt_len = (uint16_t)(sizeof(struct rte_ether_hdr) + ip_len);

	rte_memcpy(&resp_eth_hdr->dst_addr, &rp->eth_hdr->src_addr, RTE_ETHER_ADDR_LEN);
	rte_memcpy(&resp_eth_hdr->src_addr, &rp->eth_hdr->dst_addr, RTE_ETHER_ADDR_LEN);
	resp_eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	resp_ipv4_hdr->version_ihl = (4 << 4) | 5;
	resp_ipv4_hdr->type_of_service = 0;
	resp_ipv4_hdr->total_length = rte_cpu_to_be_16(ip_len);
	resp_ipv4_hdr->packet_id = 0;
	resp_ipv4_hdr->fragment_offset = 0;
	resp_ipv4_hdr->time_to_live = 64;
	resp_ipv4_hdr->next_proto_id = rp->l4_proto;
	resp_ipv4_hdr->hdr_checksum = 0;
	resp_ipv4_hdr->src_addr = rp->ipv4_hdr->dst_addr;
	resp_ipv4_hdr->dst_addr = rp->ipv4_hdr->src_addr;

	struct rte_udp_hdr *resp_udp = (struct rte_udp_hdr *)l4_dest;
	resp_udp->src_port = rp->udp_hdr->dst_port;
	resp_udp->dst_port = rp->udp_hdr->src_port;
	resp_udp->dgram_len = rte_cpu_to_be_16((uint16_t)(l4_hdr_len + payload_len));
	resp_udp->dgram_cksum = 0;

	*pkt_len_out = pkt_len;
	return m;
}

void send_response_packet(const struct response_params *rp)
{
	struct rte_mbuf *m;
	uint16_t pkt_len;

	m = tx_build_response(rp, &pkt_len);
	if (m == NULL)
		return;

	tx_send_response(m, pkt_len);
}
