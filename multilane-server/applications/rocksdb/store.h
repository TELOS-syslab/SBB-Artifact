#ifndef APPLICATIONS_ROCKSDB_STORE_H
#define APPLICATIONS_ROCKSDB_STORE_H

#include <rte_mbuf.h>

#include "applications/rocksdb/protocol.h"

void rocksdb_store_init(void);
void rocksdb_preload(uint32_t count);
bool rocksdb_store_get(const struct rocksdb_request *req, struct rocksdb_response *resp);
bool rocksdb_store_scan(struct rte_mbuf *m, uint64_t rx_burst_time,
	uint32_t processed_time_us, void **scan_ctx_io,
	const struct rocksdb_request *req, struct rocksdb_response *resp,
	bool *completed);

#endif /* APPLICATIONS_ROCKSDB_STORE_H */
