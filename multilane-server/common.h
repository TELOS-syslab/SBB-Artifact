#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_epoll.h>
#include <rte_pause.h>
#include <rte_errno.h>
#include <rte_dev.h>
#include <rte_bus.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_icmp.h>
#include <rte_arp.h>
#include <rte_spinlock.h>

/*********************************
* Constants
**********************************/

#define RTE_LOGTYPE_ML RTE_LOGTYPE_USER1
#define ML_WORKER_QUIT_SIGNAL SIGUSR1
#define INVALID_QUEUE_ID UINT16_MAX

#define MAX_PKT_BURST 64
#define SHARED_BUFFER_SIZE MAX_PKT_BURST
#define TIMER_HZ 200000
#define WAIT_TIMEOUT 5
#define MAX_RX_QUEUES 128
#define MAX_IRQ_COUNT (MAX_RX_QUEUES + 1)

#define APIC_LVTT   0x320
#define APIC_TMICT  0x380
#define APIC_TDCR   0x3E0
#define APIC_LVTT_MASK_BIT (1U << 16)
#define UINTR_UPID_PUIR_SET_BIT 16

#define MAX_POP_BATCH 16
#define MAX_LOAD_BALANCING_TRIES 4
#define SHARE_THRESHOLD 16
#define WORKER_STATE_LIGHT 0
#define WORKER_STATE_BUSY 1
#define WORKER_STATE_OVERLOADED 2
#define LIGHT_TO_BUSY_THRESHOLD 32
#define BUSY_TO_OVERLOADED_THRESHOLD 48
#define OVERLOADED_TO_BUSY_THRESHOLD 32
#define BUSY_TO_LIGHT_THRESHOLD 16

#define RX_BURST_HISTOGRAM_SIZE (SHARED_BUFFER_SIZE + 1)

/*********************************
* Structs
**********************************/

struct uintr_upid_user {
	struct {
		uint8_t status;
		uint8_t reserved1;
		uint8_t nv;
		uint8_t reserved2;
		uint32_t ndst;
	} __attribute__((packed)) nc;
	uint64_t puir;
} __attribute__((aligned(64)));

enum app_type {
	APP_TYPE_SYNTHETIC = 0,
	APP_TYPE_MEMCACHED,
	APP_TYPE_ROCKSDB,
	APP_TYPE_COUNT
};

struct packet_analysis_result {
	uint32_t src_ip;
	uint32_t dst_ip;
	uint16_t l4_src_port;
	uint16_t l4_dst_port;
	uint8_t l4_proto;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct rte_tcp_hdr *tcp_hdr;
	const uint8_t *app_payload;
	uint16_t app_payload_len;
	
	uint16_t padding;
	uint16_t processing_time;
	uint32_t sequence_number;
	uint64_t timestamp_send;
	uint64_t analysis_start_time;
	uint64_t analysis_end_time;
};

struct response_params {
	uint32_t src_ip;
	uint32_t dst_ip;
	uint16_t l4_src_port;
	uint16_t l4_dst_port;
	uint8_t l4_proto;

	uint16_t processing_time;
	uint32_t sequence_number;
	uint64_t timestamp_send;
	uint64_t rx_burst_time;
	uint64_t analysis_start_time;
	uint64_t analysis_end_time;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	const struct memcached_response *memcached_response;
	const struct rocksdb_response *rocksdb_response;
};

struct packet_info {
	struct rte_mbuf *pkt;
	uint64_t rx_burst_time;
	uint32_t processed_time_us;
	void *app_ctx;
};

struct active_packet_buffer {
	struct packet_info packets[SHARED_BUFFER_SIZE];
	volatile uint32_t head;
	volatile uint32_t tail;
	volatile uint32_t count;
	rte_spinlock_t lock;
} __rte_cache_aligned;

struct deferred_packet_buffer {
	struct packet_info packets[SHARED_BUFFER_SIZE];
	uint32_t head;
	uint32_t tail;
	uint32_t count;
} __rte_cache_aligned;

struct worker_shared_state {
	uint8_t load_state;
	uint8_t published_valid;
	uint8_t published_l4_proto;
	uint16_t published_l4_src_port;
	uint32_t published_src_ip_host;
	rte_spinlock_t published_lock;
} __rte_cache_aligned;

struct worker_context {
	uint16_t port_id;
	uint16_t queue_id;
	volatile uint32_t pending;
	uint32_t irq;
	unsigned int last_stolen_from;
	volatile bool app_running;
	volatile bool packet_preempted;

	volatile uint32_t *lapic;
	int lapic_fd;
	bool timer_registered;
	uint8_t timer_vector;

	int upid_fd;
	int upid_idx;
	void *upid_page;
	struct uintr_upid_user *upid;

	uint8_t busy_streak;
	uint8_t light_streak;
	uint8_t overloaded_streak;
	bool publish_next_overloaded_flow;
	uint32_t lapic_tmict_value;
	uint32_t lapic_lvtt_unmask_value;
	uint32_t lapic_lvtt_mask_value;
};

/*********************************
* Global variables
**********************************/

extern volatile bool global_quit_signal;
extern volatile pid_t global_lcore_tid_map[RTE_MAX_LCORE];
extern uint64_t global_tsc_hz;
extern uint16_t global_queue_num;
extern uint32_t global_enabled_port_mask;
extern uint16_t global_first_enabled_lcore;
extern uint16_t global_nic_port_id;
extern enum app_type global_app_type;
extern bool global_enable_load_balance;
extern bool global_enable_timer;
extern bool global_enable_colocation;

extern struct rte_mempool *pktmbuf_pool;

extern uint32_t queue_irq_map[MAX_RX_QUEUES];
extern uint16_t lcore_queue_map[RTE_MAX_LCORE];

extern __thread struct worker_context *active_worker_ctx;

extern struct active_packet_buffer active_buffers[RTE_MAX_LCORE];
extern struct deferred_packet_buffer deferred_buffers[RTE_MAX_LCORE];
extern struct worker_shared_state worker_shared_states[RTE_MAX_LCORE];
extern uint32_t worker_rx_burst_histogram[RTE_MAX_LCORE][RX_BURST_HISTOGRAM_SIZE];

/*********************************
* Functions
**********************************/

/* args.c */
int parse_args(int argc, char **argv);

/* flow_migration.c */
void init_migration_info(void);
int try_migrate_overloaded_flow(unsigned victim_lcore, unsigned self_lcore);
void publish_overloaded_flow_candidate(unsigned lcore_id, uint32_t src_ip_host,
	uint16_t src_port, uint8_t l4_proto);
void print_migration_summary(void);

/* irq_config.c */
int collect_irq_info(uint16_t portid, const struct rte_eth_dev_info *dev_info);
int set_irq_affinity(uint32_t irq, unsigned int lcore_id);

/* network/rx.c */
bool analyze_packet_content(struct rte_mbuf *m, struct packet_analysis_result *result);

/* network/tx.c */
void send_response_packet(const struct response_params *p);

/* shared_buffer.c */
void init_shared_buffers(void);
void active_buffer_init(struct active_packet_buffer *buf);
void deferred_buffer_init(struct deferred_packet_buffer *buf);
uint32_t active_buffer_add_packets(unsigned lcore_id,
	struct rte_mbuf **pkts, uint32_t nb_pkts, uint64_t rx_burst_time);
int deferred_buffer_enqueue_packet(unsigned lcore_id, struct rte_mbuf *pkt,
	uint64_t rx_burst_time, uint32_t processed_time_us, void *app_ctx);
uint32_t active_buffer_get_packets(unsigned lcore_id,
	struct packet_info *out, uint32_t max_n);
uint32_t active_buffer_steal_half(unsigned target_lcore,
	struct packet_info *stolen_packets, uint32_t max_steal);
bool active_buffer_has_packets(unsigned lcore_id);
uint32_t deferred_buffer_count(unsigned lcore_id);
uint32_t deferred_buffer_move_to_active(unsigned lcore_id,
	uint32_t max_move);

/* state.c */
void print_worker_shared_states(void);
void print_worker_rx_burst_histograms(void);
void update_worker_load_state(unsigned lcore_id, uint16_t nb_rx);
void print_port_stats_summary(void);

/* syscalls.c */
bool register_user_interrupt(struct worker_context *ctx);
int register_queue_task(uint32_t irq);
int unregister_queue_task(uint32_t irq);
int reset_upid_page(void);
int free_upid_page(void);
int uintr_wait(int time);
int uintr_kernel_handler_enable(void);
int uintr_kernel_handler_disable(void);
int register_lapic_ctl(void);
int unregister_lapic_ctl(void);
int register_timer_interrupt(uint32_t hz, uint8_t vector);
int unregister_timer_interrupt(void);

/* uintr.c */
void user_intr_disable(void);
void user_intr_enable(void);
void user_intr_prime(void);
void multilane_uintr_handler(struct __uintr_frame *frame, uint64_t vector)
	__attribute__((interrupt, target("general-regs-only")));

/* worker.c */
uint64_t tsc_now(void);
void process_received_packets(void);
void worker_run(void);

#endif /* COMMON_H */
