#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>

#include "common.h"

/* Remove trailing whitespace/newline from text read from sysfs/procfs. */
static void trim_trailing(char *buf)
{
	if (buf == NULL)
		return;

	size_t len = strlen(buf);
	while (len > 0) {
		unsigned char c = buf[len - 1];
		if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
			buf[--len] = '\0';
		else
			break;
	}
}

static int prepare_irq_scan_context(uint16_t portid,
	const struct rte_eth_dev_info *dev_info,
	const char **bdf_out,
	char *path_out,
	size_t path_out_len,
	DIR **dir_out)
{
	const struct rte_device *dev = dev_info->device;
	const struct rte_bus *bus = rte_dev_bus(dev);
	const char *bus_name = bus ? rte_bus_name(bus) : NULL;

	if (dev == NULL || bus_name == NULL || strcmp(bus_name, "pci") != 0) {
		printf("No PCI device information available, cannot dump IRQ layout\n");
		return -ENODEV;
	}

	const char *bdf = rte_dev_name(dev);
	if (bdf == NULL) {
		printf("Missing BDF string, cannot dump IRQ layout\n");
		return -ENODEV;
	}

	int len = snprintf(path_out, path_out_len, "/sys/bus/pci/devices/%s/msi_irqs", bdf);
	if (len <= 0 || len >= (int)path_out_len) {
		printf("Sysfs path truncated, cannot dump IRQ layout\n");
		return -ENAMETOOLONG;
	}

	DIR *dir = opendir(path_out);
	if (dir == NULL) {
		printf("No msi_irqs directory (path=%s)\n", path_out);
		return -errno;
	}

	*bdf_out = bdf;
	*dir_out = dir;
	return 0;
}

static int collect_irq_entries(DIR *dir, unsigned long *irq_list, unsigned int *irq_count_out)
{
	struct dirent *ent;
	unsigned int irq_count = 0;

	while ((ent = readdir(dir)) != NULL) {
		if (!isdigit((unsigned char)ent->d_name[0]))
			continue;

		char *end = NULL;
		unsigned long irq = strtoul(ent->d_name, &end, 10);
		if (ent->d_name[0] == '\0' || end == NULL || *end != '\0')
			continue;

		if (irq_count > MAX_IRQ_COUNT) {
			printf("Too many IRQs found, limiting to %u\n", MAX_IRQ_COUNT);
			break;
		}

		char affinity_path[PATH_MAX];
		char affinity[128] = "unknown";
		int alen = snprintf(affinity_path, sizeof(affinity_path),
				"/proc/irq/%lu/smp_affinity_list", irq);
		if (alen > 0 && alen < (int)sizeof(affinity_path)) {
			FILE *fp = fopen(affinity_path, "r");
			if (fp != NULL) {
				if (fgets(affinity, sizeof(affinity), fp) != NULL)
					trim_trailing(affinity);
				fclose(fp);
			}
		}

		printf("  IRQ %lu affinity: %s\n", irq, affinity);
		irq_list[irq_count++] = irq;
	}

	*irq_count_out = irq_count;
	return (irq_count > 0) ? 0 : -ENOENT;
}

static int sort_and_map_irqs(const char *path, unsigned long *irq_list, unsigned int irq_count)
{
	for (unsigned int i = 0; i + 1 < irq_count; i++) {
		for (unsigned int j = 0; j + 1 < irq_count - i; j++) {
			if (irq_list[j] > irq_list[j + 1]) {
				unsigned long temp = irq_list[j];
				irq_list[j] = irq_list[j + 1];
				irq_list[j + 1] = temp;
			}
		}
	}

	if (irq_count > 0) {
		printf("Sorted IRQs: ");
		for (unsigned int i = 0; i < irq_count; i++)
			printf("%lu ", irq_list[i]);
		printf("\n");
	}

	// skip smallest because it is not for queue interrupt
	unsigned int start = (irq_count >= 2) ? 1 : 0;

	uint16_t queue_irq_count = 0;
	for (unsigned int q = 0; q < global_queue_num && (start + q) < irq_count &&
	     q < MAX_RX_QUEUES; q++) {
		queue_irq_map[q] = (uint32_t)irq_list[start + q];
		queue_irq_count++;
		printf("  -> queue %u mapped to IRQ %u\n", q, queue_irq_map[q]);
	}
	
	if (queue_irq_count != global_queue_num) {
		rte_exit(EXIT_FAILURE,
			"queue_irq_count (%u) != global_queue_num (%u)\n",
			queue_irq_count, global_queue_num);
	}

	return 0;
}

int collect_irq_info(uint16_t portid, const struct rte_eth_dev_info *dev_info)
{
	const char *bdf = NULL;
	char path[PATH_MAX];
	DIR *dir = NULL;

	int ret = prepare_irq_scan_context(portid, dev_info, &bdf, path, sizeof(path), &dir);
	if (ret < 0)
		return ret;
	printf("IRQ layout for port %u (PCI %s):\n", portid, bdf);

	unsigned long irq_list[MAX_IRQ_COUNT];
	unsigned int irq_count = 0;
	ret = collect_irq_entries(dir, irq_list, &irq_count);
	closedir(dir);
	if (ret < 0) {
		printf("  no MSI-X entries discovered under %s\n\n", path);
		return ret;
	}

	return sort_and_map_irqs(path, irq_list, irq_count);
}

int set_irq_affinity(uint32_t irq, unsigned int lcore_id)
{
	if (lcore_id < 0)
		return -EINVAL;

	char path[PATH_MAX];
	int len = snprintf(path, sizeof(path),
			 "/proc/irq/%u/smp_affinity_list", irq);
	if (len <= 0 || len >= (int)sizeof(path))
		return -ENOSPC;

	FILE *fp = fopen(path, "w");
	if (fp == NULL)
		return -errno;

	if (fprintf(fp, "%d\n", lcore_id) < 0) {
		int err = errno;
		fclose(fp);
		return -err;
	}

	if (fclose(fp) != 0)
		return -errno;

	printf("IRQ %u pinned to CPU %d\n", irq, lcore_id);
	return 0;
}