#ifndef APPLICATIONS_MEMCACHED_STORE_H
#define APPLICATIONS_MEMCACHED_STORE_H

#include "applications/memcached/protocol.h"

#define MEMCACHED_MAX_ENTRIES 100

void memcached_store_init(void);
void memcached_preload(uint32_t count);
bool memcached_store_get(const struct memcached_request *req,
	struct memcached_response *resp);
bool memcached_store_set(const struct memcached_request *req,
	struct memcached_response *resp);

#endif /* APPLICATIONS_MEMCACHED_STORE_H */
