#include <inttypes.h>
#include <string.h>

#include "common.h"

uint64_t migration_dir_cnt[MAX_RX_QUEUES][MAX_RX_QUEUES];
uint64_t migration_total_success;
uint64_t migration_total_fail;
uint64_t migration_total_eexist;

void init_migration_info(void)
{
	memset(migration_dir_cnt, 0, sizeof(migration_dir_cnt));
	migration_total_success = 0;
	migration_total_fail = 0;
	migration_total_eexist = 0;
}

static int consume_published_flow(unsigned victim_lcore, uint32_t *src_ip_host,
	uint16_t *src_port, uint8_t *l4_proto)
{
	struct worker_shared_state *victim = &worker_shared_states[victim_lcore];

	if (!rte_spinlock_trylock(&victim->published_lock))
		return -EAGAIN;

	if (!victim->published_valid) {
		rte_spinlock_unlock(&victim->published_lock);
		return -ENOENT;
	}

	*src_ip_host = victim->published_src_ip_host;
	*src_port = victim->published_l4_src_port;
	*l4_proto = victim->published_l4_proto;
	victim->published_valid = 0;
	victim->published_l4_proto = 0;
	victim->published_src_ip_host = 0;
	victim->published_l4_src_port = 0;
	rte_spinlock_unlock(&victim->published_lock);

	return 0;
}

static int program_flow_migration_rule(uint32_t src_ip_host, uint16_t src_port,
	uint8_t l4_proto, uint16_t to_queue)
{
	struct rte_flow_attr attr = {0};
	struct rte_flow_item pattern[4];
	struct rte_flow_item_eth eth_spec = {0};
	struct rte_flow_item_eth eth_mask = {0};
	struct rte_flow_item_ipv4 ipv4_spec = {0};
	struct rte_flow_item_ipv4 ipv4_mask = {0};
	struct rte_flow_item_udp udp_spec = {0};
	struct rte_flow_item_udp udp_mask = {0};
	struct rte_flow_item_tcp tcp_spec = {0};
	struct rte_flow_item_tcp tcp_mask = {0};
	struct rte_flow_action action[2];
	struct rte_flow_action_queue queue = { .index = to_queue };
	struct rte_flow_error error;
	struct rte_flow *flow;

	if (global_nic_port_id == RTE_MAX_ETHPORTS)
		return -ENODEV;

	attr.ingress = 1;

	ipv4_spec.hdr.src_addr = rte_cpu_to_be_32(src_ip_host);
	ipv4_mask.hdr.src_addr = RTE_BE32(UINT32_MAX);
	ipv4_spec.hdr.next_proto_id = l4_proto;
	ipv4_mask.hdr.next_proto_id = UINT8_MAX;

	memset(pattern, 0, sizeof(pattern));
	pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
	pattern[0].spec = &eth_spec;
	pattern[0].mask = &eth_mask;
	pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
	pattern[1].spec = &ipv4_spec;
	pattern[1].mask = &ipv4_mask;
	if (l4_proto == IPPROTO_UDP) {
		udp_spec.hdr.src_port = rte_cpu_to_be_16(src_port);
		udp_mask.hdr.src_port = RTE_BE16(UINT16_MAX);
		pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
		pattern[2].spec = &udp_spec;
		pattern[2].mask = &udp_mask;
	} else if (l4_proto == IPPROTO_TCP) {
		tcp_spec.hdr.src_port = rte_cpu_to_be_16(src_port);
		tcp_mask.hdr.src_port = RTE_BE16(UINT16_MAX);
		pattern[2].type = RTE_FLOW_ITEM_TYPE_TCP;
		pattern[2].spec = &tcp_spec;
		pattern[2].mask = &tcp_mask;
	} else {
		return -EPROTONOSUPPORT;
	}
	pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

	memset(action, 0, sizeof(action));
	action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
	action[0].conf = &queue;
	action[1].type = RTE_FLOW_ACTION_TYPE_END;

	flow = rte_flow_create(global_nic_port_id, &attr, pattern, action, &error);
	if (flow != NULL)
		return 0;
	if (error.type == RTE_FLOW_ERROR_TYPE_UNSPECIFIED && rte_errno != 0)
		return -rte_errno;
	if (rte_errno != 0)
		return -rte_errno;
	return -EINVAL;
}

void publish_overloaded_flow_candidate(unsigned lcore_id, uint32_t src_ip_host,
	uint16_t src_port, uint8_t l4_proto)
{
	struct worker_shared_state *state = &worker_shared_states[lcore_id];

	if (__atomic_load_n(&state->load_state, __ATOMIC_RELAXED) != WORKER_STATE_OVERLOADED)
		return;
	if (src_ip_host == 0 || src_port == 0)
		return;
	if (l4_proto != IPPROTO_UDP && l4_proto != IPPROTO_TCP)
		return;

	rte_spinlock_lock(&state->published_lock);
	state->published_l4_proto = l4_proto;
	state->published_src_ip_host = src_ip_host;
	state->published_l4_src_port = src_port;
	state->published_valid = 1;
	rte_spinlock_unlock(&state->published_lock);
}

int try_migrate_overloaded_flow(unsigned victim_lcore, unsigned self_lcore)
{
	uint16_t from_q, to_q, src_port = 0;
	uint32_t src_ip_host = 0;
	uint8_t l4_proto = 0;
	int ret;

	if (victim_lcore == self_lcore)
		return -EINVAL;

	ret = consume_published_flow(victim_lcore, &src_ip_host, &src_port, &l4_proto);
	if (ret != 0) {
		__sync_fetch_and_add(&migration_total_fail, 1);
		return ret;
	}

	to_q = lcore_queue_map[self_lcore];
	from_q = lcore_queue_map[victim_lcore];
	if (to_q == INVALID_QUEUE_ID || from_q == INVALID_QUEUE_ID) {
		__sync_fetch_and_add(&migration_total_fail, 1);
		return -EAGAIN;
	}

	ret = program_flow_migration_rule(src_ip_host, src_port, l4_proto, to_q);
	if (ret != 0) {
		if (ret == -EEXIST || ret == EEXIST)
			__sync_fetch_and_add(&migration_total_eexist, 1);
		__sync_fetch_and_add(&migration_total_fail, 1);
		return ret;
	}

	__sync_fetch_and_add(&migration_dir_cnt[from_q][to_q], 1);
	__sync_fetch_and_add(&migration_total_success, 1);
	
	return 0;
}

void print_migration_summary(void)
{
	uint16_t n;
	uint64_t col_sum[MAX_RX_QUEUES];

	printf("\nMigration summary ================================\n");
	printf("success=%" PRIu64 " fail=%" PRIu64 " eexist=%" PRIu64 "\n",
		migration_total_success, migration_total_fail, migration_total_eexist);
	printf("Migration matrix (from_q -> to_q):\n");
	n = global_queue_num;
	if (n > MAX_RX_QUEUES)
		n = MAX_RX_QUEUES;
	memset(col_sum, 0, sizeof(col_sum));

	for (uint16_t i = 0; i < n; i++) {
		uint64_t row_sum = 0;
		printf("from q%u:", i);
		for (uint16_t j = 0; j < n; j++) {
			uint64_t v = migration_dir_cnt[i][j];
			row_sum += v;
			col_sum[j] += v;
			printf(" %" PRIu64, v);
		}
		printf(" | sum=%" PRIu64 "\n", row_sum);
	}
	printf("col sum:");
	for (uint16_t j = 0; j < n; j++)
		printf(" %" PRIu64, col_sum[j]);
	printf("\n");
	printf("==================================================\n");
}
