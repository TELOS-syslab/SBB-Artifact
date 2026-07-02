#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <rte_byteorder.h>

#include "applications/application.h"
#include "applications/memcached/protocol.h"
#include "applications/memcached/store.h"

void memcached_init_application(void)
{
	memcached_store_init();
	memcached_preload(MEMCACHED_MAX_ENTRIES);
}

bool memcached_process_request(struct rte_mbuf *m, uint64_t rx_burst_time,
	uint32_t processed_time_us, void **app_ctx_io,
	struct packet_analysis_result *analysis_result,
	struct app_process_result *result)
{
	(void)m;
	(void)rx_burst_time;
	(void)processed_time_us;
	(void)app_ctx_io;
	struct memcached_payload_header payload_hdr;
	struct memcached_request req;

	if (result == NULL)
		return false;
	if (!memcached_parse_request(analysis_result, &payload_hdr, &req))
		return false;

	memset(&result->memcached_response, 0, sizeof(result->memcached_response));
	result->uses_memcached_response = true;
	result->memcached_response.header = payload_hdr;
	result->memcached_request = req;

	switch (req.cmd) {
	case MEMCACHED_CMD_GET:
		if (!memcached_store_get(&result->memcached_request, &result->memcached_response))
			return false;
		break;
	case MEMCACHED_CMD_SET:
		memcached_store_set(&result->memcached_request, &result->memcached_response);
		break;
	default:
		return false;
	}

	result->completed = true;
	return true;
}

size_t memcached_build_response_payload(const struct response_params *rp,
	void *dst, size_t dst_cap)
{
	char text_buf[MEMCACHED_MAX_TEXT_LINE];
	size_t text_len = 0;
	struct memcached_payload_header *hdr;
	char *text_part;
	size_t need;

	if (rp->memcached_response == NULL)
		return 0;

	switch (rp->memcached_response->type) {
	case MEMCACHED_RESP_VALUE: {
		int n;

		if (rp->memcached_response->key == NULL || rp->memcached_response->value == NULL)
			return 0;
		n = snprintf(text_buf, sizeof(text_buf), "VALUE %.*s %" PRIu32 " %" PRIu32 "\r\n",
			rp->memcached_response->key_len, rp->memcached_response->key,
			rp->memcached_response->flags, rp->memcached_response->value_len);
		if (n < 0 || (size_t)n >= sizeof(text_buf))
			return 0;
		text_len = (size_t)n;
		if (text_len + rp->memcached_response->value_len + 7 > sizeof(text_buf))
			return 0;
		memcpy(text_buf + text_len, rp->memcached_response->value,
			rp->memcached_response->value_len);
		text_len += rp->memcached_response->value_len;
		memcpy(text_buf + text_len, "\r\nEND\r\n", 7);
		text_len += 7;
		break;
	}
	case MEMCACHED_RESP_STORED:
		memcpy(text_buf, "STORED\r\n", 8);
		text_len = 8;
		break;
	case MEMCACHED_RESP_ERROR:
		memcpy(text_buf, "ERROR\r\n", 7);
		text_len = 7;
		break;
	case MEMCACHED_RESP_END:
	default:
		memcpy(text_buf, "END\r\n", 5);
		text_len = 5;
		break;
	}

	need = sizeof(struct memcached_payload_header) + text_len;
	if (dst_cap < need)
		return 0;

	hdr = (struct memcached_payload_header *)dst;
	text_part = (char *)(hdr + 1);

	*hdr = rp->memcached_response->header;
	hdr->total_datagrams = rte_cpu_to_be_16(1);
	hdr->rx_burst_time = rte_cpu_to_be_64(rp->rx_burst_time);
	hdr->tx_before_time = rte_cpu_to_be_64(tsc_now());

	memcpy(text_part, text_buf, text_len);
	return need;
}
