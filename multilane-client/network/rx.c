#include <stdio.h>

#include <rte_mbuf.h>

#include "applications/application.h"
#include "common.h"
#include "network/net.h"

static __thread uint32_t tls_batch_cnt;
static __thread double tls_batch_results[BATCH_FPIRNT];

static void flush_batch_log(FILE *log)
{
	if (log == NULL)
		return;
	if (tls_batch_cnt == 0)
		return;

	/* Write buffered slowdown values as raw double. */
	fwrite(tls_batch_results, sizeof(double), tls_batch_cnt, log);

	fflush(log);
	tls_batch_cnt = 0;
}

void network_rx_log_slowdown(FILE *log, double slowdown)
{
	if (log == NULL)
		return;

	// MultiLane: the content of the log
	if (tls_batch_cnt < BATCH_FPIRNT)
		tls_batch_results[tls_batch_cnt++] = slowdown;

	if (tls_batch_cnt >= BATCH_FPIRNT)
		flush_batch_log(log);
}

int network_rx_process_packet(struct rte_mbuf *m, FILE *log, uint64_t rx_burst_tsc)
{
	return application_rx_process_packet(m, log, rx_burst_tsc);
}

void network_flush_log(FILE *log)
{
	flush_batch_log(log);
}
