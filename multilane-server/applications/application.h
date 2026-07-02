#ifndef APPLICATIONS_APPLICATION_H
#define APPLICATIONS_APPLICATION_H

#include "common.h"
#include "applications/memcached/protocol.h"
#include "applications/rocksdb/protocol.h"
#include "applications/synthetic/workload.h"

struct app_process_result {
	bool completed;
	bool uses_rocksdb_response;
	bool uses_memcached_response;
	struct rocksdb_request rocksdb_request;
	struct rocksdb_response rocksdb_response;
	struct memcached_request memcached_request;
	struct memcached_response memcached_response;
};

struct application_ops {
	void (*app_init_application)(void);
	bool (*app_process_request)(struct rte_mbuf *m, uint64_t rx_burst_time,
		uint32_t processed_time_us, void **app_ctx_io,
		struct packet_analysis_result *analysis_result,
		struct app_process_result *result);
	size_t (*app_build_response_payload)(const struct response_params *rp,
		void *dst, size_t dst_cap);
};

/*********************************
* Application Hooks
**********************************/

void app_init_application(void);
void synthetic_init_application(void);
void memcached_init_application(void);
void rocksdb_init_application(void);

bool app_process_request(struct rte_mbuf *m, uint64_t rx_burst_time, uint32_t processed_time_us,
	void **app_ctx_io, struct packet_analysis_result *analysis_result,
	struct app_process_result *result);
bool synthetic_process_request(struct rte_mbuf *m, uint64_t rx_burst_time,
	uint32_t processed_time_us, void **app_ctx_io,
	struct packet_analysis_result *analysis_result,
	struct app_process_result *result);
bool memcached_process_request(struct rte_mbuf *m, uint64_t rx_burst_time,
	uint32_t processed_time_us, void **app_ctx_io,
	struct packet_analysis_result *analysis_result,
	struct app_process_result *result);
bool rocksdb_process_request(struct rte_mbuf *m, uint64_t rx_burst_time,
	uint32_t processed_time_us, void **app_ctx_io,
	struct packet_analysis_result *analysis_result,
	struct app_process_result *result);

size_t app_build_response_payload(const struct response_params *rp,
	void *dst, size_t dst_cap);
size_t synthetic_build_response_payload(const struct response_params *rp,
	void *dst, size_t dst_cap);
size_t memcached_build_response_payload(const struct response_params *rp,
	void *dst, size_t dst_cap);
size_t rocksdb_build_response_payload(const struct response_params *rp,
	void *dst, size_t dst_cap);

#endif /* APPLICATIONS_APPLICATION_H */
