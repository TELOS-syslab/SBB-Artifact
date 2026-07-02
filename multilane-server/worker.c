#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common.h"
#include "applications/application.h"

#define DEV_UPID_PAGE_CTL "/dev/upid_page_ctl"
#define UINTR_UPID_CTL_IOCTL_MAGIC 'U'
#define UINTR_UPID_CTL_GET_OFFSET _IOR(UINTR_UPID_CTL_IOCTL_MAGIC, 0x01, int)

__thread struct worker_context *active_worker_ctx;

uint64_t tsc_now(void)
{
	unsigned int lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

static void process_single_packet(struct rte_mbuf *m, uint64_t rx_burst_time,
	uint32_t processed_time_us, void **app_ctx_io)
{
	struct packet_analysis_result analysis_result;
	bool is_request = analyze_packet_content(m, &analysis_result);
	if (!is_request) {
		rte_pktmbuf_free(m);
		return;
	}

	struct app_process_result app_result = {0};
	if (!app_process_request(m, rx_burst_time, processed_time_us, app_ctx_io,
			&analysis_result, &app_result)) {
		rte_pktmbuf_free(m);
		return;
	}
	if (!app_result.completed)
		return;

	struct response_params rp = {
		.src_ip = analysis_result.src_ip,
		.dst_ip = analysis_result.dst_ip,
		.l4_src_port = analysis_result.l4_src_port,
		.l4_dst_port = analysis_result.l4_dst_port,
		.l4_proto = analysis_result.l4_proto,
		.processing_time = analysis_result.processing_time,
		.sequence_number = analysis_result.sequence_number,
		.timestamp_send = analysis_result.timestamp_send,
		.rx_burst_time = rx_burst_time,
		.analysis_start_time = analysis_result.analysis_start_time,
		.analysis_end_time = analysis_result.analysis_end_time,
		.eth_hdr = analysis_result.eth_hdr,
		.ipv4_hdr = analysis_result.ipv4_hdr,
		.udp_hdr = analysis_result.udp_hdr,
		.memcached_response = app_result.uses_memcached_response ? &app_result.memcached_response : NULL,
		.rocksdb_response = app_result.uses_rocksdb_response ? &app_result.rocksdb_response : NULL,
	};
	send_response_packet(&rp);

	rte_pktmbuf_free(m);
}

void process_received_packets(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct packet_info own_batch[MAX_POP_BATCH];
	uint16_t portid = active_worker_ctx->port_id;
	uint16_t queue_id = active_worker_ctx->queue_id;
	unsigned int lcore_id = rte_lcore_id();

	while (1) {
		uint32_t deferred_count = deferred_buffer_count(lcore_id);
		uint16_t rx_budget = SHARED_BUFFER_SIZE - deferred_count;
		uint16_t nb_rx;
		uint16_t direct_count = 0;
		uint16_t queued_count = 0;
		uint64_t rx_burst_time = 0;

		if (rx_budget > 0) {
			rx_burst_time = tsc_now();
			nb_rx = rte_eth_rx_burst(portid, queue_id, pkts_burst, rx_budget);
		} else {
			nb_rx = 0;
		}

		if (nb_rx >= RX_BURST_HISTOGRAM_SIZE) {
			rte_panic("CRITICAL BUG: nb_rx=%u exceeds rx_burst_histogram size=%u on core %u\n",
				nb_rx, RX_BURST_HISTOGRAM_SIZE, lcore_id);
		}
		worker_rx_burst_histogram[lcore_id][nb_rx]++;

		update_worker_load_state(lcore_id, nb_rx);

		direct_count = RTE_MIN(nb_rx, (uint16_t)SHARE_THRESHOLD);
		queued_count = nb_rx - direct_count;

		// Only packets beyond the threshold are exposed to work stealing.
		uint32_t added = 0;
		if (queued_count > 0)
			added = active_buffer_add_packets(lcore_id, &pkts_burst[direct_count],
				queued_count, rx_burst_time);

		if (added < queued_count) {
			rte_panic("CRITICAL BUG: active_buffer_add_packets() added %u < %u packets on core %u. "
						"Buffer size=%d, burst size=%d. This indicates a serious concurrency bug!\n",
						added, queued_count, lcore_id, SHARED_BUFFER_SIZE, MAX_PKT_BURST);
		}

		for (uint16_t i = 0; i < direct_count; i++) 
			process_single_packet(pkts_burst[i], rx_burst_time, 0, NULL);

		if (deferred_count > 0)
			deferred_buffer_move_to_active(lcore_id, SHARED_BUFFER_SIZE);

		/* Process all packets from active until empty. */
		uint32_t own_batch_count;
		while ((own_batch_count = active_buffer_get_packets(lcore_id, own_batch, MAX_POP_BATCH)) > 0) {
			for (uint32_t i = 0; i < own_batch_count; i++) {
				process_single_packet(own_batch[i].pkt, own_batch[i].rx_burst_time,
					own_batch[i].processed_time_us, &own_batch[i].app_ctx);
			}
			if (own_batch_count < MAX_POP_BATCH)
				break;
		}

		if (nb_rx == 0 && deferred_buffer_count(lcore_id) == 0)
			break;
	}
}

static bool init_worker_ctx(unsigned int lcore_id, uint16_t self_worker_idx,
	struct worker_context *local_ctx)
{
	if (self_worker_idx == INVALID_QUEUE_ID || self_worker_idx >= global_queue_num) {
		rte_panic("CRITICAL BUG: core %u has invalid queue mapping=%u, global_queue_num=%u\n",
			lcore_id, self_worker_idx, global_queue_num);
	}

	if (global_nic_port_id == RTE_MAX_ETHPORTS) {
		rte_panic("CRITICAL BUG: core %u has invalid global_nic_port_id\n", lcore_id);
	}

	memset(local_ctx, 0, sizeof(*local_ctx));

	local_ctx->port_id = global_nic_port_id;
	local_ctx->queue_id = self_worker_idx;
	local_ctx->last_stolen_from = lcore_id + 1;
	if (local_ctx->last_stolen_from >= global_first_enabled_lcore + global_queue_num)
		local_ctx->last_stolen_from = global_first_enabled_lcore;
	local_ctx->lapic_fd = -1;
	local_ctx->lapic = NULL;
	local_ctx->timer_registered = false;
	local_ctx->lapic_tmict_value = 0;
	local_ctx->app_running = false;
	local_ctx->packet_preempted = false;
	local_ctx->busy_streak = 0;
	local_ctx->light_streak = 0;
	local_ctx->overloaded_streak = 0;
	local_ctx->publish_next_overloaded_flow = false;
	local_ctx->upid_fd = -1;
	local_ctx->upid_idx = -1;
	local_ctx->upid_page = NULL;
	local_ctx->upid = NULL;
	local_ctx->pending = 1;
	active_worker_ctx = local_ctx;

	if (local_ctx->queue_id < global_queue_num)
		local_ctx->irq = queue_irq_map[local_ctx->queue_id];
	else
		RTE_LOG(WARNING, ML, "Core %u has no IRQ for queue %u\n",
			lcore_id, local_ctx->queue_id);

	return true;
}

static bool configure_queue(unsigned int lcore_id, struct worker_context *local_ctx)
{
	int ret = rte_eth_dev_rx_intr_enable(local_ctx->port_id, local_ctx->queue_id);
	if (ret < 0) {
		RTE_LOG(ERR, ML,
			"Core %u: failed to enable Rx interrupt on port %u: %s\n",
			lcore_id, local_ctx->port_id, rte_strerror(-ret));
		active_worker_ctx = NULL;
		return false;
	}

	if (!register_user_interrupt(local_ctx)) {
		active_worker_ctx = NULL;
		return false;
	}

	ret = register_queue_task(local_ctx->irq);
	if (ret < 0) {
		RTE_LOG(WARNING, ML,
			"Core %u: failed to register queue task for queue_id %u, continuing anyway\n",
			lcore_id, local_ctx->queue_id);
	}

	return true;
}

static bool configure_upid_mapping(unsigned int lcore_id, struct worker_context *local_ctx)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t entry_count;

	if (page_size <= 0) {
		RTE_LOG(ERR, ML,
			"Core %u: failed to get page size for UPID mapping\n",
			lcore_id);
		return false;
	}

	local_ctx->upid_fd = open(DEV_UPID_PAGE_CTL, O_RDWR | O_CLOEXEC);
	if (local_ctx->upid_fd < 0) {
		RTE_LOG(ERR, ML,
			"Core %u: failed to open %s: %s\n",
			lcore_id, DEV_UPID_PAGE_CTL, strerror(errno));
		return false;
	}

	if (ioctl(local_ctx->upid_fd, UINTR_UPID_CTL_GET_OFFSET, &local_ctx->upid_idx) < 0) {
		RTE_LOG(ERR, ML,
			"Core %u: failed to get UPID index: %s\n",
			lcore_id, strerror(errno));
		close(local_ctx->upid_fd);
		local_ctx->upid_fd = -1;
		return false;
	}

	local_ctx->upid_page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		MAP_SHARED, local_ctx->upid_fd, 0);
	if (local_ctx->upid_page == MAP_FAILED) {
		RTE_LOG(ERR, ML,
			"Core %u: failed to mmap UPID page: %s\n",
			lcore_id, strerror(errno));
		close(local_ctx->upid_fd);
		local_ctx->upid_fd = -1;
		local_ctx->upid_page = NULL;
		return false;
	}

	entry_count = (size_t)page_size / sizeof(struct uintr_upid_user);
	if (local_ctx->upid_idx < 0 || (size_t)local_ctx->upid_idx >= entry_count) {
		RTE_LOG(ERR, ML,
			"Core %u: got invalid UPID index %d (entry_count=%zu)\n",
			lcore_id, local_ctx->upid_idx, entry_count);
		munmap(local_ctx->upid_page, (size_t)page_size);
		local_ctx->upid_page = NULL;
		close(local_ctx->upid_fd);
		local_ctx->upid_fd = -1;
		local_ctx->upid_idx = -1;
		return false;
	}

	local_ctx->upid = &((struct uintr_upid_user *)local_ctx->upid_page)[local_ctx->upid_idx];
	__atomic_fetch_or(&local_ctx->upid->puir,
		1ULL << UINTR_UPID_PUIR_SET_BIT, __ATOMIC_SEQ_CST);

	return true;
}

static bool configure_timer(unsigned int lcore_id, struct worker_context *local_ctx)
{
	int lapic_ret = register_lapic_ctl();
	if (lapic_ret < 0) {
		RTE_LOG(ERR, ML,
			"Core %u: failed to register LAPIC control, cannot use timer interrupt\n",
			lcore_id);
		return false;
	}

	local_ctx->lapic_fd = open("/dev/lapic_ctl", O_RDWR);
	if (local_ctx->lapic_fd < 0) {
		unregister_lapic_ctl();
		RTE_LOG(ERR, ML,
			"Core %u: failed to open /dev/lapic_ctl: %s\n",
			lcore_id, strerror(errno));
		return false;
	}

	local_ctx->lapic = (volatile uint32_t *)mmap(NULL, getpagesize(),
		PROT_READ | PROT_WRITE, MAP_SHARED, local_ctx->lapic_fd, 0);
	if (local_ctx->lapic == MAP_FAILED) {
		close(local_ctx->lapic_fd);
		local_ctx->lapic_fd = -1;
		local_ctx->lapic = NULL;
		unregister_lapic_ctl();
		RTE_LOG(ERR, ML,
			"Core %u: failed to mmap LAPIC: %s\n",
			lcore_id, strerror(errno));
		return false;
	}

	RTE_LOG(INFO, ML,
		"Core %u: successfully mmapped LAPIC registers, initial LVTT=0x%08x\n",
		lcore_id, local_ctx->lapic[APIC_LVTT / 4]);

	int timer_ret = register_timer_interrupt(TIMER_HZ, local_ctx->timer_vector);
	if (timer_ret < 0) {
		munmap((void *)local_ctx->lapic, getpagesize());
		local_ctx->lapic = NULL;
		close(local_ctx->lapic_fd);
		local_ctx->lapic_fd = -1;
		unregister_lapic_ctl();
		RTE_LOG(ERR, ML,
			"Core %u: failed to register timer interrupt\n",
			lcore_id);
		return false;
	}

	local_ctx->timer_registered = true;

	uint32_t tmict_val = local_ctx->lapic[APIC_TMICT / 4];
	local_ctx->lapic_tmict_value = tmict_val;
	uint32_t lvtt_current = local_ctx->lapic[APIC_LVTT / 4];
	local_ctx->lapic_lvtt_unmask_value = lvtt_current & ~APIC_LVTT_MASK_BIT;
	local_ctx->lapic_lvtt_mask_value = lvtt_current | APIC_LVTT_MASK_BIT;
	
	RTE_LOG(INFO, ML,
		"Core %u: registered timer interrupt at %u Hz (vector=%u), default masked (LVTT=0x%08x, TMICT=0x%08x)\n",
		lcore_id, TIMER_HZ, local_ctx->timer_vector,
		lvtt_current, tmict_val);
	return true;
}

// MultiLane currently assumes workers are continuous to perform load balancing.
static bool perform_load_balancing(unsigned int lcore_id, struct worker_context *local_ctx, unsigned int try_num)
{
	struct packet_info stolen_packets[MAX_PKT_BURST];
	unsigned int last_lcore = global_first_enabled_lcore + global_queue_num - 1;
	unsigned int start_lcore = local_ctx->last_stolen_from;
	unsigned int target_lcore = start_lcore;
	unsigned int traversed = 0;
	bool found_work = false;

	if (try_num == 0)
		return false;

	if (start_lcore < global_first_enabled_lcore || start_lcore > last_lcore)
		rte_panic("CRITICAL BUG: core %u has invalid last_stolen_from=%u, valid range is [%u, %u]\n",
			lcore_id, start_lcore, global_first_enabled_lcore, last_lcore);

	do {
		if (target_lcore != lcore_id) {
			uint8_t target_state = __atomic_load_n(
				&worker_shared_states[target_lcore].load_state,
				__ATOMIC_RELAXED);
			if (target_state == WORKER_STATE_OVERLOADED) {
				int mig_ret = try_migrate_overloaded_flow(
					target_lcore, lcore_id);
				if (mig_ret == 0) {
					found_work = true;
					local_ctx->last_stolen_from = target_lcore;
					break;
				}
			}

			if (target_state != WORKER_STATE_LIGHT && active_buffer_has_packets(target_lcore)) {
				// Try to steal half of the packets from target core.
				uint32_t stolen_count = active_buffer_steal_half(target_lcore, 
					stolen_packets, MAX_PKT_BURST);

				if (stolen_count > 0) {
					found_work = true;
					local_ctx->last_stolen_from = target_lcore;

					for (unsigned int i = 0; i < stolen_count; i++) {
						process_single_packet(stolen_packets[i].pkt,
							stolen_packets[i].rx_burst_time,
							stolen_packets[i].processed_time_us,
							&stolen_packets[i].app_ctx);
					}

					break;
				}
			}
		}

		target_lcore++;
		if (target_lcore > last_lcore)
			target_lcore = global_first_enabled_lcore;
		traversed++;
		if (traversed >= try_num)
			break;
	} while (target_lcore != start_lcore && !found_work);

	return found_work;
}

static void run_worker_loop(unsigned int lcore_id, struct worker_context *local_ctx)
{
	int wait_debug_count = 0;

	while (!global_quit_signal) {
		bool packets_processed = false;

		if (local_ctx->pending) {
			local_ctx->pending = 0;
			packets_processed = true;

			process_received_packets();
			int rc = rte_eth_dev_rx_intr_enable(local_ctx->port_id, local_ctx->queue_id);
			if (rc < 0) {
				RTE_LOG(ERR, ML,
					"Core %u: failed to enable Rx interrupt on port %u: %s\n",
					lcore_id, local_ctx->port_id, rte_strerror(-rc));
			}
		}

		if (!packets_processed) {
			rte_eth_dev_rx_intr_disable(local_ctx->port_id, local_ctx->queue_id);
			if (global_enable_load_balance)
				packets_processed = perform_load_balancing(lcore_id, local_ctx,
					MAX_LOAD_BALANCING_TRIES);
			rte_eth_dev_rx_intr_enable(local_ctx->port_id, local_ctx->queue_id);

			if (!packets_processed && global_enable_colocation) {
				if (local_ctx->pending == 0) {
					user_intr_disable();
					(void)uintr_wait(WAIT_TIMEOUT);
					user_intr_enable();
				}
			}
		}
	}
}

static void resume_timer(unsigned int lcore_id, struct worker_context *local_ctx)
{
	if (local_ctx->timer_registered) {
		int timer_ret = unregister_timer_interrupt();
		if (timer_ret < 0) {
			RTE_LOG(WARNING, ML,
				"Core %u: failed to unregister timer interrupt\n",
				lcore_id);
		}
	}

	if (local_ctx->lapic != NULL) {
		RTE_LOG(INFO, ML,
			"Core %u: final LAPIC LVTT=0x%08x\n",
			lcore_id, local_ctx->lapic[APIC_LVTT / 4]);
		munmap((void *)local_ctx->lapic, getpagesize());
		local_ctx->lapic = NULL;
	}

	if (local_ctx->lapic_fd >= 0) {
		close(local_ctx->lapic_fd);
		local_ctx->lapic_fd = -1;
	}

	if (local_ctx->timer_registered)
		unregister_lapic_ctl();

	local_ctx->timer_registered = false;
}

static void resume_queue(unsigned int lcore_id, struct worker_context *local_ctx)
{
	if (local_ctx->upid_page != NULL) {
		munmap(local_ctx->upid_page, (size_t)getpagesize());
		local_ctx->upid_page = NULL;
		local_ctx->upid = NULL;
	}

	if (local_ctx->upid_fd >= 0) {
		close(local_ctx->upid_fd);
		local_ctx->upid_fd = -1;
	}

	rte_eth_dev_rx_intr_disable(local_ctx->port_id, local_ctx->queue_id);

	int ret = unregister_queue_task(local_ctx->irq);
	if (ret < 0) {
		RTE_LOG(WARNING, ML,
			"Core %u: failed to unregister queue task for queue_id %u\n",
			lcore_id, local_ctx->queue_id);
	}
}

void worker_run(void)
{
	unsigned int lcore_id = rte_lcore_id();
	uint16_t self_worker_idx = lcore_queue_map[lcore_id];
	struct worker_context local_ctx;

	if (!init_worker_ctx(lcore_id, self_worker_idx, &local_ctx))
		return;

	if (!configure_queue(lcore_id, &local_ctx))
		return;

	if (!configure_upid_mapping(lcore_id, &local_ctx)) {
		resume_queue(lcore_id, &local_ctx);
		active_worker_ctx = NULL;
		return;
	}

	if (global_enable_timer) {
		if (!configure_timer(lcore_id, &local_ctx)) {
			resume_queue(lcore_id, &local_ctx);
			active_worker_ctx = NULL;
			return;
		}
	}

	RTE_LOG(INFO, ML,
		"Core %u: using user interrupts (irq=%u) for queue %u\n",
		lcore_id, local_ctx.irq, local_ctx.queue_id);

	run_worker_loop(lcore_id, &local_ctx);

	user_intr_disable();

	if (global_enable_timer)
		resume_timer(lcore_id, &local_ctx);
	resume_queue(lcore_id, &local_ctx);

	active_worker_ctx = NULL;
}
