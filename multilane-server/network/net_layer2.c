#include <string.h>

#include "net_stack.h"

void net_packet_view_init(struct net_packet_view *view, struct rte_mbuf *m)
{
	view->mbuf = m;
	view->data = rte_pktmbuf_mtod(m, uint8_t *);
	view->len = rte_pktmbuf_pkt_len(m);
}

void net_dissect_reset(struct packet_dissection *dissect, const struct net_packet_view *view)
{
	memset(dissect, 0, sizeof(*dissect));
	dissect->view = *view;
}

const char *net_parse_status_str(enum net_parse_status status)
{
	switch (status) {
	case NET_PARSE_OK:
		return "ok";
	case NET_PARSE_TRUNCATED:
		return "truncated";
	case NET_PARSE_UNSUPPORTED:
		return "unsupported";
	case NET_PARSE_MALFORMED:
		return "malformed";
	default:
		return "unknown";
	}
}

enum net_parse_status net_parse_layer2(struct packet_dissection *dissect)
{
	const struct net_packet_view *view = &dissect->view;

	if (view->len < sizeof(struct rte_ether_hdr))
		return NET_PARSE_TRUNCATED;

	dissect->eth = (struct rte_ether_hdr *)view->data;
	dissect->l2_len = sizeof(struct rte_ether_hdr);
	dissect->l3_offset = dissect->l2_len;
	dissect->ether_type = rte_be_to_cpu_16(dissect->eth->ether_type);

	return NET_PARSE_OK;
}
