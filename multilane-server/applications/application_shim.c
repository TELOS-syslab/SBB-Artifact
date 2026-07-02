#include "applications/application.h"

static const struct application_ops application_ops_table[APP_TYPE_COUNT] = {
	[APP_TYPE_SYNTHETIC] = {
		.app_init_application = synthetic_init_application,
		.app_process_request = synthetic_process_request,
		.app_build_response_payload = synthetic_build_response_payload,
	},
	[APP_TYPE_MEMCACHED] = {
		.app_init_application = memcached_init_application,
		.app_process_request = memcached_process_request,
		.app_build_response_payload = memcached_build_response_payload,
	},
	[APP_TYPE_ROCKSDB] = {
		.app_init_application = rocksdb_init_application,
		.app_process_request = rocksdb_process_request,
		.app_build_response_payload = rocksdb_build_response_payload,
	},
};

static enum app_type application_clamp_app_type(void)
{
	if ((unsigned)global_app_type >= APP_TYPE_COUNT)
		return APP_TYPE_SYNTHETIC;
	return global_app_type;
}

void app_init_application(void)
{
	application_ops_table[application_clamp_app_type()].app_init_application();
}

bool app_process_request(struct rte_mbuf *m,
	uint64_t rx_burst_time, uint32_t processed_time_us,
	void **app_ctx_io,
	struct packet_analysis_result *analysis_result,
	struct app_process_result *result)
{
	return application_ops_table[application_clamp_app_type()].app_process_request(m,
		rx_burst_time, processed_time_us, app_ctx_io, analysis_result, result);
}

size_t app_build_response_payload(const struct response_params *rp,
	void *dst, size_t dst_cap)
{
	return application_ops_table[application_clamp_app_type()].app_build_response_payload(
		rp, dst, dst_cap);
}
