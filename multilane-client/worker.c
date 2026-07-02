#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#include "common.h"

#define FLOW_NUM 10000
#define TX_BURST_BATCH 4

static uint16_t find_queue_index(const uint32_t *lcores, uint32_t nb_lcores, uint32_t lcore_id)
{
	for (uint32_t i = 0; i < nb_lcores; i++) {
		if (lcores[i] == lcore_id)
			return (uint16_t)i;
	}
	return UINT16_MAX;
}

static void init_tx_worker(struct tx_worker_context *ctx, uint16_t queue_id)
{
	uint32_t packets_per_worker = global_max_pkt_count / global_nb_tx_lcores;
	uint32_t remainder = global_max_pkt_count % global_nb_tx_lcores;
	uint64_t per_worker_rps = global_target_rps / global_nb_tx_lcores;
	uint64_t delay_cycles = (per_worker_rps > 0) ? (global_tsc_hz / per_worker_rps) : 0;

	ctx->queue_id = queue_id;
	ctx->packets_to_send = packets_per_worker + (queue_id < remainder ? 1U : 0U);
	ctx->delay_cycles = delay_cycles;
	ctx->worker_index = queue_id;
	ctx->start_flow = 0;
	if (global_nb_tx_lcores > 0) {
		ctx->start_flow = (uint32_t)((uint64_t)ctx->worker_index * FLOW_NUM / global_nb_tx_lcores);
		if (ctx->start_flow >= FLOW_NUM)
			ctx->start_flow = FLOW_NUM - 1;
	}
}

static void init_rx_worker(struct rx_worker_context *ctx, uint16_t queue_id, uint32_t lcore_id)
{
	ctx->queue_id = queue_id;
	snprintf(ctx->filename, sizeof(ctx->filename),
		"worker%u_queue%u_%s.ml", lcore_id, queue_id, global_unified_timestamp);
	ctx->log = fopen(ctx->filename, "wb");
	if (ctx->log == NULL) {
		/* Fallback to /dev/null to avoid spamming stdout */
		ctx->log = fopen("/dev/null", "wb");
	}
}

void run_tx_loop(void *arg)
{
	struct tx_worker_context *ctx = (struct tx_worker_context *)arg;
	unsigned lcore_id = rte_lcore_id();
	// Rotate source UDP port from 1 to FLOW_NUM
	uint16_t current_src_port = (uint16_t)(ctx->start_flow + 1);  
	uint32_t local_sent = 0;
	uint32_t local_built = 0;
	struct rte_mbuf *tx_batch[TX_BURST_BATCH];
	uint16_t tx_batch_count = 0;
	uint64_t next_send_tsc = rte_get_tsc_cycles();
	uint32_t sequence_number = 1;

	while (!global_quit_signal && local_built < ctx->packets_to_send) {
		uint64_t now_tsc = rte_get_tsc_cycles();
		if (now_tsc < next_send_tsc) {
			while (rte_get_tsc_cycles() < next_send_tsc) {
				rte_pause();
			}
		}

		struct rte_mbuf *m = network_tx_build_packet(current_src_port, sequence_number,
			local_built);

		if (m == NULL) {
			printf("Core %u: failed to allocate packet buffer\n", lcore_id);
			break;
		}

		tx_batch[tx_batch_count++] = m;
		local_built++;

		current_src_port++;
		if (current_src_port > FLOW_NUM) {
			current_src_port = 1;
		}

		if (tx_batch_count == TX_BURST_BATCH || local_built == ctx->packets_to_send) {
			uint16_t sent = rte_eth_tx_burst(global_nic_port_id, ctx->queue_id,
							 tx_batch, tx_batch_count);
			local_sent += sent;
			for (uint16_t i = sent; i < tx_batch_count; i++)
				rte_pktmbuf_free(tx_batch[i]);
			tx_batch_count = 0;
		}

		next_send_tsc += ctx->delay_cycles;
	}

	// Flush remaining packets in the last partial batch
	if (tx_batch_count > 0) {
		uint16_t sent = rte_eth_tx_burst(global_nic_port_id, ctx->queue_id,
						 tx_batch, tx_batch_count);
		local_sent += sent;
		for (uint16_t i = sent; i < tx_batch_count; i++)
			rte_pktmbuf_free(tx_batch[i]);
	}

	RTE_LOG(INFO, ML, "Core %u: TX worker (queue %u) sent %u packets\n", lcore_id, ctx->queue_id, local_sent);
	return;
}

void run_rx_loop(void *arg)
{
	struct rx_worker_context *ctx = (struct rx_worker_context *)arg;
	struct rte_mbuf *bufs[BURST_SIZE];
	uint16_t nb_rx;
	uint32_t rx_count = 0;
	uint64_t rx_burst_tsc = 0;
	unsigned lcore_id = rte_lcore_id();
	FILE *log = ctx->log;

	while (!global_quit_signal) {
		nb_rx = rte_eth_rx_burst(global_nic_port_id, ctx->queue_id, bufs, BURST_SIZE);
		
		if (nb_rx > 0) {
			rx_burst_tsc = tsc_now();

			for (uint16_t i = 0; i < nb_rx; i++) {
				struct rte_mbuf *m = bufs[i];
				rx_count++;
				
				network_rx_process_packet(m, log, rx_burst_tsc);
			}
			
			for (uint16_t i = 0; i < nb_rx; i++) {
				rte_pktmbuf_free(bufs[i]);
			}
		}
	}

	RTE_LOG(INFO, ML, "Core %u: RX worker (queue %u) received %u packets\n", lcore_id, ctx->queue_id, rx_count);
	
	if (log) {
		network_flush_log(log);
		fclose(log);
		ctx->log = NULL;
	}
	return;
}

void tx_worker_run(void)
{
	uint32_t lcore_id = rte_lcore_id();
	uint16_t tx_queue_id = find_queue_index(global_tx_lcores, global_nb_tx_lcores, lcore_id);
	struct tx_worker_context tx_ctx;

	if (tx_queue_id == UINT16_MAX)
		return;

	init_tx_worker(&tx_ctx, tx_queue_id);
	RTE_LOG(INFO, ML, "Core %u: launching TX worker (queue %u), will send %u packets\n",
		lcore_id, tx_ctx.queue_id, tx_ctx.packets_to_send);
	
	run_tx_loop(&tx_ctx);
	return;
}

void rx_worker_run(void)
{
	uint32_t lcore_id = rte_lcore_id();
	uint16_t rx_queue_id = find_queue_index(global_rx_lcores, global_nb_rx_lcores, lcore_id);
	struct rx_worker_context rx_ctx;

	if (rx_queue_id == UINT16_MAX)
		return;

	init_rx_worker(&rx_ctx, rx_queue_id, lcore_id);
	RTE_LOG(INFO, ML, "Core %u: launching RX worker (queue %u)\n", lcore_id, rx_ctx.queue_id);

	run_rx_loop(&rx_ctx);
	return;
}

void worker_run(void)
{
	uint32_t lcore_id = rte_lcore_id();
	uint16_t tx_queue_id = find_queue_index(global_tx_lcores, global_nb_tx_lcores, lcore_id);
	uint16_t rx_queue_id = find_queue_index(global_rx_lcores, global_nb_rx_lcores, lcore_id);

	if (tx_queue_id != UINT16_MAX && rx_queue_id != UINT16_MAX)
		rte_exit(EXIT_FAILURE, "lcore %u is configured as both TX and RX\n", lcore_id);
	else if (tx_queue_id != UINT16_MAX)
		tx_worker_run();
	else if (rx_queue_id != UINT16_MAX)
		rx_worker_run();
	else
		// Not selected by -t/-r; keep this worker lcore idle
		return;
}