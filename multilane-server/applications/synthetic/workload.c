#include <rte_byteorder.h>

#include "common.h"
#include "applications/synthetic/workload.h"

bool synthetic_parse_request(struct packet_analysis_result *analysis_result)
{
	const struct request_payload *req;

	if (analysis_result == NULL || analysis_result->app_payload == NULL)
		return false;
	if (analysis_result->app_payload_len < sizeof(*req))
		return false;

	req = (const struct request_payload *)analysis_result->app_payload;

	analysis_result->processing_time = rte_be_to_cpu_16(req->processing_time);
	analysis_result->sequence_number = rte_be_to_cpu_32(req->sequence_number);
	analysis_result->timestamp_send = rte_be_to_cpu_64(req->timestamp_send);
	return true;
}

uint64_t synthetic_scheduled_delay_ns(uint32_t remaining_time)
{
	if (remaining_time == MAGIC_100_NS)
		return 100;
	if (remaining_time == MAGIC_500_NS)
		return 500;
	if (remaining_time == MAGIC_1250_NS)
		return 1250;
	if (remaining_time == MAGIC_5700_NS)
		return 5700;
	return (uint64_t)remaining_time * 1000ULL;
}

/* Busy loop delay using TSC with nanosecond input.
 * Returns the actual time spent (in nanoseconds). */
uint64_t busy_loop_ns(uint64_t ns)
{
	if (ns == 0)
		return 0;

	uint64_t tsc_hz = global_tsc_hz;
	uint64_t start_tsc = rte_rdtsc();
	uint64_t target_cycles = ns * tsc_hz / 1000000000ULL;
	uint64_t elapsed_cycles = 0;

	/* Busy loop until target time is reached or preemption occurs */
	while ((elapsed_cycles = rte_rdtsc() - start_tsc) < target_cycles) {
		if (active_worker_ctx != NULL && active_worker_ctx->packet_preempted)
			break;
		__asm__ volatile("pause" ::: "memory");
	}

	uint64_t elapsed_ns = elapsed_cycles * 1000000000ULL / tsc_hz;
	if (elapsed_ns > ns)
		elapsed_ns = ns;
	return elapsed_ns;
}
