# Applications layer

This directory holds **per-application implementations** for MultiLane server. A thin **`application_shim.c`** dispatches to the active backend according to `global_app_type`.

## Current applications

| `-A` value     | `enum app_type`        | Directory              |
|----------------|------------------------|------------------------|
| `synthetic`    | `APP_TYPE_SYNTHETIC`   | `synthetic/`           |
| `memcached`    | `APP_TYPE_MEMCACHED`   | `memcached/`           | 
| `rocksdb`      | `APP_TYPE_ROCKSDB`     | `rocksdb/`             | 

We find that `Get` and `Scan` request processing time varies across different machine environments, so we also implement rocksdb pattern in synthetic application for convenience.

## Porting a new application

Rough checklist:

1. **`args.c`**  
   - Extend `parse_app_type()` and the `-A` usage line in `multilane_usage()`.

2. **New subdirectory** `applications/<APP>/`  
   - Add **`interface.c`** implementing:
     - `void <APP>_init_application(void);`
     - `bool <APP>_process_request(struct rte_mbuf *m, uint64_t rx_burst_time, uint32_t processed_time_us, void **app_ctx_io, struct packet_analysis_result *, struct app_process_result *);`
     - `size_t <APP>_build_response_payload(const struct response_params *rp, void *dst, size_t dst_cap);`  

3. **`application.h`**  
   - Declare the three `<APP>_…` functions.

4. **`application_shim.c`**  
   - Add one row to `application_ops_table` for `APP_TYPE_<APP>` pointing at your three functions.

### Interface file (`interface.c`)

Treat **`interface.c`** as the only place that **implements the three hooks** required by `application_ops`. Keep it focused on:

- Initial setup in `<APP>_init_application`.
- Process the request in application layer in `<APP>_process_request`.
- Write the variable-length application payload into `dst` in `<APP>_build_response_payload`.

