#include "net_stack.h"

static enum net_parse_status parse_ipv4(struct packet_dissection *dissect)
{
	const struct net_packet_view *view = &dissect->view;
	size_t remaining = view->len - dissect->l3_offset;

	if (remaining < sizeof(struct rte_ipv4_hdr))
		return NET_PARSE_TRUNCATED;

	struct rte_ipv4_hdr *ipv4 = (struct rte_ipv4_hdr *)(view->data + dissect->l3_offset);
	uint8_t version = ipv4->version_ihl >> 4;
	uint8_t ihl = (ipv4->version_ihl & 0x0F) * 4;
	if (version != 4 || ihl < sizeof(struct rte_ipv4_hdr))
		return NET_PARSE_MALFORMED;
	if (remaining < ihl)
		return NET_PARSE_TRUNCATED;

	uint16_t total_length = rte_be_to_cpu_16(ipv4->total_length);
	if (total_length < ihl)
		return NET_PARSE_MALFORMED;

	uint16_t max_len = (uint16_t)(view->len - dissect->l3_offset);
	if (total_length > max_len)
		total_length = max_len;

	dissect->ipv4 = ipv4;
	dissect->l3_len = total_length;
	dissect->l4_offset = dissect->l3_offset + ihl;
	dissect->l4_proto = ipv4->next_proto_id;

	uint16_t frag_field = rte_be_to_cpu_16(ipv4->fragment_offset);
	dissect->ipv4_frag_offset = (uint16_t)(frag_field & RTE_IPV4_HDR_OFFSET_MASK);
	dissect->ipv4_fragmented = (frag_field & (RTE_IPV4_HDR_OFFSET_MASK | RTE_IPV4_HDR_MF_FLAG)) != 0;

	return NET_PARSE_OK;
}

static enum net_parse_status parse_arp(struct packet_dissection *dissect)
{
	const struct net_packet_view *view = &dissect->view;
	size_t remaining = view->len - dissect->l3_offset;

	if (remaining < sizeof(struct rte_arp_hdr))
		return NET_PARSE_TRUNCATED;

	dissect->arp = (struct rte_arp_hdr *)(view->data + dissect->l3_offset);
	dissect->l3_len = sizeof(struct rte_arp_hdr);
	dissect->l4_offset = dissect->l3_offset + dissect->l3_len;
	dissect->l4_proto = 0;

	return NET_PARSE_OK;
}

enum net_parse_status net_parse_layer3(struct packet_dissection *dissect)
{
	const struct net_packet_view *view = &dissect->view;
	if (dissect->l3_offset >= view->len)
		return NET_PARSE_TRUNCATED;

	switch (dissect->ether_type) {
	case RTE_ETHER_TYPE_ARP:
		return parse_arp(dissect);
	case RTE_ETHER_TYPE_IPV4:
		return parse_ipv4(dissect);
	default:
		return NET_PARSE_UNSUPPORTED;
	}
}
