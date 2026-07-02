#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include "common.h"

static const char short_options[] =
	"c:"  /* count */
	"d:"  /* distribution for synthetic */
	"l:"  /* load (target RPS) */
	"p:"  /* portmask */
	"A:"  /* application type (match server -A) */
	"t:"  /* tx-cores */
	"r:"  /* rx-cores */
	"R:"  /* GET ratio for memcached/rocksdb */
	"h"   /* help */
	;

static void multilane_usage(const char *prgname)
{
	printf("%s [EAL options] -- [options]\n", prgname);
	printf("  -c, --count <num>         Number of packets to send\n");
	printf("  -d, --distribution <type> Load distribution (only with -A synthetic):\n");
	printf("                            fixed_1          - Fixed(1us)\n");
	printf("                            fixed_10         - Fixed(10us)\n");
	printf("                            exponential_10   - Exponential(mean=10us)\n");
	printf("                            high_bimodal     - Bimodal(50%%:1us, 50%%:100us)\n");
	printf("                            extreme_bimodal  - Bimodal(99.5%%:0.5us, 0.5%%:500us)\n");
	printf("                            rocksdb_ht       - Bimodal(50%%:1.25us, 50%%:613us)\n");
	printf("                            rocksdb_lt       - Fixed(1.25us)\n");
	printf("                            tpcc        - TPC-C mix(44%%:5.7us, 4%%:6us, 44%%:20us, 4%%:88us, 4%%:100us)\n");
	printf("                            (default: fixed_1)\n");
	printf("  -l, --load <rps>          Target total RPS for all TX workers (e.g., 1000000)\n");
	printf("  -p, --port <PORTMASK>     Hexadecimal bitmask of port to use (e.g., 0x1)\n");
	printf("  -A <APP>                  Application type [synthetic|memcached|rocksdb] (default: synthetic)\n");
	printf("  -t, --tx-cores <range>    TX worker lcore range (e.g., 1-16 or 1,2,3)\n");
	printf("  -r, --rx-cores <range>    RX worker lcore range (e.g., 17-32 or 17,18,19)\n");
	printf("  -R <ratio>                GET fraction in [0,1] for two-request modes (only with\n");
	printf("                            -A memcached or -A rocksdb). Default: 0.8. \n");
	printf("  -h, --help                Show this help message\n");
}

void init_default_args(void)
{
	global_max_pkt_count = 0;
	global_load_dist = DIST_FIXED_1;
	global_nb_tx_lcores = 0;
	global_nb_rx_lcores = 0;
	global_target_rps = 0;
	memset(global_tx_lcores, 0, sizeof(global_tx_lcores));
	memset(global_rx_lcores, 0, sizeof(global_rx_lcores));
	global_app_type = APP_TYPE_SYNTHETIC;
	global_app_get_ratio_init = false;
	global_app_get_ratio = 0.8;
}

int parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm = strtoul(portmask, &end, 16);

	if (portmask[0] == '\0' || end == NULL || *end != '\0')
		return 0;

	return pm;
}

static int parse_app_type(const char *arg, enum app_type *type)
{
	if (strcmp(arg, "synthetic") == 0) {
		*type = APP_TYPE_SYNTHETIC;
		return 0;
	}
	if (strcmp(arg, "memcached") == 0) {
		*type = APP_TYPE_MEMCACHED;
		return 0;
	}
	if (strcmp(arg, "rocksdb") == 0) {
		*type = APP_TYPE_ROCKSDB;
		return 0;
	}
	return -1;
}

int parse_lcore_range(const char *lcore_str, uint32_t *lcore_array, uint32_t *nb_lcores)
{
	char *str_copy = strdup(lcore_str);
	char *token;
	char *saveptr;
	uint32_t count = 0;
	uint32_t seen[MAX_LCORES] = {0};

	if (str_copy == NULL)
		return -1;

	/* Parse comma-separated ranges */
	token = strtok_r(str_copy, ",", &saveptr);
	while (token != NULL) {
		char *dash = strchr(token, '-');
		if (dash != NULL) {
			/* Range format: "1-16" */
			*dash = '\0';
			int start = atoi(token);
			int end = atoi(dash + 1);
			if (start < 0 || end < 0 || start > end || end >= MAX_LCORES) {
				free(str_copy);
				return -1;
			}
			for (int i = start; i <= end; i++) {
				if (count >= MAX_WORKER_LCORES) {
					free(str_copy);
					return -1;
				}
				if (!seen[i]) {
					seen[i] = 1;
					lcore_array[count++] = i;
				}
			}
		} else {
			/* Single lcore */
			int lcore = atoi(token);
			if (lcore < 0 || lcore >= MAX_LCORES) {
				free(str_copy);
				return -1;
			}
			if (count >= MAX_WORKER_LCORES) {
				free(str_copy);
				return -1;
			}
			if (!seen[lcore]) {
				seen[lcore] = 1;
				lcore_array[count++] = lcore;
			}
		}
		token = strtok_r(NULL, ",", &saveptr);
	}

	free(str_copy);

	if (count == 0)
		return -1;

	*nb_lcores = count;
	return 0;
}

int parse_get_ratio(const char *s, double *ratio)
{
	char *end = NULL;
	double v;

	if (ratio == NULL || s == NULL)
		return -1;

	errno = 0;
	v = strtod(s, &end);
	if (errno != 0 || end == NULL || end == s || *end != '\0')
		return -1;
	if (v < 0.0 || v > 1.0)
		return -1;
	*ratio = v;
	return 0;
}

int parse_args(int argc, char **argv)
{
	int opt;
	int option_index;
	char **argvopt = argv;
	char *prgname = argv[0];
	bool saw_distribution = false;

	while ((opt = getopt_long(argc, argvopt, short_options,
				NULL, &option_index)) != EOF) {
		switch (opt) {
		case 'c':
			global_max_pkt_count = atoi(optarg);
			if (global_max_pkt_count == 0) {
				printf("Error: packet count must be greater than 0\n");
				multilane_usage(prgname);
				return -1;
			}
			break;

		case 'd':
			saw_distribution = true;
			if (strcmp(optarg, "fixed_1") == 0) {
				global_load_dist = DIST_FIXED_1;
			} else if (strcmp(optarg, "fixed_10") == 0) {
				global_load_dist = DIST_FIXED_10;
			} else if (strcmp(optarg, "exponential_10") == 0) {
				global_load_dist = DIST_EXPONENTIAL_10;
			} else if (strcmp(optarg, "high_bimodal") == 0) {
				global_load_dist = DIST_HIGH_BIMODAL;
			} else if (strcmp(optarg, "extreme_bimodal") == 0) {
				global_load_dist = DIST_EXTREME_BIMODAL;
			} else if (strcmp(optarg, "rocksdb_ht") == 0) {
				global_load_dist = DIST_ROCKSDB_HT;
			} else if (strcmp(optarg, "rocksdb_lt") == 0) {
				global_load_dist = DIST_ROCKSDB_LT;
			} else if (strcmp(optarg, "tpcc") == 0) {
				global_load_dist = DIST_TPCC;
			} else {
				printf("Error: unknown distribution type '%s'\n", optarg);
				multilane_usage(prgname);
				return -1;
			}
			break;

		case 'l':
			global_target_rps = (uint32_t)atoi(optarg);
			if (global_target_rps == 0) {
				printf("Error: target RPS must be greater than 0\n");
				multilane_usage(prgname);
				return -1;
			}
			break;

		case 'p':
			global_enabled_port_mask = parse_portmask(optarg);
			break;

		case 'A': {
			enum app_type app_type;

			if (parse_app_type(optarg, &app_type) != 0) {
				printf("Error: invalid application type: %s\n", optarg);
				multilane_usage(prgname);
				return -1;
			}
			global_app_type = app_type;
			break;
		}

		case 't':
			if (parse_lcore_range(optarg, global_tx_lcores, &global_nb_tx_lcores) < 0) {
				printf("Error: invalid TX lcore range '%s'\n", optarg);
				multilane_usage(prgname);
				return -1;
			}
			break;

		case 'r':
			if (parse_lcore_range(optarg, global_rx_lcores, &global_nb_rx_lcores) < 0) {
				printf("Error: invalid RX lcore range '%s'\n", optarg);
				multilane_usage(prgname);
				return -1;
			}
			break;

		case 'R':
			if (parse_get_ratio(optarg, &global_app_get_ratio) != 0) {
				printf("Error: invalid GET ratio '%s' (expected a number in [0,1])\n", optarg);
				multilane_usage(prgname);
				return -1;
			}
			global_app_get_ratio_init = true;
			break;

		case 'h':
			multilane_usage(prgname);
			return -1;

		default:
			multilane_usage(prgname);
			return -1;
		}
	}

	if (saw_distribution && global_app_type != APP_TYPE_SYNTHETIC) {
		printf("Error: -d is only valid with -A synthetic\n");
		multilane_usage(prgname);
		return -1;
	}

	if (global_app_get_ratio_init && global_app_type != APP_TYPE_MEMCACHED &&
	    global_app_type != APP_TYPE_ROCKSDB) {
		printf("Error: -R (GET ratio) is only valid with -A memcached or -A rocksdb\n");
		multilane_usage(prgname);
		return -1;
	}

	if (optind >= 0)
		argv[optind - 1] = prgname;

	return optind - 1;
}
