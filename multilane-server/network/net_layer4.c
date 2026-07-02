#include <netinet/in.h>

#include "net_stack.h"

static uint16_t available_l4_bytes(const struct packet_dissection *dissect)
{
	if (dissect->l3_len == 0 || dissect->l4_offset < dissect->l3_offset)
		return 0;

	uint16_t header_delta = (uint16_t)(dissect->l4_offset - dissect->l3_offset);
	if (dissect->l3_len <= header_delta)
		return 0;

	return (uint16_t)(dissect->l3_len - header_delta);
}

static enum net_parse_status parse_udp(struct packet_dissection *dissect)
{
	uint16_t available = available_l4_bytes(dissect);
	if (available < sizeof(struct rte_udp_hdr))
		return NET_PARSE_TRUNCATED;

	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(dissect->view.data + dissect->l4_offset);
	uint16_t dgram_len = rte_be_to_cpu_16(udp->dgram_len);
	if (dgram_len < sizeof(struct rte_udp_hdr))
		return NET_PARSE_MALFORMED;
	if (dgram_len > available)
		dgram_len = available;

	dissect->udp = udp;
	dissect->l4_len = dgram_len;
	dissect->payload_offset = dissect->l4_offset + sizeof(struct rte_udp_hdr);
	dissect->payload_len = dgram_len - sizeof(struct rte_udp_hdr);

	return NET_PARSE_OK;
}

static enum net_parse_status parse_tcp(struct packet_dissection *dissect)
{
	uint16_t available = available_l4_bytes(dissect);
	if (available < sizeof(struct rte_tcp_hdr))
		return NET_PARSE_TRUNCATED;

	struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(dissect->view.data + dissect->l4_offset);
	uint8_t data_offset = (tcp->data_off >> 4) * 4;
	if (data_offset < sizeof(struct rte_tcp_hdr))
		return NET_PARSE_MALFORMED;
	if (available < data_offset)
		return NET_PARSE_TRUNCATED;

	dissect->tcp = tcp;
	dissect->l4_len = available;
	dissect->payload_offset = dissect->l4_offset + data_offset;
	dissect->payload_len = available - data_offset;

	return NET_PARSE_OK;
}

enum net_parse_status net_parse_layer4(struct packet_dissection *dissect)
{
	if (dissect->l4_offset >= dissect->view.len)
		return NET_PARSE_TRUNCATED;

	if (dissect->ipv4_fragmented && dissect->ipv4_frag_offset != 0)
		return NET_PARSE_UNSUPPORTED;

	switch (dissect->l4_proto) {
	case IPPROTO_UDP:
		return parse_udp(dissect);
	case IPPROTO_TCP:
		return parse_tcp(dissect);
	default:
		return NET_PARSE_UNSUPPORTED;
	}
}
