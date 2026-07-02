#include <rte_byteorder.h>

#include "common.h"
#include "applications/application.h"
#include "applications/synthetic/workload.h"

void synthetic_init_application(void)
{
	return;
}

bool synthetic_process_request(struct rte_mbuf *m, uint64_t rx_burst_time,
	uint32_t processed_time_us, void **app_ctx_io,
	struct packet_analysis_result *analysis_result,
	struct app_process_result *result)
{
	(void)app_ctx_io;
	if (analysis_result == NULL || result == NULL)
		return false;
	if (!synthetic_parse_request(analysis_result))
		return false;
	unsigned lcore_id = rte_lcore_id();

	if (active_worker_ctx->publish_next_overloaded_flow) {
		uint16_t src_port = analysis_result->l4_src_port;
		if (src_port != 0) {
			publish_overloaded_flow_candidate(lcore_id,
				analysis_result->src_ip, src_port, analysis_result->l4_proto);
			active_worker_ctx->publish_next_overloaded_flow = false;
		}
	}

	uint32_t total_processing_time = analysis_result->processing_time;
	if (processed_time_us > total_processing_time)
		processed_time_us = total_processing_time;

	uint32_t remaining_time = total_processing_time - processed_time_us;
	uint64_t remaining_time_ns = synthetic_scheduled_delay_ns(remaining_time);
	uint64_t elapsed_ns = 0;

	active_worker_ctx->packet_preempted = false;
	if (active_worker_ctx->lapic != NULL) {
		active_worker_ctx->lapic[APIC_LVTT / 4] =
			active_worker_ctx->lapic_lvtt_unmask_value;
		active_worker_ctx->lapic[APIC_TMICT / 4] =
			active_worker_ctx->lapic_tmict_value;
		active_worker_ctx->app_running = true;
	}
	elapsed_ns = busy_loop_ns(remaining_time_ns);
	if (active_worker_ctx->lapic != NULL && active_worker_ctx->app_running) {
		active_worker_ctx->lapic[APIC_LVTT / 4] =
			active_worker_ctx->lapic_lvtt_mask_value;
		active_worker_ctx->app_running = false;
	}

	bool packet_preempted = active_worker_ctx->packet_preempted;
	if (packet_preempted)
		active_worker_ctx->packet_preempted = false;

	uint32_t elapsed_us = (uint32_t)(elapsed_ns / 1000ULL);
	uint32_t accumulated_time = processed_time_us + elapsed_us;
	if (accumulated_time > total_processing_time)
		accumulated_time = total_processing_time;

	if (packet_preempted) {
		int requeue_rc = deferred_buffer_enqueue_packet(
			lcore_id, m, rx_burst_time, accumulated_time, NULL);
		if (requeue_rc < 0) {
			printf("Core %u: failed to requeue preempted packet (rc=%d)\n",
				lcore_id, requeue_rc);
			rte_pktmbuf_free(m);
		}
		result->completed = false;
		return true;
	}

	result->completed = true;
	return true;
}

size_t synthetic_build_response_payload(const struct response_params *rp,
	void *dst, size_t dst_cap)
{
	struct response_payload *p;

	if (rp == NULL || dst == NULL || active_worker_ctx == NULL)
		return 0;
	if (dst_cap < sizeof(struct response_payload))
		return 0;

	p = (struct response_payload *)dst;
	p->queue_id = active_worker_ctx->queue_id;
	p->core_id = (uint8_t)rte_lcore_id();
	p->processing_time = rte_cpu_to_be_16(rp->processing_time);
	p->sequence_number = rte_cpu_to_be_32(rp->sequence_number);
	p->timestamp_send = rte_cpu_to_be_64(rp->timestamp_send);
	p->rx_burst_time = rte_cpu_to_be_64(rp->rx_burst_time);
	p->analysis_start_time = rte_cpu_to_be_64(rp->analysis_start_time);
	p->analysis_end_time = rte_cpu_to_be_64(rp->analysis_end_time);
	p->processing_complete_time = rte_cpu_to_be_64(tsc_now());
	return sizeof(struct response_payload);
}
