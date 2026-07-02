#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <time.h>
#include <sys/time.h>

#include "common.h"
#include "applications/application.h"

/* Global variables */
volatile bool global_quit_signal = false;
uint64_t global_tsc_hz = 0;
uint32_t global_enabled_port_mask = 0;
uint16_t global_nic_port_id = RTE_MAX_ETHPORTS;
uint16_t global_nic_port_socket_id = RTE_MAX_NUMA_NODES;
char global_unified_timestamp[32] = {0};
uint32_t global_max_pkt_count;
uint32_t global_target_rps;
enum load_distribution global_load_dist;
uint32_t global_tx_lcores[MAX_WORKER_LCORES];
uint32_t global_rx_lcores[MAX_WORKER_LCORES];
uint32_t global_nb_tx_lcores;
uint32_t global_nb_rx_lcores;
enum app_type global_app_type;
bool global_app_get_ratio_init;
double global_app_get_ratio;

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

static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		RTE_LOG(INFO, ML, "Signal %d received, preparing to exit...\n\n\n", signum);
		global_quit_signal = true;
	}
}

static void init_signal_handlers(void)
{
	global_quit_signal = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
}

static void check_lcores_args(void)
{
	/* Validate TX and RX lcore configuration */
	if (global_nb_tx_lcores == 0 || global_nb_rx_lcores == 0)
		rte_exit(EXIT_FAILURE,
			"Must specify both -t (TX cores) and -r (RX cores)\n");

	/* Verify that lcore 0 is not used for TX or RX */
	for (uint32_t i = 0; i < global_nb_tx_lcores; i++) {
		if (global_tx_lcores[i] == 0)
			rte_exit(EXIT_FAILURE,
				"Lcore 0 must not be used for TX workers\n");
	}
	for (uint32_t i = 0; i < global_nb_rx_lcores; i++) {
		if (global_rx_lcores[i] == 0)
			rte_exit(EXIT_FAILURE,
				"Lcore 0 must not be used for RX workers\n");
	}

	/* Check for overlap between TX and RX cores */
	for (uint32_t i = 0; i < global_nb_tx_lcores; i++) {
		for (uint32_t j = 0; j < global_nb_rx_lcores; j++) {
			if (global_tx_lcores[i] == global_rx_lcores[j])
				rte_exit(EXIT_FAILURE,
					"TX and RX lcore ranges must not overlap (lcore %u)\n",
					global_tx_lcores[i]);
		}
	}
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

static void init_runtime_mempool(void)
{
	unsigned int nb_mbufs;
	unsigned int nb_rx_queues = global_nb_rx_lcores;
	unsigned int nb_tx_queues = global_nb_tx_lcores;

	nb_mbufs = (unsigned int)(nb_rx_queues * nb_rxd +
			nb_tx_queues * nb_txd +
			nb_rx_queues * BURST_SIZE +
			(nb_rx_queues + nb_tx_queues) * MEMPOOL_CACHE_SIZE);
	nb_mbufs = RTE_MAX(nb_mbufs, MIN_MBUF_POOL_SIZE);

	pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		global_nic_port_socket_id);
	if (pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	RTE_LOG(INFO, ML, "Creating mbuf pool with %u mbufs\n", nb_mbufs);
}

static void configure_port(void)
{
	int ret;
	struct rte_eth_dev_info dev_info;

	RTE_LOG(INFO, ML, "Configuring port %u\n", global_nic_port_id);

	struct rte_eth_conf local_port_conf = {
        .rxmode = {
            .mq_mode = (global_nb_rx_lcores > 1) ? RTE_ETH_MQ_RX_RSS : RTE_ETH_MQ_RX_NONE,
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
    if (dev_info.max_rx_queues < global_nb_rx_lcores || dev_info.max_tx_queues < global_nb_tx_lcores)
        rte_exit(EXIT_FAILURE,
            "Port queue capacity not enough: RX required/max=%u/%u, TX required/max=%u/%u\n",
            global_nb_rx_lcores, dev_info.max_rx_queues, global_nb_tx_lcores, dev_info.max_tx_queues);

    ret = rte_eth_dev_configure(global_nic_port_id, global_nb_rx_lcores, global_nb_tx_lcores, &local_port_conf);
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
    for (uint16_t q = 0; q < global_nb_rx_lcores; q++) {
        ret = rte_eth_rx_queue_setup(global_nic_port_id, q, nb_rxd,
                             global_nic_port_socket_id, &rxq_conf, pktmbuf_pool);
        if (ret < 0)
            rte_exit(EXIT_FAILURE,
                "rte_eth_rx_queue_setup: err=%d, queue=%u\n", ret, q);
    }

    struct rte_eth_txconf txq_conf = dev_info.default_txconf;
    txq_conf.offloads = local_port_conf.txmode.offloads;
    for (uint16_t q = 0; q < global_nb_tx_lcores; q++) {
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

    RTE_LOG(INFO, ML, "Port initialized (RX=%u, TX=%u)\n", global_nb_rx_lcores, global_nb_tx_lcores);
}


static void create_unified_timestamp(void)
{
	time_t unified_time = time(NULL);
	struct tm *tm_info = localtime(&unified_time);
	snprintf(global_unified_timestamp, sizeof(global_unified_timestamp),
		"%04d%02d%02d_%02d%02d%02d",
		tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
		tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

static int launch_one_lcore(__rte_unused void *dummy)
{
	worker_run();

	return 0;
}

static const char *app_type_label(enum app_type t)
{
	switch (t) {
	case APP_TYPE_SYNTHETIC:
		return "synthetic";
	case APP_TYPE_MEMCACHED:
		return "memcached";
	case APP_TYPE_ROCKSDB:
		return "rocksdb";
	default:
		return "unknown";
	}
}

static void init_multilane_client(void)
{
	RTE_LOG(INFO, ML, "Will send %u packets total, target total RPS: %u\n", global_max_pkt_count, global_target_rps);
	RTE_LOG(INFO, ML, "TX: %u workers, RX: %u workers\n", global_nb_tx_lcores, global_nb_rx_lcores);
	RTE_LOG(INFO, ML, "Application type (-A): %s\n", app_type_label(global_app_type));
	RTE_LOG(INFO, ML, "Load distribution: %s\n", load_generator_get_name());

	srand(rte_get_timer_cycles());

	create_unified_timestamp();
}

void print_port_stats_summary(void) {
	struct rte_eth_stats hw_stats;
	if (rte_eth_stats_get(global_nic_port_id, &hw_stats) == 0) {
		printf("\nPort stats before exit:\n");
		printf("  HW ipackets: %" PRIu64 "\n", hw_stats.ipackets);
		printf("  HW ibytes: %" PRIu64 "\n", hw_stats.ibytes);
		printf("  HW imissed: %" PRIu64 "\n", hw_stats.imissed);
		printf("  HW ierrors: %" PRIu64 "\n", hw_stats.ierrors);
		printf("  HW rx_nombuf: %" PRIu64 "\n", hw_stats.rx_nombuf);
		printf("  HW opackets: %" PRIu64 "\n", hw_stats.opackets);
		printf("  HW obytes: %" PRIu64 "\n", hw_stats.obytes);
		printf("  HW oerrors: %" PRIu64 "\n", hw_stats.oerrors);
	}

	printf("\n");
}

static void merge_rx_worker_outputs(void)
{
	char result_filename[256];
	snprintf(result_filename, sizeof(result_filename), "result_%s.ml", global_unified_timestamp);
	RTE_LOG(INFO, ML, "Merging RX worker output files into %s...\n", result_filename);

	FILE *result_file = fopen(result_filename, "w");
	if (result_file == NULL) {
		RTE_LOG(WARNING, ML, "Cannot create result file %s\n", result_filename);
		return;
	}

	/* Merge files in queue order */
	for (uint32_t q = 0; q < global_nb_rx_lcores; q++) {
		uint32_t lcore_id = global_rx_lcores[q];
		char worker_filename[256];

		snprintf(worker_filename, sizeof(worker_filename),
			"worker%u_queue%u_%s.ml", lcore_id, q, global_unified_timestamp);

		FILE *worker_file = fopen(worker_filename, "r");
		if (worker_file != NULL) {
			char buffer[8192];
			size_t bytes_read;
			while ((bytes_read = fread(buffer, 1, sizeof(buffer), worker_file)) > 0) {
				fwrite(buffer, 1, bytes_read, result_file);
			}
			fclose(worker_file);
			// Remove individual worker file after merging
			remove(worker_filename);
		} else {
			RTE_LOG(WARNING, ML, "Cannot open worker file %s for merging\n", worker_filename);
		}
	}

	fclose(result_file);
	RTE_LOG(INFO, ML, "Successfully merged all RX worker files into %s\n", result_filename);
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

int main(int argc, char *argv[])
{
	int ret;
	unsigned int lcore_id;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	init_default_args();
	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Failed to parse arguments\n");

	check_lcores_args();

	load_generator_init(global_load_dist);

	init_signal_handlers();

	get_runtime_info();

	init_runtime_mempool();

	configure_port();

	init_multilane_client();

	application_parse_init();
	
	/* Main lcore waits for selected workers to finish */
	RTE_LOG(INFO, ML, "Main lcore waiting for all workers to complete...\n");
	RTE_LOG(INFO, ML, "Starting multilane client... Press Ctrl+C to stop\n\n");
	
	rte_eal_mp_remote_launch(launch_one_lcore, NULL, SKIP_MAIN);

	ret = 0;
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	print_port_stats_summary();

	merge_rx_worker_outputs();

	cleanup_runtime();

	return ret;
}
