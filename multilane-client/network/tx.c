#include <stdint.h>

#include <rte_mbuf.h>
#include <rte_ether.h>

#include "applications/application.h"
#include "network/net.h"

// MultiLane: change the MAC and IP addresses as needed.
uint8_t ml_client_src_mac[RTE_ETHER_ADDR_LEN] = {0x6C, 0xFE, 0x54, 0x41, 0x1E, 0x10};
uint8_t ml_client_dst_mac[RTE_ETHER_ADDR_LEN] = {0x6C, 0xFE, 0x54, 0x41, 0x1D, 0x40};

uint32_t ml_client_src_ip_h = 1U | (0U << 8) | (18U << 16) | (198U << 24);
uint32_t ml_client_dst_ip_h = 2U | (0U << 8) | (18U << 16) | (198U << 24);

uint64_t tsc_now(void)
{
	unsigned int lo, hi;

	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

struct rte_mbuf *
network_tx_build_packet(uint16_t current_src_port, uint32_t seq_num, uint32_t tx_packet_index)
{
	return application_tx_build_packet(current_src_port, seq_num, tx_packet_index);
}
