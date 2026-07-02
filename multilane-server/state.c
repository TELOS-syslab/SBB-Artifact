#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "common.h"

struct worker_shared_state worker_shared_states[RTE_MAX_LCORE];
uint32_t worker_rx_burst_histogram[RTE_MAX_LCORE][RX_BURST_HISTOGRAM_SIZE];

static const char *worker_load_state_name(uint8_t state)
{
	switch (state) {
	case WORKER_STATE_LIGHT:
		return "LIGHT";
	case WORKER_STATE_BUSY:
		return "BUSY";
	case WORKER_STATE_OVERLOADED:
		return "OVERLOADED";
	default:
		return "UNKNOWN";
	}
}

void print_worker_rx_burst_histograms(void)
{
	unsigned int lcore_id;
	printf("\nWorker RX burst histograms before exit:\n");

	RTE_LCORE_FOREACH(lcore_id) {
		uint64_t total_packets = 0;

		for (uint32_t i = 0; i < RX_BURST_HISTOGRAM_SIZE; i++)
			total_packets += (uint64_t)i *
				worker_rx_burst_histogram[lcore_id][i];

		printf("  Worker %u rx_burst histogram:", lcore_id);
		for (uint32_t i = 0; i < RX_BURST_HISTOGRAM_SIZE; i++)
			printf(" %u", worker_rx_burst_histogram[lcore_id][i]);
		printf(" total_packets=%" PRIu64 "\n", total_packets);
	}
}

void print_worker_shared_states(void)
{
	unsigned int lcore_id;

	printf("\nWorker shared states before exit:\n");
    
	RTE_LCORE_FOREACH(lcore_id) {
		uint8_t state = __atomic_load_n(
			&worker_shared_states[lcore_id].load_state,
			__ATOMIC_RELAXED);
		printf("  lcore %u: worker_shared_states.load_state=%u (%s)\n",
			lcore_id, state, worker_load_state_name(state));
	}
}

void print_port_stats_summary(void) {
	struct rte_eth_stats hw_stats;
	if (rte_eth_stats_get(global_nic_port_id, &hw_stats) == 0) {
		printf("\nPort stats before exit:\n");
		printf("  HW ipackets: %" PRIu64 "\n", hw_stats.ipackets);
		printf("  HW ibytes: %" PRIu64 "\n", hw_stats.ibytes);
		printf("  HW imissed: %" PRIu64 "\n", hw_stats.imissed);
		printf("  HW ierrors: %" PRIu64 "\n", hw_stats.ierrors);
		printf("  HW rx_nombuf: %" PRIu64 "\n", hw_stats.rx_nombuf);
		printf("  HW opackets: %" PRIu64 "\n", hw_stats.opackets);
		printf("  HW obytes: %" PRIu64 "\n", hw_stats.obytes);
		printf("  HW oerrors: %" PRIu64 "\n", hw_stats.oerrors);
	}
}

void update_worker_load_state(unsigned lcore_id, uint16_t nb_rx)
{
	struct worker_context *ctx = active_worker_ctx;
	uint8_t state;

	if (ctx == NULL)
		return;

	state = __atomic_load_n(&worker_shared_states[lcore_id].load_state,
		__ATOMIC_RELAXED);

	switch (state) {
	case WORKER_STATE_LIGHT:
		if (nb_rx > LIGHT_TO_BUSY_THRESHOLD) {
			ctx->busy_streak++;
			if (ctx->busy_streak >= 3) {
				__atomic_store_n(&worker_shared_states[lcore_id].load_state,
					WORKER_STATE_BUSY, __ATOMIC_RELAXED);
				ctx->busy_streak = 0;
			}
		} else {
			ctx->busy_streak = 0;
		}
		ctx->light_streak = 0;
		ctx->overloaded_streak = 0;
		return;
	case WORKER_STATE_BUSY:
		if (nb_rx > BUSY_TO_OVERLOADED_THRESHOLD) {
			ctx->overloaded_streak++;
			ctx->light_streak = 0;
			if (ctx->overloaded_streak >= 3) {
				__atomic_store_n(&worker_shared_states[lcore_id].load_state,
					WORKER_STATE_OVERLOADED, __ATOMIC_RELAXED);
				ctx->publish_next_overloaded_flow = true;
				ctx->overloaded_streak = 0;
				rte_spinlock_lock(&worker_shared_states[lcore_id].published_lock);
				worker_shared_states[lcore_id].published_valid = 0;
				worker_shared_states[lcore_id].published_l4_proto = 0;
				worker_shared_states[lcore_id].published_l4_src_port = 0;
				worker_shared_states[lcore_id].published_src_ip_host = 0;
				rte_spinlock_unlock(&worker_shared_states[lcore_id].published_lock);
			}
		} else if (nb_rx < BUSY_TO_LIGHT_THRESHOLD) {
			ctx->light_streak++;
			ctx->overloaded_streak = 0;
			if (ctx->light_streak >= 3) {
				__atomic_store_n(&worker_shared_states[lcore_id].load_state,
					WORKER_STATE_LIGHT, __ATOMIC_RELAXED);
				ctx->light_streak = 0;
			}
		} else {
			ctx->light_streak = 0;
			ctx->overloaded_streak = 0;
		}
		ctx->busy_streak = 0;
		return;
	case WORKER_STATE_OVERLOADED:
		if (nb_rx < OVERLOADED_TO_BUSY_THRESHOLD) {
			ctx->light_streak++;
			if (ctx->light_streak >= 3) {
				__atomic_store_n(&worker_shared_states[lcore_id].load_state,
					WORKER_STATE_BUSY, __ATOMIC_RELAXED);
				ctx->light_streak = 0;
			}
		} else {
			ctx->light_streak = 0;
		}
		ctx->busy_streak = 0;
		ctx->overloaded_streak = 0;
		return;
	default:
		__atomic_store_n(&worker_shared_states[lcore_id].load_state,
			WORKER_STATE_LIGHT, __ATOMIC_RELAXED);
		ctx->busy_streak = 0;
		ctx->light_streak = 0;
		ctx->overloaded_streak = 0;
		return;
	}
}
