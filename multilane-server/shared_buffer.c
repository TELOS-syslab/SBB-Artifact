#include <errno.h>
#include <string.h>

#include "common.h"

struct active_packet_buffer active_buffers[RTE_MAX_LCORE];
struct deferred_packet_buffer deferred_buffers[RTE_MAX_LCORE];

/*********************************
* Initialization Functions
**********************************/

void active_buffer_init(struct active_packet_buffer *buf)
{
	buf->head = 0;
	buf->tail = 0;
	buf->count = 0;
	rte_spinlock_init(&buf->lock);

	for (unsigned j = 0; j < SHARED_BUFFER_SIZE; j++) {
		buf->packets[j].pkt = NULL;
		buf->packets[j].rx_burst_time = 0;
		buf->packets[j].processed_time_us = 0;
		buf->packets[j].app_ctx = NULL;
	}
}

void deferred_buffer_init(struct deferred_packet_buffer *buf)
{
	buf->head = 0;
	buf->tail = 0;
	buf->count = 0;

	for (unsigned j = 0; j < SHARED_BUFFER_SIZE; j++) {
		buf->packets[j].pkt = NULL;
		buf->packets[j].rx_burst_time = 0;
		buf->packets[j].processed_time_us = 0;
		buf->packets[j].app_ctx = NULL;
	}
}

void init_shared_buffers(void)
{
	for (unsigned i = 0; i < RTE_MAX_LCORE; i++) {
		active_buffer_init(&active_buffers[i]);
		deferred_buffer_init(&deferred_buffers[i]);
		worker_shared_states[i].load_state = WORKER_STATE_LIGHT;
		worker_shared_states[i].published_valid = 0;
		worker_shared_states[i].published_l4_proto = 0;
		worker_shared_states[i].published_l4_src_port = 0;
		worker_shared_states[i].published_src_ip_host = 0;
		rte_spinlock_init(&worker_shared_states[i].published_lock);
		memset(worker_rx_burst_histogram[i], 0, sizeof(worker_rx_burst_histogram[i]));
	}
}

/*********************************
* Active Buffer Functions
**********************************/

bool active_buffer_has_packets(unsigned lcore_id)
{
	struct active_packet_buffer *buf = &active_buffers[lcore_id];
	return (__atomic_load_n(&buf->count, __ATOMIC_RELAXED) > 0);
}

uint32_t active_buffer_add_packets(unsigned lcore_id,
	struct rte_mbuf **pkts, uint32_t nb_pkts, uint64_t rx_burst_time)
{
	struct active_packet_buffer *buf = &active_buffers[lcore_id];
	uint32_t added = 0;

	rte_spinlock_lock(&buf->lock);

	for (uint32_t i = 0; i < nb_pkts; i++) {
		if (buf->count >= SHARED_BUFFER_SIZE)
			break;

		uint32_t index = buf->tail % SHARED_BUFFER_SIZE;
		buf->tail++;
		buf->count++;

		buf->packets[index].pkt = pkts[i];
		buf->packets[index].rx_burst_time = rx_burst_time;
		buf->packets[index].processed_time_us = 0;
		buf->packets[index].app_ctx = NULL;
		added++;
	}

	rte_spinlock_unlock(&buf->lock);
	return added;
}

uint32_t active_buffer_get_packets(unsigned lcore_id,
	struct packet_info *out, uint32_t max_n)
{
	struct active_packet_buffer *buf = &active_buffers[lcore_id];
	uint32_t got = 0;

	if (max_n == 0)
		return 0;

	rte_spinlock_lock(&buf->lock);
	while (got < max_n && buf->count > 0) {
		uint32_t index = buf->head % SHARED_BUFFER_SIZE;
		struct packet_info *slot = &buf->packets[index];
		if (slot->pkt == NULL)
			break;

		out[got] = *slot;
		slot->pkt = NULL;
		slot->rx_burst_time = 0;
		slot->processed_time_us = 0;
		slot->app_ctx = NULL;
		buf->head++;
		buf->count--;
		got++;
	}
	rte_spinlock_unlock(&buf->lock);
	return got;
}

uint32_t active_buffer_steal_half(unsigned target_lcore,
	struct packet_info *stolen_packets, uint32_t max_steal)
{
	struct active_packet_buffer *target_buf = &active_buffers[target_lcore];
	uint32_t actually_stolen = 0;

	if (!rte_spinlock_trylock(&target_buf->lock))
		return 0;

	uint32_t target_count = target_buf->count;
	if (target_count < 2) {
		rte_spinlock_unlock(&target_buf->lock);
		return 0;
	}

	uint32_t steal_count = target_count / 2;
	if (steal_count > max_steal)
		steal_count = max_steal;

	for (uint32_t i = 0; i < steal_count && target_buf->count > 0; i++) {
		uint32_t index = target_buf->head % SHARED_BUFFER_SIZE;
		struct packet_info *slot = &target_buf->packets[index];
		if (slot->pkt == NULL) {
			target_buf->head++;
			target_buf->count--;
			continue;
		}

		stolen_packets[actually_stolen] = *slot;
		slot->pkt = NULL;
		slot->rx_burst_time = 0;
		slot->processed_time_us = 0;
		slot->app_ctx = NULL;
		target_buf->head++;
		target_buf->count--;
		actually_stolen++;
	}

	rte_spinlock_unlock(&target_buf->lock);
	return actually_stolen;
}

/*********************************
* Deferred Buffer Functions
**********************************/

uint32_t deferred_buffer_count(unsigned lcore_id)
{
	struct deferred_packet_buffer *buf = &deferred_buffers[lcore_id];
	return buf->count;
}

int deferred_buffer_enqueue_packet(unsigned lcore_id, struct rte_mbuf *pkt,
	uint64_t rx_burst_time, uint32_t processed_time_us, void *app_ctx)
{
	struct deferred_packet_buffer *buf = &deferred_buffers[lcore_id];
	int ret = 0;

	if (buf->count >= SHARED_BUFFER_SIZE) {
		ret = -ENOBUFS;
	} else {
		uint32_t index = buf->tail % SHARED_BUFFER_SIZE;
		buf->tail++;
		buf->count++;
		buf->packets[index].pkt = pkt;
		buf->packets[index].rx_burst_time = rx_burst_time;
		buf->packets[index].processed_time_us = processed_time_us;
		buf->packets[index].app_ctx = app_ctx;
	}
	return ret;
}

uint32_t deferred_buffer_move_to_active(unsigned lcore_id, uint32_t max_move)
{
	struct deferred_packet_buffer *src = &deferred_buffers[lcore_id];
	struct active_packet_buffer *dst = &active_buffers[lcore_id];
	uint32_t moved = 0;

	if (max_move == 0)
		return 0;

	rte_spinlock_lock(&dst->lock);
	while (moved < max_move && src->count > 0 &&
	       dst->count < SHARED_BUFFER_SIZE) {
		uint32_t src_index = src->head % SHARED_BUFFER_SIZE;
		uint32_t dst_index = dst->tail % SHARED_BUFFER_SIZE;
		struct packet_info *src_slot = &src->packets[src_index];
		struct packet_info *dst_slot = &dst->packets[dst_index];

		if (src_slot->pkt == NULL) {
			src->head++;
			src->count--;
			continue;
		}

		*dst_slot = *src_slot;
		src_slot->pkt = NULL;
		src_slot->rx_burst_time = 0;
		src_slot->processed_time_us = 0;
		src_slot->app_ctx = NULL;
		src->head++;
		src->count--;
		dst->tail++;
		dst->count++;
		moved++;
	}

	if (src->count > 0) 
		rte_panic("CRITICAL BUG: deferred_buffer_move_to_active() src->count > 0\n");

	rte_spinlock_unlock(&dst->lock);
	return moved;
}
