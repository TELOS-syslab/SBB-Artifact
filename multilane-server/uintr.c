#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common.h"
#include "syscalls.h"

void user_intr_disable(void)
{
	_clui();
}

void user_intr_enable(void)
{
	_stui();
}

void user_intr_prime(void)
{
	_senduipi(0);
}

void __attribute__((interrupt, target("general-regs-only")))
multilane_uintr_handler(struct __uintr_frame *frame, uint64_t vector)
{
	(void)frame;
	(void)vector;

	if (active_worker_ctx == NULL)
		return;

	if (active_worker_ctx->app_running) {
		active_worker_ctx->packet_preempted = true;
	} else {
		active_worker_ctx->pending = 1;
	}

	if (active_worker_ctx->upid != NULL) {
		__atomic_fetch_or(&active_worker_ctx->upid->puir,
			1ULL << UINTR_UPID_PUIR_SET_BIT, __ATOMIC_SEQ_CST);
	}
}
