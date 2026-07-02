#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "applications/rocksdb/protocol.h"

static bool parse_u32_token(const char *data, size_t len, uint32_t *out)
{
	char buf[16];

	if (len == 0 || len >= sizeof(buf))
		return false;

	memcpy(buf, data, len);
	buf[len] = '\0';

	char *end = NULL;
	errno = 0;
	unsigned long val = strtoul(buf, &end, 10);
	if (errno != 0 || end == buf || *end != '\0')
		return false;
	if (val > UINT32_MAX)
		return false;

	*out = (uint32_t)val;
	return true;
}

static bool next_token(const char *data, size_t len, size_t *offset,
	const char **token, size_t *token_len)
{
	while (*offset < len && data[*offset] == ' ')
		(*offset)++;

	if (*offset >= len)
		return false;

	size_t start = *offset;
	while (*offset < len && data[*offset] != ' ' &&
	       data[*offset] != '\r' && data[*offset] != '\n')
		(*offset)++;

	if (*offset == start)
		return false;

	*token = data + start;
	*token_len = *offset - start;
	return true;
}

static bool skip_crlf(const char *data, size_t len, size_t *offset)
{
	if (*offset >= len)
		return false;

	if (data[*offset] == '\r') {
		(*offset)++;
		if (*offset < len && data[*offset] == '\n') {
			(*offset)++;
			return true;
		}
		return false;
	}

	if (data[*offset] == '\n') {
		(*offset)++;
		return true;
	}

	return false;
}

static bool token_equals(const char *token, size_t len, const char *match)
{
	size_t match_len = strlen(match);

	if (len != match_len)
		return false;

	for (size_t i = 0; i < match_len; i++) {
		if (tolower((unsigned char)token[i]) !=
		    tolower((unsigned char)match[i]))
			return false;
	}

	return true;
}

static void copy_token(char *dst, size_t dst_cap, const char *tok, size_t tok_len,
	uint16_t *out_len)
{
	memcpy(dst, tok, tok_len);
	dst[tok_len] = '\0';
	*out_len = (uint16_t)tok_len;
}

bool rocksdb_parse_request(const struct packet_analysis_result *analysis_result,
	struct rocksdb_payload_header *payload_hdr, struct rocksdb_request *req)
{
	const char *text;
	size_t text_len;
	size_t offset = 0;
	const char *cmd_token;
	size_t cmd_len;

	if (payload_hdr == NULL)
		return false;
	memset(req, 0, sizeof(*req));

	if (analysis_result == NULL || analysis_result->app_payload == NULL)
		return false;
	if (analysis_result->l4_proto != IPPROTO_UDP || analysis_result->udp_hdr == NULL)
		return false;
	if (analysis_result->l4_dst_port != ROCKSDB_APP_DEFAULT_PORT)
		return false;
	if (analysis_result->app_payload_len <= sizeof(struct rocksdb_payload_header))
		return false;

	memcpy(payload_hdr, analysis_result->app_payload, sizeof(*payload_hdr));
	text = (const char *)analysis_result->app_payload + sizeof(struct rocksdb_payload_header);
	text_len = analysis_result->app_payload_len - sizeof(struct rocksdb_payload_header);
	if (text_len == 0)
		return false;

	if (!next_token(text, text_len, &offset, &cmd_token, &cmd_len))
		return false;

	if (token_equals(cmd_token, cmd_len, "get")) {
		const char *key_token;
		size_t key_len;

		if (!next_token(text, text_len, &offset, &key_token, &key_len))
			return false;
		if (key_len == 0 || key_len > ROCKSDB_APP_MAX_KEY_LEN)
			return false;

		while (offset < text_len && text[offset] == ' ')
			offset++;
		if (!skip_crlf(text, text_len, &offset))
			return false;

		copy_token(req->key, sizeof(req->key), key_token, key_len, &req->key_len);
		req->cmd = ROCKSDB_CMD_GET;
		req->valid = true;
		return true;
	} else if (token_equals(cmd_token, cmd_len, "scan")) {
		const char *start_key_token = NULL;
		size_t start_key_len = 0;
		const char *end_key_token = NULL;
		size_t end_key_len = 0;
		uint32_t limit = 100;

		if (next_token(text, text_len, &offset, &start_key_token, &start_key_len)) {
			if (start_key_len > 0 && start_key_len <= ROCKSDB_APP_MAX_KEY_LEN) {
				uint32_t tmp_limit;

				if (parse_u32_token(start_key_token, start_key_len, &tmp_limit)) {
					limit = tmp_limit;
					if (limit == 0 || limit > ROCKSDB_APP_MAX_SCAN_LIMIT)
						limit = ROCKSDB_APP_MAX_SCAN_LIMIT;
				} else {
					copy_token(req->start_key, sizeof(req->start_key),
						start_key_token, start_key_len, &req->start_key_len);

					if (next_token(text, text_len, &offset, &end_key_token, &end_key_len)) {
						if (end_key_len > 0 &&
						    end_key_len <= ROCKSDB_APP_MAX_KEY_LEN) {
							if (parse_u32_token(end_key_token, end_key_len,
								    &tmp_limit)) {
								limit = tmp_limit;
								if (limit == 0 ||
								    limit > ROCKSDB_APP_MAX_SCAN_LIMIT)
									limit = ROCKSDB_APP_MAX_SCAN_LIMIT;
							} else {
								copy_token(req->end_key, sizeof(req->end_key),
									end_key_token, end_key_len,
									&req->end_key_len);

								const char *limit_token;
								size_t limit_len;

								if (next_token(text, text_len, &offset,
									    &limit_token, &limit_len) &&
								    parse_u32_token(limit_token, limit_len,
									    &tmp_limit)) {
									limit = tmp_limit;
									if (limit == 0 ||
									    limit > ROCKSDB_APP_MAX_SCAN_LIMIT)
										limit = ROCKSDB_APP_MAX_SCAN_LIMIT;
								}
							}
						}
					}
				}
			}
		}

		while (offset < text_len && text[offset] == ' ')
			offset++;
		if (!skip_crlf(text, text_len, &offset))
			return false;

		req->cmd = ROCKSDB_CMD_SCAN;
		req->limit = limit;
		req->valid = true;
		return true;
	} else {
		return false;
	}

	return false;
}
