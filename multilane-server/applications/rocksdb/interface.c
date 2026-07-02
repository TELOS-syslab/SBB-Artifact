#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <rte_byteorder.h>

#include "common.h"
#include "applications/application.h"
#include "applications/rocksdb/protocol.h"
#include "applications/rocksdb/store.h"

void rocksdb_init_application(void)
{
	rocksdb_store_init();
	rocksdb_preload(ROCKSDB_APP_PRELOAD_ENTRIES);
}

bool rocksdb_process_request(struct rte_mbuf *m, uint64_t rx_burst_time,
	uint32_t processed_time_us, void **scan_ctx_io,
	struct packet_analysis_result *analysis_result,
	struct app_process_result *result)
{
	struct rocksdb_payload_header payload_hdr;
	struct rocksdb_request req;

	if (result == NULL)
		return false;

	if (!rocksdb_parse_request(analysis_result, &payload_hdr, &req)) {
		if (scan_ctx_io != NULL)
			*scan_ctx_io = NULL;
		return false;
	}

	memset(&result->rocksdb_response, 0, sizeof(result->rocksdb_response));
	result->uses_rocksdb_response = true;
	result->rocksdb_response.header = payload_hdr;
	result->rocksdb_request = req;

	switch (req.cmd) {
	case ROCKSDB_CMD_GET:
		if (!rocksdb_store_get(&result->rocksdb_request, &result->rocksdb_response))
			return false;
		break;
	case ROCKSDB_CMD_SCAN:
		if (rocksdb_store_scan(m, rx_burst_time, processed_time_us, scan_ctx_io,
			    &req, &result->rocksdb_response, &result->completed))
			return true;
		break;
	default:
		result->rocksdb_response.type = ROCKSDB_RESP_ERROR;
		break;
	}

	result->completed = true;
	return true;
}

size_t rocksdb_build_response_payload(const struct response_params *rp,
	void *dst, size_t dst_cap)
{
	char text_buf[ROCKSDB_APP_MAX_TEXT_LINE];
	size_t text_len = 0;
	struct rocksdb_payload_header *hdr;
	char *text_part;
	size_t need;

	if (rp->rocksdb_response == NULL)
		return 0;

	switch (rp->rocksdb_response->type) {
	case ROCKSDB_RESP_VALUE: {
		int n;

		if (rp->rocksdb_response->key == NULL || rp->rocksdb_response->value == NULL)
			return 0;
		n = snprintf(text_buf, sizeof(text_buf), "VALUE %.*s %" PRIu32 "\r\n",
			rp->rocksdb_response->key_len, rp->rocksdb_response->key,
			rp->rocksdb_response->value_len);
		if (n < 0 || (size_t)n >= sizeof(text_buf))
			return 0;
		text_len = (size_t)n;
		if (text_len + rp->rocksdb_response->value_len + 2 + 5 > sizeof(text_buf))
			return 0;
		memcpy(text_buf + text_len, rp->rocksdb_response->value,
			rp->rocksdb_response->value_len);
		text_len += rp->rocksdb_response->value_len;
		text_buf[text_len++] = '\r';
		text_buf[text_len++] = '\n';
		memcpy(text_buf + text_len, "END\r\n", 5);
		text_len += 5;
		break;
	}
	case ROCKSDB_RESP_SCAN_RESULT: {
		bool truncated = false;
		size_t text_cap = ROCKSDB_APP_SCAN_RESP_TEXT_CAP < sizeof(text_buf) ?
			ROCKSDB_APP_SCAN_RESP_TEXT_CAP : sizeof(text_buf);
		const size_t end_len = 5;
		const size_t has_more_len = 10;

		if (rp->rocksdb_response->entries == NULL ||
		    rp->rocksdb_response->entry_count == 0)
			return 0;

		for (uint32_t i = 0; i < rp->rocksdb_response->entry_count; i++) {
			struct scan_entry *entry = &rp->rocksdb_response->entries[i];
			size_t trailer_reserve = (rp->rocksdb_response->has_more ||
				i + 1 < rp->rocksdb_response->entry_count) ?
				(has_more_len + end_len) : end_len;
			int n;

			if (text_len + trailer_reserve >= text_cap) {
				truncated = true;
				break;
			}
			n = snprintf(text_buf + text_len, text_cap - text_len,
				"SCAN_ENTRY %.*s %" PRIu32 "\r\n",
				entry->key_len, entry->key, entry->value_len);
			if (n < 0 || text_len + (size_t)n + trailer_reserve >= text_cap) {
				truncated = true;
				break;
			}
			text_len += (size_t)n;
			if (text_len + entry->value_len + 2 + trailer_reserve >= text_cap) {
				truncated = true;
				break;
			}
			memcpy(text_buf + text_len, entry->value, entry->value_len);
			text_len += entry->value_len;
			text_buf[text_len++] = '\r';
			text_buf[text_len++] = '\n';
		}

		if (rp->rocksdb_response->has_more || truncated) {
			if (text_len + has_more_len > text_cap)
				return 0;
			memcpy(text_buf + text_len, "HAS_MORE\r\n", has_more_len);
			text_len += has_more_len;
		}

		if (text_len + end_len > text_cap)
			return 0;
		memcpy(text_buf + text_len, "END\r\n", end_len);
		text_len += end_len;
		break;
	}
	case ROCKSDB_RESP_ERROR:
		memcpy(text_buf, "ERROR\r\n", 7);
		text_len = 7;
		break;
	case ROCKSDB_RESP_END:
	default:
		memcpy(text_buf, "END\r\n", 5);
		text_len = 5;
		break;
	}

	need = sizeof(struct rocksdb_payload_header) + text_len;
	if (dst_cap < need)
		return 0;

	hdr = (struct rocksdb_payload_header *)dst;
	text_part = (char *)(hdr + 1);

	*hdr = rp->rocksdb_response->header;
	hdr->total_datagrams = rte_cpu_to_be_16(1);
	hdr->rx_burst_time = rte_cpu_to_be_64(rp->rx_burst_time);
	hdr->tx_before_time = rte_cpu_to_be_64(tsc_now());

	memcpy(text_part, text_buf, text_len);
	return need;
}
