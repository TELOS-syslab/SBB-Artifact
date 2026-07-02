#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "applications/memcached/protocol.h"

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

static void copy_token(char *dst, size_t dst_cap, const char *token, size_t token_len,
	uint16_t *out_len)
{
	memcpy(dst, token, token_len);
	dst[token_len] = '\0';
	*out_len = (uint16_t)token_len;
}

bool memcached_parse_request(const struct packet_analysis_result *analysis_result,
	struct memcached_payload_header *payload_hdr, struct memcached_request *req)
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
	if (analysis_result->l4_dst_port != MEMCACHED_DEFAULT_PORT)
		return false;
	if (analysis_result->app_payload_len <= sizeof(struct memcached_payload_header))
		return false;

	memcpy(payload_hdr, analysis_result->app_payload, sizeof(*payload_hdr));
	text = (const char *)analysis_result->app_payload + sizeof(struct memcached_payload_header);
	text_len = analysis_result->app_payload_len - sizeof(struct memcached_payload_header);
	if (!next_token(text, text_len, &offset, &cmd_token, &cmd_len))
		return false;

	if (token_equals(cmd_token, cmd_len, "get")) {
		const char *key_token;
		size_t key_len;

		if (!next_token(text, text_len, &offset, &key_token, &key_len))
			return false;
		if (key_len == 0 || key_len > MEMCACHED_MAX_KEY_LEN)
			return false;

		while (offset < text_len && text[offset] == ' ')
			offset++;
		if (!skip_crlf(text, text_len, &offset))
			return false;

		copy_token(req->key, sizeof(req->key), key_token, key_len, &req->key_len);
		req->cmd = MEMCACHED_CMD_GET;
		req->valid = true;
		return true;
	} else if (token_equals(cmd_token, cmd_len, "set")) {
		const char *key_token = NULL;
		const char *flags_token = NULL;
		const char *exp_token = NULL;
		const char *bytes_token = NULL;
		size_t key_len = 0;
		size_t flags_len = 0;
		size_t exp_len = 0;
		size_t bytes_len = 0;
		uint32_t flags;
		uint32_t exptime;
		uint32_t value_len;

		if (!next_token(text, text_len, &offset, &key_token, &key_len) ||
		    !next_token(text, text_len, &offset, &flags_token, &flags_len) ||
		    !next_token(text, text_len, &offset, &exp_token, &exp_len) ||
		    !next_token(text, text_len, &offset, &bytes_token, &bytes_len))
			return false;

		if (key_len == 0 || key_len > MEMCACHED_MAX_KEY_LEN)
			return false;

		if (!parse_u32_token(flags_token, flags_len, &flags) ||
		    !parse_u32_token(exp_token, exp_len, &exptime) ||
		    !parse_u32_token(bytes_token, bytes_len, &value_len))
			return false;

		if (value_len > MEMCACHED_MAX_VALUE_LEN)
			return false;

		while (offset < text_len && text[offset] == ' ')
			offset++;

		if (!skip_crlf(text, text_len, &offset))
			return false;

		if (offset + value_len > text_len)
			return false;

		copy_token(req->key, sizeof(req->key), key_token, key_len, &req->key_len);
		memcpy(req->value, text + offset, value_len);
		req->value_len = value_len;
		req->flags = flags;
		req->exptime = exptime;
		req->cmd = MEMCACHED_CMD_SET;
		req->valid = true;

		offset += value_len;
		return skip_crlf(text, text_len, &offset);
	}

	return false;
}
