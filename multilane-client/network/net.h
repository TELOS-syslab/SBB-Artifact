#ifndef MULTILANE_CLIENT_NETWORK_NET_H
#define MULTILANE_CLIENT_NETWORK_NET_H

#include <stdio.h>
#include <stdint.h>
#include <rte_ether.h>

extern uint8_t ml_client_src_mac[RTE_ETHER_ADDR_LEN];
extern uint8_t ml_client_dst_mac[RTE_ETHER_ADDR_LEN];
extern uint32_t ml_client_src_ip_h;
extern uint32_t ml_client_dst_ip_h;

void network_rx_log_slowdown(FILE *log, double slowdown);

#endif
