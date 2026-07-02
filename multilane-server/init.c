#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <dirent.h>
#include <limits.h>
#include <sys/syscall.h>
#include <x86gprintrin.h>
#include <uintrintrin.h>
#include <sched.h>
#include <time.h>

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

#include "common.h"
#include "syscalls.h"
#include "applications/application.h"

/* Global variables */
volatile bool global_quit_signal = false;
volatile pid_t global_lcore_tid_map[RTE_MAX_LCORE];
uint64_t global_tsc_hz = 0;
uint16_t global_queue_num = 1;
uint32_t global_enabled_port_mask = 0;
uint16_t global_first_enabled_lcore = INVALID_QUEUE_ID;
uint16_t global_nic_port_id = RTE_MAX_ETHPORTS;
uint16_t global_nic_port_socket_id = RTE_MAX_NUMA_NODES;
enum app_type global_app_type = APP_TYPE_SYNTHETIC;
bool global_enable_load_balance = false;
bool global_enable_timer = false;
bool global_enable_colocation = false;

/* maps */
uint32_t queue_irq_map[MAX_RX_QUEUES];
uint16_t lcore_queue_map[RTE_MAX_LCORE];
uint16_t queue_lcore_map[MAX_RX_QUEUES];

/* queue configuration */
uint16_t nb_rxd = 2048;
uint16_t nb_txd = 2048;
#define MEMPOOL_CACHE_SIZE 256
#define MIN_MBUF_POOL_SIZE 160000U
struct rte_mempool *pktmbuf_pool;
// Random and fixed RSS key
static uint8_t rss_key[52] = {
	0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
	0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
	0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
	0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
	0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
	0x5d, 0x51, 0x3e, 0x7b, 0x61, 0xca, 0x4c, 0xdd,
	0x3e, 0x7a, 0x6e, 0x89
};

static int launch_one_lcore(__rte_unused void *dummy)
{
	unsigned int lcore_id = rte_lcore_id();
	long tid = syscall(SYS_gettid);

	RTE_LOG(INFO, ML, "Core %u: starting initialization\n", lcore_id);

	if (lcore_id < RTE_MAX_LCORE)
		global_lcore_tid_map[lcore_id] = (pid_t)tid;
	worker_run();

	return 0;
}

static void external_quit_signal_handler(int signum)
{
	if (signum != SIGINT && signum != SIGTERM)
		return;

	RTE_LOG(INFO, ML, "Signal %d received, preparing to exit...\n\n\n", signum);

	global_quit_signal = true;

	if (!global_enable_colocation)
		return;

	pid_t pid = getpid();
	long tid = syscall(SYS_gettid);

	for (unsigned int i = 0; i < RTE_MAX_LCORE; i++) {
		pid_t target_tid = global_lcore_tid_map[i];

		if (target_tid <= 0 || target_tid == (pid_t)tid)
			continue;
		syscall(SYS_tgkill, pid, target_tid, ML_WORKER_QUIT_SIGNAL);
	}
}

static void worker_quit_signal_handler(int signum)
{
	(void)signum;
}

static void init_signal_handlers(void)
{
	global_quit_signal = false;
	memset((void *)global_lcore_tid_map, 0, sizeof(global_lcore_tid_map));
	signal(SIGINT, external_quit_signal_handler);
	signal(SIGTERM, external_quit_signal_handler);
	signal(ML_WORKER_QUIT_SIGNAL, worker_quit_signal_handler);
}

static void get_runtime_info(void)
{
	uint16_t nb_ports;
	uint16_t portid;
	int ret;
	struct rte_ether_addr mac_addr;

	unsigned int total_lcores = rte_lcore_count();
	if (total_lcores == 0)
		rte_exit(EXIT_FAILURE, "No available lcores found (total_lcores=0)\n");

	global_queue_num = (total_lcores > MAX_RX_QUEUES) ? MAX_RX_QUEUES : total_lcores;
	if (global_queue_num > 1) {
		RTE_LOG(INFO, ML, "Using %u queues with RSS\n", global_queue_num);
	} else {
		RTE_LOG(INFO, ML, "Using only one queue\n");
	}

	global_tsc_hz = rte_get_tsc_hz();
	RTE_LOG(INFO, ML, "TSC frequency: %u Hz\n", (unsigned int)global_tsc_hz);

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports found\n");

	if (global_enabled_port_mask == 0)
		global_enabled_port_mask = (1u << nb_ports) - 1;

	if (global_enabled_port_mask & ~((1u << nb_ports) - 1))
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
			(1u << nb_ports) - 1);

	// Single-port mode: select the first enabled available port only
	RTE_ETH_FOREACH_DEV(portid) {
		if ((global_enabled_port_mask & (1u << portid)) == 0)
			continue;
		global_nic_port_id = portid;
		break;
	}
	if (global_nic_port_id == RTE_MAX_ETHPORTS)
		rte_exit(EXIT_FAILURE, "All available ports are disabled. Please set portmask.\n");

	ret = rte_eth_macaddr_get(global_nic_port_id, &mac_addr);
	if (ret != 0)
		rte_exit(EXIT_FAILURE, "Failed to get MAC address for port %u: %s\n",
			global_nic_port_id, rte_strerror(-ret));
	RTE_LOG(INFO, ML, "Port %u MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
		global_nic_port_id,
		(unsigned)mac_addr.addr_bytes[0], (unsigned)mac_addr.addr_bytes[1],
		(unsigned)mac_addr.addr_bytes[2], (unsigned)mac_addr.addr_bytes[3],
		(unsigned)mac_addr.addr_bytes[4], (unsigned)mac_addr.addr_bytes[5]);

	global_nic_port_socket_id = rte_eth_dev_socket_id(global_nic_port_id);
	RTE_LOG(INFO, ML, "Port %u is on NUMA socket %u\n", global_nic_port_id, global_nic_port_socket_id);
}

static void init_runtime_resources(void)
{
	unsigned int nb_mbufs;
	unsigned int nb_rx_queues = global_queue_num;
	unsigned int nb_tx_queues = global_queue_num;
	uint16_t worker_count = 0;

	nb_mbufs = (unsigned int)(nb_rx_queues * nb_rxd +
			nb_tx_queues * nb_txd +
			nb_rx_queues * MAX_PKT_BURST +
			(nb_rx_queues + nb_tx_queues) * MEMPOOL_CACHE_SIZE);
	nb_mbufs = RTE_MAX(nb_mbufs, MIN_MBUF_POOL_SIZE);
	
	pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		global_nic_port_socket_id);
	if (pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
	RTE_LOG(INFO, ML, "Creating mbuf pool with %u mbufs\n", nb_mbufs);

	for (unsigned i = 0; i < RTE_MAX_LCORE; i++)
		lcore_queue_map[i] = INVALID_QUEUE_ID;
	for (unsigned i = 0; i < MAX_RX_QUEUES; i++)
		queue_lcore_map[i] = INVALID_QUEUE_ID;

    unsigned int lc;
    RTE_LCORE_FOREACH(lc) {
        if (worker_count >= global_queue_num)
            break;
		if (global_first_enabled_lcore == INVALID_QUEUE_ID)
			global_first_enabled_lcore = (uint16_t)lc;
        queue_lcore_map[worker_count] = lc;

		if (lc != rte_lcore_to_cpu_id(lc)) {
			rte_exit(EXIT_FAILURE, "Lcore ID (%u) does not match rte_lcore_to_cpu_id (%d)\n",
				lc, rte_lcore_to_cpu_id(lc));
		}
		lcore_queue_map[lc] = worker_count;
        worker_count++;
    }
    if (worker_count != global_queue_num)
        rte_exit(EXIT_FAILURE,
            "Not enough enabled workers for queue affinity: required=%u, found=%u\n",
            global_queue_num, worker_count);
}

static void configure_irq_affinity(uint16_t port_id,
	const struct rte_eth_dev_info *dev_info)
{
	int irq_ret = collect_irq_info(port_id, dev_info);
	if (irq_ret < 0)
		rte_exit(EXIT_FAILURE,
			"Failed to auto-configure user interrupt IRQ (err=%d)\n", irq_ret);

	for (uint16_t q = 0; q < global_queue_num; q++) {
		uint32_t irq = queue_irq_map[q];
		unsigned int lcore_id = queue_lcore_map[q];
		set_irq_affinity(irq, lcore_id);
	}
}

static void configure_port(void)
{
	int ret;
	struct rte_eth_dev_info dev_info;

	RTE_LOG(INFO, ML, "Configuring port %u\n", global_nic_port_id);

	struct rte_eth_conf local_port_conf = {
        .rxmode = {
            .mq_mode = (global_queue_num > 1) ? RTE_ETH_MQ_RX_RSS : RTE_ETH_MQ_RX_NONE,
        },
        .rx_adv_conf = {
			.rss_conf = {
				.rss_key = rss_key,
				.rss_key_len = 52,  
				.rss_hf = RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV4_UDP,
			},
		},
		.intr_conf = {
			.rxq = 1,
		},
	};
	
    ret = rte_eth_dev_info_get(global_nic_port_id, &dev_info);
    if (ret != 0)
        rte_exit(EXIT_FAILURE,
            "Error getting port info: %s\n", strerror(-ret));
    if (dev_info.max_rx_queues < global_queue_num || dev_info.max_tx_queues < global_queue_num)
        rte_exit(EXIT_FAILURE,
            "Port queue capacity not enough: RX required/max=%u/%u, TX required/max=%u/%u\n",
            global_queue_num, dev_info.max_rx_queues, global_queue_num, dev_info.max_tx_queues);
	
    ret = rte_eth_dev_configure(global_nic_port_id, global_queue_num, global_queue_num, &local_port_conf);
	if (ret < 0)
		rte_exit(EXIT_FAILURE,
			"Cannot configure device: err=%d\n", ret);
	
	ret = rte_eth_dev_adjust_nb_rx_tx_desc(global_nic_port_id, &nb_rxd, &nb_txd);
	if (ret < 0)
		rte_exit(EXIT_FAILURE,
			"Cannot adjust number of descriptors: err=%d\n", ret);
	RTE_LOG(INFO, ML, "Port descriptor number: RX=%u TX=%u\n", nb_rxd, nb_txd);

	struct rte_eth_rxconf rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = local_port_conf.rxmode.offloads;
    for (uint16_t q = 0; q < global_queue_num; q++) {
        ret = rte_eth_rx_queue_setup(global_nic_port_id, q, nb_rxd,
                             global_nic_port_socket_id, &rxq_conf, pktmbuf_pool);
        if (ret < 0)
            rte_exit(EXIT_FAILURE,
                "rte_eth_rx_queue_setup: err=%d, queue=%u\n", ret, q);
    }

    struct rte_eth_txconf txq_conf = dev_info.default_txconf;
    txq_conf.offloads = local_port_conf.txmode.offloads;
    for (uint16_t q = 0; q < global_queue_num; q++) {
        ret = rte_eth_tx_queue_setup(global_nic_port_id, q, nb_txd,
                             global_nic_port_socket_id, &txq_conf);
        if (ret < 0)
            rte_exit(EXIT_FAILURE,
                "rte_eth_tx_queue_setup: err=%d, queue=%u\n", ret, q);
    }

	ret = rte_eth_promiscuous_enable(global_nic_port_id);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			"rte_eth_promiscuous_enable: err=%s\n", rte_strerror(-ret));

	ret = rte_eth_dev_start(global_nic_port_id);
	if (ret < 0)
		rte_exit(EXIT_FAILURE,
			"rte_eth_dev_start: err=%d\n", ret);

    RTE_LOG(INFO, ML, "Port initialized (RX=%u, TX=%u)\n", global_queue_num, global_queue_num);

	configure_irq_affinity(global_nic_port_id, &dev_info);
}

static void cleanup_runtime(void)
{
	int ret;

	RTE_LOG(INFO, ML, "Closing port %u\n", global_nic_port_id);

	ret = rte_eth_dev_stop(global_nic_port_id);
	if (ret != 0)
		rte_exit(EXIT_FAILURE, "rte_eth_dev_stop: err=%d\n", ret);

	rte_eth_dev_close(global_nic_port_id);

	rte_eal_cleanup();

	RTE_LOG(INFO, ML, "Runtime resources cleaned up\n");
}

void init_multilane(void) {
	init_shared_buffers();
	init_migration_info();

	reset_upid_page();
	uintr_kernel_handler_enable();
}

void exit_multilane(void) {
	free_upid_page();
	uintr_kernel_handler_disable();
}


void print_runtime_info_on_exit(void) {
	print_worker_rx_burst_histograms();

	print_worker_shared_states();

	print_migration_summary();

	print_port_stats_summary();

	printf("\n");
}

int main(int argc, char *argv[])
{
	int ret;
	unsigned int lcore_id;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Failed to parse arguments\n");

	init_signal_handlers();

	get_runtime_info();

	init_runtime_resources();

	init_multilane();

	configure_port();

	app_init_application();

	RTE_LOG(INFO, ML, "Starting multilane server... Press Ctrl+C to stop\n\n");

	rte_eal_mp_remote_launch(launch_one_lcore, NULL, CALL_MAIN);

	ret = 0;
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	exit_multilane();

	print_runtime_info_on_exit();

	cleanup_runtime();

	return ret;
}
