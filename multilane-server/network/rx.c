#include <stdio.h>
#include <string.h>

#include "common.h"
#include "network/net_stack.h"

bool analyze_packet_content(struct rte_mbuf *m, struct packet_analysis_result *result)
{
	enum net_parse_status status;
	struct net_packet_view view;
	struct packet_dissection dissect;

	memset(result, 0, sizeof(*result));
	result->analysis_start_time = tsc_now();

	if (m == NULL) {
		printf("Error: NULL mbuf received\n");
		return false;
	}

	net_packet_view_init(&view, m);
	if (view.data == NULL || view.len == 0)
		return false;

	net_dissect_reset(&dissect, &view);

	status = net_parse_layer2(&dissect);
	if (status != NET_PARSE_OK) {
		printf("Error: Failed to parse layer 2: %s\n", net_parse_status_str(status));
		return false;
	}

	status = net_parse_layer3(&dissect);
	if (status != NET_PARSE_OK) {
		printf("Error: Failed to parse layer 3: %s\n", net_parse_status_str(status));
		return false;
	}

	status = net_parse_layer4(&dissect);
	if (status != NET_PARSE_OK) {
		printf("Error: Failed to parse layer 4: %s\n", net_parse_status_str(status));
		return false;
	}

	result->l4_proto = dissect.l4_proto;
	result->eth_hdr = dissect.eth;
	result->ipv4_hdr = dissect.ipv4;
	result->udp_hdr = dissect.udp;
	result->tcp_hdr = dissect.tcp;
	if (dissect.ipv4 != NULL) {
		result->src_ip = rte_be_to_cpu_32(dissect.ipv4->src_addr);
		result->dst_ip = rte_be_to_cpu_32(dissect.ipv4->dst_addr);
	}
	if (dissect.udp != NULL) {
		result->l4_src_port = rte_be_to_cpu_16(dissect.udp->src_port);
		result->l4_dst_port = rte_be_to_cpu_16(dissect.udp->dst_port);
	} else if (dissect.tcp != NULL) {
		result->l4_src_port = rte_be_to_cpu_16(dissect.tcp->src_port);
		result->l4_dst_port = rte_be_to_cpu_16(dissect.tcp->dst_port);
	}

	if (dissect.payload_len > 0) {
		result->app_payload = dissect.view.data + dissect.payload_offset;
		result->app_payload_len = dissect.payload_len;
	}

	result->analysis_end_time = tsc_now();
	return true;
}
