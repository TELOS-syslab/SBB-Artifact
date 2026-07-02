#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "common.h"

static const char short_options[] = "p:A:LTC";

static void multilane_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-A APP] [-L] [-T] [-C]\n"
		"  -p <PORTMASK>     Hexadecimal bitmask of ports to configure\n"
		"  -A <APP>          Application type [synthetic|memcached|rocksdb] (default: synthetic)\n"
		"  -L                Enable load balance\n"
		"  -T                Enable timer\n"
		"  -C                Enable co-location\n",
		prgname);
}

static int parse_portmask(const char *portmask)
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

int parse_args(int argc, char **argv)
{
	int opt;
	int option_index;
	char **argvopt = argv;
	char *prgname = argv[0];

	while ((opt = getopt_long(argc, argvopt, short_options,
				  NULL, &option_index)) != EOF) {
		switch (opt) {
		case 'p':
			global_enabled_port_mask = parse_portmask(optarg);
			if (global_enabled_port_mask == 0) {
				printf("Error: invalid portmask\n");
				multilane_usage(prgname);
				return -1;
			}
			break;

		case 'A': 
			enum app_type app_type;
			if (parse_app_type(optarg, &app_type) != 0) {
				printf("Error: invalid application type: %s\n", optarg);
				multilane_usage(prgname);
				return -1;
			}
			global_app_type = app_type;
			break;
		
		case 'L':
			global_enable_load_balance = true;
			break;

		case 'T':
			global_enable_timer = true;
			break;

		case 'C':
			global_enable_colocation = true;
			break;

		case 'h':
			multilane_usage(prgname);
			return -1;
			
		default:
			multilane_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind - 1] = prgname;

	return optind - 1;
}
