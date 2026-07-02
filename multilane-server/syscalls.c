#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common.h"
#include "syscalls.h"

bool register_user_interrupt(struct worker_context *ctx)
{
	uint64_t handler_addr = (uint64_t)multilane_uintr_handler;
	long ret;

	user_intr_disable();

	ret = syscall(__NR_uintr_register_irq_handler, (long)&handler_addr, ctx->irq);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML,
			"Core %u: uintr_register_irq_handler failed for irq %u: %s\n",
			rte_lcore_id(), ctx->irq, strerror(saved_errno));
		user_intr_enable();
		return false;
	}

	ctx->timer_vector = (uint8_t)ret;

	ret = syscall(__NR_uintr_init_uitt, ctx->irq);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(WARNING, ML,
			"Core %u: uintr_init_uitt failed for irq %u: %s\n",
			rte_lcore_id(), ctx->irq, strerror(saved_errno));
	}

	user_intr_enable();

	return true;
}

int reset_upid_page(void)
{
	long ret = syscall(__NR_uintr_reset_upid_page);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML, "uintr_reset_upid_page failed: %s\n", strerror(saved_errno));
		return -saved_errno;
	}

	RTE_LOG(INFO, ML, "uintr_reset_upid_page succeeded\n");
	return 0;
}

int free_upid_page(void)
{
	long ret = syscall(__NR_uintr_free_upid_page);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML, "uintr_free_upid_page failed: %s\n", strerror(saved_errno));
		return -saved_errno;
	}

	RTE_LOG(INFO, ML, "uintr_free_upid_page succeeded\n");
	return 0;
}

int uintr_wait(int time)
{
	long ret = syscall(__NR_uintr_wait, time);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML,
			"Core %u: uintr_wait failed: %s\n",
			rte_lcore_id(), strerror(saved_errno));
		return -saved_errno;
	}

	return 0;
}

int register_queue_task(uint32_t irq)
{
	long ret = syscall(__NR_uintr_register_queue_task, irq);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML,
			"Core %u: uintr_register_queue_task failed for irq %u: %s\n",
			rte_lcore_id(), irq, strerror(saved_errno));
		return -saved_errno;
	}
	
	RTE_LOG(INFO, ML,
		"Core %u: uintr_register_queue_task succeeded for irq %u\n",
		rte_lcore_id(), irq);
	return 0;
}

int unregister_queue_task(uint32_t irq)
{
	long ret = syscall(__NR_uintr_unregister_queue_task, irq);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML,
			"Core %u: failed to unregister queue task for irq %u: %s\n",
			rte_lcore_id(), irq, strerror(saved_errno));
		return -saved_errno;
	}

	RTE_LOG(INFO, ML,
		"Core %u: unregistered queue task for irq %u\n",
		rte_lcore_id(), irq);
	return 0;
}

int uintr_kernel_handler_enable(void)
{
	long ret = syscall(__NR_uintr_kernel_handler_enable);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML, "uintr_kernel_handler_enable failed: %s\n", strerror(saved_errno));
		return -saved_errno;
	}

	RTE_LOG(INFO, ML, "uintr_kernel_handler_enable succeeded\n");
	return 0;
}

int uintr_kernel_handler_disable(void)
{
	long ret = syscall(__NR_uintr_kernel_handler_disable);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML, "uintr_kernel_handler_disable failed: %s\n", strerror(saved_errno));
		return -saved_errno;
	}

	RTE_LOG(INFO, ML, "uintr_kernel_handler_disable succeeded\n");
	return 0;
}

int register_timer_interrupt(uint32_t hz, uint8_t vector)
{
	long ret = syscall(__NR_uintr_register_timer_int, hz, vector);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML,
			"Core %u: uintr_register_timer_int failed for hz %u and vector %u: %s\n",
			rte_lcore_id(), hz, vector, strerror(saved_errno));
		return -saved_errno;
	}

	RTE_LOG(INFO, ML,
		"Core %u: uintr_register_timer_int succeeded at %u Hz (vector=%u)\n",
		rte_lcore_id(), hz, vector);
	return 0;
}

int unregister_timer_interrupt(void)
{
	long ret = syscall(__NR_uintr_unregister_timer_int);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML,
			"Core %u: uintr_unregister_timer_int failed: %s\n",
			rte_lcore_id(), strerror(saved_errno));
		return -saved_errno;
	}

	RTE_LOG(INFO, ML,
		"Core %u: uintr_unregister_timer_int succeeded\n",
		rte_lcore_id());
	return 0;
}

int register_lapic_ctl(void)
{
	long ret = syscall(__NR_register_lapic_ctl);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML,
			"Core %u: register_lapic_ctl failed: %s\n",
			rte_lcore_id(), strerror(saved_errno));
		return -saved_errno;
	}

	RTE_LOG(INFO, ML,
		"Core %u: register_lapic_ctl succeeded\n",
		rte_lcore_id());
	return 0;
}

int unregister_lapic_ctl(void)
{
	long ret = syscall(__NR_unregister_lapic_ctl);
	if (ret < 0) {
		int saved_errno = errno;
		RTE_LOG(ERR, ML,
			"Core %u: unregister_lapic_ctl failed: %s\n",
			rte_lcore_id(), strerror(saved_errno));
		return -saved_errno;
	}
	
	RTE_LOG(INFO, ML,
		"Core %u: unregister_lapic_ctl succeeded\n",
		rte_lcore_id());
	return 0;
}
