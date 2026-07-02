#include <stdio.h>
#include <string.h>

#include "applications/memcached/store.h"

struct memcached_entry {
	bool used;
	uint16_t key_len;
	uint32_t value_len;
	uint32_t flags;
	uint32_t exptime;
	char key[MEMCACHED_MAX_KEY_LEN + 1];
	char value[MEMCACHED_MAX_VALUE_LEN];
	rte_rwlock_t lock;
};

static struct memcached_entry memcached_table[MEMCACHED_MAX_ENTRIES];

void memcached_store_init(void)
{
	memset(memcached_table, 0, sizeof(memcached_table));
	for (int i = 0; i < MEMCACHED_MAX_ENTRIES; i++)
		rte_rwlock_init(&memcached_table[i].lock);
}

void memcached_preload(uint32_t count)
{
	struct memcached_request req;
	int n;

	if (count == 0 || count > MEMCACHED_MAX_ENTRIES)
		count = MEMCACHED_MAX_ENTRIES;

	for (uint32_t i = 0; i < count; i++) {
		memset(&req, 0, sizeof(req));
		req.cmd = MEMCACHED_CMD_SET;
		req.key[0] = 'k';
		req.key[1] = 'e';
		req.key[2] = 'y';
		req.key[3] = (char)('0' + (i / 10));
		req.key[4] = (char)('0' + (i % 10));
		req.key[5] = '\0';
		req.key_len = 5;
		/* value is fixed-width decimal of index: "000" .. "099" */
		n = snprintf(req.value, sizeof(req.value), "%03u", i);
		if (n <= 0 || (size_t)n >= sizeof(req.value))
			continue;
		req.value_len = (uint32_t)n;
		req.flags = 0;
		req.exptime = 60;
		memcached_store_set(&req, NULL);
	}
}

static int find_entry(const char *key, uint16_t key_len)
{
	for (int i = 0; i < MEMCACHED_MAX_ENTRIES; i++) {
		if (!memcached_table[i].used)
			continue;
		if (memcached_table[i].key_len == key_len &&
		    memcmp(memcached_table[i].key, key, key_len) == 0)
			return i;
	}
	return -1;
}

static int find_free_slot(void)
{
	for (int i = 0; i < MEMCACHED_MAX_ENTRIES; i++) {
		if (!memcached_table[i].used)
			return i;
	}
	return 0;
}

bool memcached_store_get(const struct memcached_request *req,
	struct memcached_response *resp)
{
	struct memcached_entry *entry;
	bool found;
	int idx;

	if (req == NULL || resp == NULL)
		return false;
	if (req->key_len == 0 || req->key_len > MEMCACHED_MAX_KEY_LEN)
		return false;

	idx = find_entry(req->key, req->key_len);
	if (idx < 0) {
		resp->type = MEMCACHED_RESP_END;
		return true;
	}

	entry = &memcached_table[idx];
	rte_rwlock_read_lock(&entry->lock);
	found = entry->used &&
		entry->key_len == req->key_len &&
		memcmp(entry->key, req->key, req->key_len) == 0;
	if (found) {
		resp->type = MEMCACHED_RESP_VALUE;
		resp->key = req->key;
		resp->key_len = req->key_len;
		resp->value = entry->value;
		resp->value_len = entry->value_len;
		resp->flags = entry->flags;
	} else {
		resp->type = MEMCACHED_RESP_END;
	}
	rte_rwlock_read_unlock(&entry->lock);
	return true;
}

bool memcached_store_set(const struct memcached_request *req,
	struct memcached_response *resp)
{
	struct memcached_entry *entry;
	int idx;

	if (req == NULL || req->key_len == 0 || req->key_len > MEMCACHED_MAX_KEY_LEN) {
		if (resp != NULL)
			resp->type = MEMCACHED_RESP_ERROR;
		return false;
	}
	if (req->value_len > MEMCACHED_MAX_VALUE_LEN) {
		if (resp != NULL)
			resp->type = MEMCACHED_RESP_ERROR;
		return false;
	}

	idx = find_entry(req->key, req->key_len);
	if (idx < 0)
		idx = find_free_slot();

	entry = &memcached_table[idx];
	rte_rwlock_write_lock(&entry->lock);
	entry->used = true;
	entry->key_len = req->key_len;
	memcpy(entry->key, req->key, req->key_len);
	entry->key[req->key_len] = '\0';
	entry->value_len = req->value_len;
	if (req->value_len > 0)
		rte_memcpy(entry->value, req->value, req->value_len);
	entry->flags = req->flags;
	entry->exptime = req->exptime;
	rte_rwlock_write_unlock(&entry->lock);

	if (resp != NULL)
		resp->type = MEMCACHED_RESP_STORED;
	return true;
}
