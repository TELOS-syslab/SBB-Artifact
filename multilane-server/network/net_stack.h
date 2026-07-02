#ifndef NET_STACK_H
#define NET_STACK_H

#include "common.h"

enum net_parse_status {
	NET_PARSE_OK = 0,
	NET_PARSE_TRUNCATED,
	NET_PARSE_UNSUPPORTED,
	NET_PARSE_MALFORMED
};

struct net_packet_view {
	struct rte_mbuf *mbuf;
	uint8_t *data;
	uint16_t len;
};

struct packet_dissection {
	struct net_packet_view view;
	uint16_t l2_len;
	uint16_t l3_len;
	uint16_t l4_len;
	uint16_t payload_len;
	uint16_t l3_offset;
	uint16_t l4_offset;
	uint16_t payload_offset;

	struct rte_ether_hdr *eth;
	uint16_t ether_type;

	struct rte_arp_hdr *arp;
	struct rte_ipv4_hdr *ipv4;
	bool ipv4_fragmented;
	uint16_t ipv4_frag_offset;

	uint8_t l4_proto;
	struct rte_udp_hdr *udp;
	struct rte_tcp_hdr *tcp;
};

void net_packet_view_init(struct net_packet_view *view, struct rte_mbuf *m);
void net_dissect_reset(struct packet_dissection *dissect, const struct net_packet_view *view);
const char *net_parse_status_str(enum net_parse_status status);

enum net_parse_status net_parse_layer2(struct packet_dissection *dissect);
enum net_parse_status net_parse_layer3(struct packet_dissection *dissect);
enum net_parse_status net_parse_layer4(struct packet_dissection *dissect);

#endif /* NET_STACK_H */
