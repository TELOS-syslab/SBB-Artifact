/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UINTR_H
#define _ASM_X86_UINTR_H

#include <linux/sched.h>
#include <linux/irqreturn.h>

/* User Posted Interrupt Descriptor (UPID) */
struct uintr_upid {
	struct {
		u8 status;     /* bit 0: ON, bit 1: SN, bit 2-7: reserved */
		u8 reserved1;  /* Reserved */
		u8 nv;         /* Notification vector */
		u8 reserved2;  /* Reserved */
		u32 ndst;      /* Notification destination */
	} nc __packed;     /* Notification control */
	u64 puir;          /* Posted user interrupt requests */
} __aligned(64);

/* UPID Context structure */
struct uintr_upid_ctx {
	struct task_struct *task;     /* Receiver task */
	struct uintr_upid *upid;
	refcount_t refs;
};

/* UITT entry structure */
struct uitt_entry {
	u8 valid;
	u8 user_vec;
	u8 reserved[6];
	u64 target_upid_addr;
} __packed __aligned(16);

/* irq -> task mapping */
struct uintr_irq_task_entry {
	struct hlist_node node;
	u32 irq;
	struct task_struct *task;
};

/* Outstanding notification */
#define UINTR_UPID_STATUS_ON 0x0
/* Suppressed notification */
#define UINTR_UPID_STATUS_SN 0x1
/* PIR set bit */
#define UINTR_UPID_PUIR_SET_BIT 16

/* Global UPID page pointer */
extern struct uintr_upid *upid_page;

/* Global counter for number of processes that have called the registration syscall */
extern atomic_t queue_registered_count;

/* Global flag to enable/disable uintr_kernel_handler */
extern bool uintr_kernel_handler_enabled;

struct uintr_upid *init_upid_mem(void);
void free_upid_mem(void);
int program_uitt_for_current_vector(u8 uinv);

void switch_uintr_return(void);
int do_uintr_register_handler(u64 handler, u8 uinv);
int do_uintr_register_irq_handler(u64 handler, u32 irq);
irqreturn_t uintr_kernel_handler(int irq, void *dev_id);

/*********************************
* Timer State Management
**********************************/

enum uintr_timer_state_slot {
	UINTR_TIMER_STATE_ORIGINAL = 0,
	UINTR_TIMER_STATE_PROGRAMMED,
};

struct uintr_timer_saved_state {
	u32 lvtt;
	u32 tdcr;
	u32 tmict;
	bool saved;
};

struct uintr_timer_saved_state *uintr_get_timer_state(enum uintr_timer_state_slot slot);
void uintr_put_timer_state(struct uintr_timer_saved_state *state);

void read_timer_msr_ipi(void *unused);
int uintr_compute_apic_tmict(u32 req_hz, u32 *base_hz, u32 *tmict, u32 *actual_hz);
void lapic_timer_switch(struct task_struct *prev_p, struct task_struct *next_p);

#endif /* _ASM_X86_UINTR_H */
