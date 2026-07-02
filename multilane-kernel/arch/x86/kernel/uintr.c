// SPDX-License-Identifier: GPL-2.0

#include "asm/apic.h"
#include "asm/uintr.h"
#include "asm/fpu/api.h"
#include "asm/fpu/types.h"
#include "asm/fpu/xstate.h"
#include "asm/ptrace.h"
#include "asm/msr-index.h"
#include "asm/msr.h"
#include "asm/hw_irq.h"
#include "asm/page.h"
#include "linux/bits.h"
#include "linux/bitops.h"
#include "linux/export.h"
#include "linux/gfp_types.h"
#include "linux/irq.h"
#include "linux/printk.h"
#include "linux/init.h"
#include "linux/sched.h"
#include "linux/sched/signal.h"
#include "linux/sched/task.h"
#include "linux/slab.h"
#include "linux/smp.h"
#include "linux/syscalls.h"
#include "linux/types.h"
#include "linux/spinlock.h"
#include "linux/percpu.h"
#include "linux/mutex.h"
#include "linux/hashtable.h"
#include "linux/hrtimer.h"

/*********************************
* Global Variables
**********************************/

/* Global UPID page pointer */
struct uintr_upid *upid_page;
EXPORT_SYMBOL(upid_page);

/* Atomic counter for allocated UPID slots */
static atomic_t upid_alloc_cnt = ATOMIC_INIT(0);

// Note that MultiLane only uses one UITT entry for each CPU
/* Global UITT entry for each CPU */
static DEFINE_PER_CPU(struct uitt_entry, uintr_uitt);

/* Mutex to protect upid_page */
static DEFINE_MUTEX(upid_page_mutex);

/* irq -> task mapping */
#define UINTR_MAP_HASH_BITS 8
static DEFINE_HASHTABLE(uintr_irq_task_map, UINTR_MAP_HASH_BITS);
static DEFINE_RWLOCK(uintr_irq_task_map_lock);

/* Global counter for number of processes that have called the registration syscall */
atomic_t queue_registered_count = ATOMIC_INIT(0);
EXPORT_SYMBOL(queue_registered_count);

/* Global flag to enable/disable uintr_kernel_handler */
bool uintr_kernel_handler_enabled = false;
EXPORT_SYMBOL(uintr_kernel_handler_enabled);

/*********************************
* Helper Functions
**********************************/

static inline u32 cpu_to_ndst(int cpu)
{
	u32 apicid = (u32)apic->cpu_present_to_apicid(cpu);

	WARN_ON_ONCE(apicid == BAD_APICID);

	if (!x2apic_enabled())
		return (apicid << 8) & 0xFF00;

	return apicid;
}

/* Map Linux IRQ number to its traditional x86 APIC interrupt vector */
static int irq_to_vector(unsigned int irq)
{
    struct irq_data *d = irq_get_irq_data(irq);
    struct irq_cfg *cfg;

    if (!d)
        return -EINVAL;

    cfg = irqd_cfg(d);
    if (!cfg)
        return -EINVAL;

    return (int)cfg->vector;
}

static struct uintr_irq_task_entry *uintr_find_irq_task_locked(u32 irq)
{
	struct uintr_irq_task_entry *entry;

	hash_for_each_possible(uintr_irq_task_map, entry, node, irq) {
		if (entry->irq == irq)
			return entry;
	}

	return NULL;
}

static bool uintr_current_has_pending(void)
{
	if (test_thread_flag(TIF_UINTR_RESUME)) {
		return true;
	} else {
		return false;
	}
}

/*********************************
* Initialization
**********************************/

static struct uintr_upid *alloc_upid(void)
{
	struct uintr_upid *ret;

	mutex_lock(&upid_page_mutex);

    if (!upid_page) {
		pr_err("[UINTR]: upid_page not initialized.\n");
		mutex_unlock(&upid_page_mutex);
		return NULL;
    }

    // Allocate next UPID slot from the page
    int upid_idx = atomic_inc_return(&upid_alloc_cnt) - 1;

	// Suppose an application uses limited UPID entries.
    if (upid_idx < 0 || upid_idx >= (PAGE_SIZE / (int)sizeof(struct uintr_upid))) {
		pr_err("[UINTR]: upid_idx is out of range. upid_idx: %d\n", upid_idx);
		mutex_unlock(&upid_page_mutex);
		return NULL;
    }

    ret = upid_page + upid_idx;
    pr_info("[UINTR]: alloc_upid called. page: %px, idx: %x, upid: %px\n",
        upid_page, upid_idx, ret);
    mutex_unlock(&upid_page_mutex);
    return ret;
}

static struct uintr_upid_ctx *alloc_upid_ctx(void)
{
	struct uintr_upid_ctx *upid_ctx;
	struct uintr_upid *upid;

	upid_ctx = kzalloc(sizeof(*upid_ctx), GFP_KERNEL);
	if (!upid_ctx)
		return NULL;

	upid = alloc_upid();

	if (!upid) {
		kfree(upid_ctx);
		return NULL;
	}

	upid_ctx->upid = upid;
	refcount_set(&upid_ctx->refs, 1);
	upid_ctx->task = get_task_struct(current);

	return upid_ctx;
}

struct uintr_upid *init_upid_mem(void)
{
    struct uintr_upid *new_page;
    struct uintr_upid *old_page;

	new_page = (struct uintr_upid *)get_zeroed_page(GFP_KERNEL);
	if (!new_page) {
		pr_info("[UINTR]: Failed to allocate memory for upid");
		return NULL;
	}

	mutex_lock(&upid_page_mutex);
	old_page = upid_page;
	upid_page = new_page;
	atomic_set(&upid_alloc_cnt, 0);
	mutex_unlock(&upid_page_mutex);

	if (old_page)
		free_page((unsigned long)old_page);

	pr_info("[UINTR]: init_upid_mem called. upid_page: %px\n", upid_page);
	return upid_page;
}
EXPORT_SYMBOL(init_upid_mem);

void free_upid_mem(void)
{
	struct uintr_upid *old_page;

	mutex_lock(&upid_page_mutex);
	old_page = upid_page;
	upid_page = NULL;
	atomic_set(&upid_alloc_cnt, 0);
	mutex_unlock(&upid_page_mutex);

	if (old_page)
		free_page((unsigned long)old_page);

	pr_info("[UINTR]: free_upid_mem called.\n");
}
EXPORT_SYMBOL(free_upid_mem);

static int __init uintr_init(void)
{
	pr_info("[UINTR]: uintr_init called\n");

    if (!upid_page)
        init_upid_mem();
    return 0;
}
subsys_initcall(uintr_init);

/*********************************
* Core Functions
**********************************/

/*
 * Handler registration call graph:
 *
 *   sys_uintr_register_irq_handler()
 *           |
 *           v
 *   do_uintr_register_irq_handler()  (irq -> vector -> uinv)
 *           |
 *           +----------------------------------+
 *                                              |
 *   sys_uintr_register_uinv_handler()          |
 *           |                                  |
 *           +----------------------------------+
 *                                              v
 *                                      do_uintr_register_handler()
 */

int do_uintr_register_handler(u64 handler, u8 uinv)
{
	struct uintr_upid_ctx *upid_ctx;
	struct uintr_upid *upid;
	struct task_struct *task = current;
	void *xstate;

	u64 msr_uintr_misc;
	u64 msr_uintr_stackadj = 256;
	
	upid_ctx = task->thread.upid_ctx;
	if (upid_ctx) {
		if (upid_ctx->task)
			put_task_struct(upid_ctx->task);
		kfree(upid_ctx);
	}
	upid_ctx = alloc_upid_ctx();
	if (!upid_ctx)
		return -ENOMEM;
	task->thread.upid_ctx = upid_ctx;

	xstate = start_update_xsave_msrs(XFEATURE_UINTR);

	upid = upid_ctx->upid;
	pr_info("[UINTR]: do_uintr_register_handler called. cpu: %d, uinv: %d, upid: %px\n", 
		smp_processor_id(), uinv, upid);

	upid->nc.nv = uinv;
	upid->nc.ndst = cpu_to_ndst(smp_processor_id());
	xsave_wrmsrl(xstate, MSR_IA32_UINTR_HANDLER, handler);
	xsave_wrmsrl(xstate, MSR_IA32_UINTR_PD, (u64)upid);
	xsave_wrmsrl(xstate, MSR_IA32_UINTR_STACKADJUST, msr_uintr_stackadj);
	xsave_rdmsrl(xstate, MSR_IA32_UINTR_MISC, &msr_uintr_misc);
	msr_uintr_misc |= (u64)uinv << 32;
	xsave_wrmsrl(xstate, MSR_IA32_UINTR_MISC, msr_uintr_misc);

	task->thread.upid_activated = 1;
	end_update_xsave_msrs();

	// Return: the index of the upid in the page
	return upid - (struct uintr_upid *)upid_page;
}
EXPORT_SYMBOL(do_uintr_register_handler);

int do_uintr_register_irq_handler(u64 handler, u32 irq)
{
	int upid_idx;
    int vec = irq_to_vector(irq);
    u8 uinv;

	// Validate vector and map to uinv
    if (vec <= 0) {
        pr_err("[UINTR]: Failed to get legacy vector. irq: %u\n", irq);
        return -EINVAL;
    }
    uinv = (u8)vec;

	pr_info("[UINTR]: do_uintr_register_irq_handler called. irq: %d, vec: %d, handler: %llx, cpu: %d, dest: %d\n",
		irq, vec, handler, smp_processor_id(),
		get_msi_dest_by_irq(irq));

	upid_idx = do_uintr_register_handler(handler, uinv);
	if (upid_idx < 0) {
		pr_err("[UINTR]: Failed to register UINTR handler: %d\n", upid_idx);
		return upid_idx;
	}
	
    pr_info("[UINTR]: Successfully registered IRQ %d (vec %d) to UINTR handler, upid_idx: %d\n", 
        irq, vec, upid_idx);
	
	return upid_idx;
}
EXPORT_SYMBOL(do_uintr_register_irq_handler);

int program_uitt_for_current_vector(u8 uinv)
{
    struct uintr_upid *upid;
    struct uitt_entry *uitt;
    void *xstate;
    u64 msr64;

    if (!current->thread.upid_ctx || !current->thread.upid_ctx->upid)
        return -ENODEV;

    upid = current->thread.upid_ctx->upid;
    uitt = &get_cpu_var(uintr_uitt);

    // Note that MultiLane only needs one entry for UITT, and sets 0x10 for PIR by default
	uitt->user_vec = UINTR_UPID_PUIR_SET_BIT;
    uitt->target_upid_addr = (u64)upid;
    uitt->valid = 1;

	pr_info("[UINTR]: program_uitt_for_current_vector called. cpu: %d, user_vec: %u, target_upid_addr: %px, valid: %u\n",
		smp_processor_id(), uitt->user_vec, (void *)uitt->target_upid_addr, uitt->valid);

    xstate = start_update_xsave_msrs(XFEATURE_UINTR);
    xsave_wrmsrl(xstate, MSR_IA32_UINTR_TT, (u64)uitt | 1ULL);

	/* IA32_UINTR_MISC = { Reserved[63:40], UINV[39:32], UITTSZ[31:0] } */
    // Update UITTSZ only (low 32 bits), keep Reserved and UINV unchanged.
    xsave_rdmsrl(xstate, MSR_IA32_UINTR_MISC, &msr64);
    msr64 &= GENMASK_ULL(63, 32);
	// Use only one valid UITT entry
    msr64 |= (1 - 1); 
    xsave_wrmsrl(xstate, MSR_IA32_UINTR_MISC, msr64);

    end_update_xsave_msrs();

    put_cpu_var(uintr_uitt);
    return 0;
}

static int uintr_register_irq_task(u32 irq, struct task_struct *task)
{
	struct uintr_irq_task_entry *entry;
	struct task_struct *old_task = NULL;
	unsigned long flags;

	if (!irq_get_irq_data(irq))
		return -EINVAL;

	write_lock_irqsave(&uintr_irq_task_map_lock, flags);
	entry = uintr_find_irq_task_locked(irq);
	if (entry) {
		old_task = entry->task;
		get_task_struct(task);
		entry->task = task;
		write_unlock_irqrestore(&uintr_irq_task_map_lock, flags);
		if (old_task)
			put_task_struct(old_task);
		return 0;
	}
	write_unlock_irqrestore(&uintr_irq_task_map_lock, flags);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	get_task_struct(task);
	entry->irq = irq;
	entry->task = task;

	write_lock_irqsave(&uintr_irq_task_map_lock, flags);
	{
		struct uintr_irq_task_entry *old_entry = uintr_find_irq_task_locked(irq);

		if (!old_entry) {
			hash_add(uintr_irq_task_map, &entry->node, irq);
			write_unlock_irqrestore(&uintr_irq_task_map_lock, flags);
			return 0;
		}

		old_task = old_entry->task;
		get_task_struct(task);
		old_entry->task = task;
		write_unlock_irqrestore(&uintr_irq_task_map_lock, flags);

		put_task_struct(entry->task);
		kfree(entry);
		if (old_task)
			put_task_struct(old_task);
		return 0;
	}
}

static int uintr_unregister_irq_task(u32 irq, struct task_struct *task)
{
	struct uintr_irq_task_entry *entry;
	unsigned long flags;

	write_lock_irqsave(&uintr_irq_task_map_lock, flags);
	entry = uintr_find_irq_task_locked(irq);
	if (!entry) {
		write_unlock_irqrestore(&uintr_irq_task_map_lock, flags);
		return -ENOENT;
	}

	if (entry->task != task) {
		write_unlock_irqrestore(&uintr_irq_task_map_lock, flags);
		return -EPERM;
	}

	hash_del(&entry->node);
	write_unlock_irqrestore(&uintr_irq_task_map_lock, flags);

	if (entry->task)
		put_task_struct(entry->task);
	kfree(entry);
	return 0;
}

void switch_uintr_return(void)
{
	u64 misc_msr;
	u64 uirr;

	if (test_thread_flag(TIF_UINTR_RESUME))
		clear_thread_flag(TIF_UINTR_RESUME);

	if (!current->thread.upid_activated ||
	    !current->thread.upid_ctx ||
	    !current->thread.upid_ctx->upid)
		return;

	WARN_ON_ONCE(test_thread_flag(TIF_NEED_FPU_LOAD));

	current->thread.upid_ctx->upid->nc.ndst = cpu_to_ndst(smp_processor_id());

	rdmsrl(MSR_IA32_UINTR_MISC, misc_msr);
	if (!(misc_msr & GENMASK_ULL(39, 32))) {
		misc_msr |= (u64)current->thread.upid_ctx->upid->nc.nv << 32;
		wrmsrl(MSR_IA32_UINTR_MISC, misc_msr);
	}

	if (READ_ONCE(current->thread.upid_ctx->upid->puir)) {
		apic->send_IPI_self((u64)current->thread.upid_ctx->upid->nc.nv);
		// pr_info("[UINTR]: Sending SELF IPI. cpu: %d\n", smp_processor_id());
	}

	// pr_info("[UINTR]: switch_uintr_return called. cpu: %d, pid: %d, uirr: %llx, upid->pir: %llx\n", 
	// 	smp_processor_id(), current->pid, uirr, current->thread.upid_ctx->upid->puir);
}

/*
 * uintr_kernel_handler - Kernel interrupt handler for UINTR-based queue interrupts
 *
 * @irq: Interrupt number
 * @dev_id: request_irq() device id (unused)
 *
 * Returns IRQ_HANDLED to indicate the interrupt was processed.
 */
irqreturn_t uintr_kernel_handler(int irq, void *dev_id)
{
	struct task_struct *target_task;
	struct uintr_irq_task_entry *entry;
	struct uintr_upid_ctx *upid_ctx;
	struct uintr_upid *upid;
	unsigned long flags;
	(void)dev_id;

	read_lock_irqsave(&uintr_irq_task_map_lock, flags);
	entry = uintr_find_irq_task_locked(irq);
	if (!entry || !entry->task) {
		read_unlock_irqrestore(&uintr_irq_task_map_lock, flags);
		pr_warn("[UINTR]: No task registered for irq %d\n", irq);
		return IRQ_HANDLED;
	}

	target_task = entry->task;
	get_task_struct(target_task);
	read_unlock_irqrestore(&uintr_irq_task_map_lock, flags);

	upid_ctx = READ_ONCE(target_task->thread.upid_ctx);
	upid = upid_ctx ? READ_ONCE(upid_ctx->upid) : NULL;
	if (!upid || !READ_ONCE(target_task->thread.upid_activated)) {
		pr_warn("[UINTR]: Task %d has no active UINTR context for irq %d\n",
			target_task->pid, irq);
	} else {
		set_bit(UINTR_UPID_PUIR_SET_BIT, (unsigned long *)&upid->puir);
		smp_mb__after_atomic();

		set_tsk_thread_flag(target_task, TIF_UINTR_RESUME);
		wake_up_process(target_task);
	}

	put_task_struct(target_task);
	return IRQ_HANDLED;
}
EXPORT_SYMBOL(uintr_kernel_handler);

/*********************************
* Syscalls Implementation
**********************************/

/*
 * Register a UINTR handler for the current task and user interrupt vector.
 *
 * @handler: userspace pointer to a u64 containing the handler RIP.
 * @uinv:    user interrupt notification vector.
 *
 * Returns vector value on success, or a negative errno on failure.
 */
SYSCALL_DEFINE2(uintr_register_irq_handler, u64 __user *, handler, u32, irq)
{
	u64 handler_addr;
	int upid_idx;
	int vec;
	
	// Copy handler address from userspace
	if (copy_from_user(&handler_addr, handler, sizeof(u64))) {
		return -EFAULT;
	}
	
	pr_info("[UINTR]: uintr_register_irq_handler called. cpu: %d, addr: 0x%016llx, irq: %d\n", smp_processor_id(), handler_addr, irq);
	
	// Get vector for this IRQ
	vec = irq_to_vector(irq);
	if (vec <= 0) {
		pr_err("[UINTR]: Failed to get vector for IRQ %u\n", irq);
		return -EINVAL;
	}
	
	// Register UINTR handler
	upid_idx = do_uintr_register_irq_handler(handler_addr, irq);
	if (upid_idx < 0) {
		pr_err("[UINTR]: Failed to register UINTR handler: %d\n", upid_idx);
		return upid_idx;
	}
	
	pr_info("[UINTR]: Successfully registered UINV UINTR handler. upid_idx: %d, vec: %d\n", upid_idx, vec);
	return vec;
}

/*
 * Register a UINTR handler for the current task and user interrupt vector.
 *
 * @handler: userspace pointer to a u64 containing the handler RIP.
 * @uinv:    user interrupt notification vector.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE2(uintr_register_uinv_handler, u64 __user *, handler, u8, uinv)
{
	u64 handler_addr;
	int upid_idx;
	
	// Copy handler address from userspace
	if (copy_from_user(&handler_addr, handler, sizeof(u64))) {
		return -EFAULT;
	}
	
	pr_info("[UINTR]: uintr_register_uinv_handler called. cpu: %d, addr: 0x%016llx, uinv: %d\n", smp_processor_id(), handler_addr, uinv);
	
	// Register UINTR handler
	upid_idx = do_uintr_register_handler(handler_addr, uinv);
	if (upid_idx < 0) {
		pr_err("[UINTR]: Failed to register UINTR handler. upid_idx: %d\n", upid_idx);
		return upid_idx;
	}
	
	pr_info("[UINTR]: Successfully registered UINV UINTR handler. upid_idx: %d\n", upid_idx);
	return 0;
}

/*
 * Reset UPID page for fresh allocation.
 *
 * This syscall allocates a new zeroed UPID page, swaps it in, and resets
 * allocation index to 0.
 *
 * Returns 0 on success, or -ENOMEM on allocation failure.
 */
SYSCALL_DEFINE0(uintr_reset_upid_page)
{
	if (!init_upid_mem())
		return -ENOMEM;

	pr_info("[UINTR]: uintr_reset_upid_page called. upid_page: %px\n", upid_page);
	return 0;
}

/*
 * Free current UPID page and reset allocation index.
 *
 * Returns 0 on success.
 */
SYSCALL_DEFINE0(uintr_free_upid_page)
{
	pr_info("[UINTR]: uintr_free_upid_page called.\n");
	free_upid_mem();

	return 0;
}

/*
 * Return UINTR status for the current task.
 *
 * Return value: UIRR
 *
 * Returns -ENODEV if UINTR is not enabled for the current task.
 */
SYSCALL_DEFINE1(get_uintr_status, int, upid_idx)
{
	pr_info("[UINTR]: get_uintr_status called. cpu: %d, upid_idx: %d\n", smp_processor_id(), upid_idx);

	u64 uirr = 0;
	struct uintr_upid *upid;
	
	// Check whether current task has a UINTR context
	if (!current->thread.upid_ctx || !current->thread.upid_activated) {
		return -ENODEV;
	}
	
	upid = current->thread.upid_ctx->upid;
	if (!upid) {
		return -EINVAL;
	}

	// Read the UIRR register
	rdmsrl_safe(MSR_IA32_UINTR_RR, &uirr);
	
	// Output UIRR and complete PUIR for user-side parsing
    pr_info("[UINTR]: get_uintr_status called. UIRR=0x%llx, PUIR=0x%016llx\n",
        uirr, (unsigned long long)READ_ONCE(upid->puir));
	
	return (long)uirr;
}

/*
 * Set or clear the UPID SN (Suppress Notification) bit.
 *
 * @enable: non-zero sets SN, zero clears SN.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE1(uintr_set_sn, int, enable)
{
	pr_info("[UINTR]: uintr_set_sn called. cpu: %d\n", smp_processor_id());

    struct uintr_upid *upid;
    if (!current->thread.upid_ctx || !current->thread.upid_ctx->upid)
        return -ENODEV;

    upid = current->thread.upid_ctx->upid;

    // status bit0: ON, bit1: SN; keep ON/other bits unchanged, update SN only.
    if (enable)
		// SN = 1
        upid->nc.status |= 0x2; 
    else
		// SN = 0
        upid->nc.status &= ~0x2;
	
    smp_mb();
    pr_info("[UINTR]: uintr_set_sn called. cpu: %d, upid: %px, status: 0x%02x, nv: %u, ndst: 0x%08x, puir: 0x%016llx\n",
        smp_processor_id(), upid, upid->nc.status, upid->nc.nv, upid->nc.ndst,
        (unsigned long long)READ_ONCE(upid->puir));
	
    return 0;
}

/*
 * Initialize or update UITT for the current task, in order to make SENDUIPI target its own UPID.
 *
 * @irq: Linux IRQ number used to derive the notification vector.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE1(uintr_init_uitt, u32, irq)
{
	pr_info("[UINTR]: uintr_init_uitt called. cpu: %d, irq: %u\n", smp_processor_id(), irq);

    int vec = irq_to_vector(irq);
    if (vec <= 0)
        return -EINVAL;

    return program_uitt_for_current_vector((u8)vec);
}

/*
 * Set the specified PIR bit in current thread's UPID, only if PIR is currently zero.
 *
 * @bit_index: PIR bit index in range [0, 63].
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE1(uintr_set_pir_bit, int, bit_index)
{
	pr_info("[UINTR]: uintr_set_pir_bit called. cpu: %d, bit_index: %d\n", smp_processor_id(), bit_index);

    struct uintr_upid *upid;
    u64 pir;

    if (!current->thread.upid_ctx || !current->thread.upid_ctx->upid)
        return -ENODEV;
    if (bit_index < 0 || bit_index > 63)
        return -EINVAL;

    upid = current->thread.upid_ctx->upid;
    pir = READ_ONCE(upid->puir);

	// If PIR is non-zero, return error.
    if (pir != 0)
        return -EBUSY; 

    pir = (1ULL << bit_index);
    WRITE_ONCE(upid->puir, pir);
    smp_mb();

    return 0;
}

/*
 * Wait until a UINTR notification is pending for the current task.
 *
 * @timeout_us: timeout in microseconds; 0 or negative waits indefinitely.
 *
 * Returns 0 on success, or -ENODEV if UINTR is not enabled for the current task.
 */
SYSCALL_DEFINE1(uintr_wait, int, timeout_us)
{
	// pr_info("[UINTR]: uintr_wait called. cpu: %d, timeout_us: %d\n", smp_processor_id(), timeout_us);

	struct uintr_upid *upid;
	ktime_t expires;
	bool timed = timeout_us > 0;

	if (!READ_ONCE(current->thread.upid_activated) ||
	    !READ_ONCE(current->thread.upid_ctx) ||
	    !READ_ONCE(current->thread.upid_ctx->upid))
		return -ENODEV;

	if (timed)
		expires = ktime_add_us(ktime_get(), timeout_us);

	for (;;) {
		if (uintr_current_has_pending())
			break;

		if (signal_pending(current))
			break;

		if (timed && ktime_compare(ktime_get(), expires) >= 0)
			break;

		set_current_state(TASK_INTERRUPTIBLE);

		if (uintr_current_has_pending())
			break;

		if (signal_pending(current)) {
			break;
		}

		if (timed) {
			struct hrtimer_sleeper t;

			hrtimer_init_sleeper_on_stack(&t, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
			hrtimer_set_expires(&t.timer, expires);
			hrtimer_sleeper_start_expires(&t, HRTIMER_MODE_ABS);

			if (likely(t.task)) {
				schedule();
			}

			hrtimer_cancel(&t.timer);
			destroy_hrtimer_on_stack(&t.timer);

			if (!t.task) {
				break;
			}
		} else {
			schedule();
		}
	}

	set_thread_flag(TIF_UINTR_RESUME);
	__set_current_state(TASK_RUNNING);

	return 0;
}

/*
 * Register the current task for an IRQ.
 *
 * @irq: Linux IRQ number.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE1(uintr_register_queue_task, u32, irq)
{
	pr_info("[UINTR]: uintr_register_queue_task called. cpu: %d, pid: %d, irq: %u, queue_registered_count: %d\n",
		smp_processor_id(), current->pid, irq, atomic_read(&queue_registered_count));

	int ret = uintr_register_irq_task(irq, current);
	if (ret)
		return ret;

	// Increment global counter for total number of syscall invocations
	atomic_inc(&queue_registered_count);

	return 0;
}

/*
 * Unregister the current task from an IRQ.
 *
 * @irq: Linux IRQ number.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE1(uintr_unregister_queue_task, u32, irq)
{
	pr_info("[UINTR]: uintr_unregister_queue_task called. cpu: %d, pid: %d, irq: %u, queue_registered_count: %d\n",
		smp_processor_id(), current->pid, irq, atomic_read(&queue_registered_count));

	int ret = uintr_unregister_irq_task(irq, current);
	if (ret) {
		if (ret == -EPERM)
			pr_warn("[UINTR]: Task %d attempted to unregister irq %u owned by another task\n",
				current->pid, irq);
		return ret;
	}

	// Decrement the global counter
	atomic_dec(&queue_registered_count);

	return 0;
}

/*
 * Enable the kernel-side UINTR handler path.
 *
 * Returns 0 on success.
 */
SYSCALL_DEFINE0(uintr_kernel_handler_enable)
{
	pr_info("[UINTR]: uintr_kernel_handler_enable called. cpu: %d\n", smp_processor_id());

	uintr_kernel_handler_enabled = true;

	return 0;
}

/*
 * Disable the kernel-side UINTR handler path.
 *
 * Returns 0 on success.
 */
SYSCALL_DEFINE0(uintr_kernel_handler_disable)
{
	pr_info("[UINTR]: uintr_kernel_handler_disable called. cpu: %d\n", smp_processor_id());

	uintr_kernel_handler_enabled = false;
	
	return 0;
}

/*
 * Register a local APIC timer interrupt for the current CPU.
 *
 * @hz: timer frequency in Hz.
 * @uinv: user interrupt notification vector.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE2(uintr_register_timer_int, u32, hz, u8, uinv)
{
	pr_info("[UINTR]: uintr_register_timer_int called. cpu: %d, hz: %u, uinv: %u\n", smp_processor_id(), hz, uinv);

	unsigned long flags;
	u32 clock;
	u32 actual_hz;
	u32 tmict_value;
	u32 lvtt_value;
	struct uintr_timer_saved_state *timer_state;
	int ret;

	if (!hz) {
		pr_warn("[UINTR]: invalid hz=0\n");
		return -EINVAL;
	}

	// Check if lapic_timer_period is available
	if (!lapic_timer_period) {
		pr_warn("[UINTR]: APIC timer period not calibrated yet\n");
		return -EINVAL;
	} else {
		pr_info("[UINTR]: lapic_timer_period = %u\n", lapic_timer_period);
	}

	ret = uintr_compute_apic_tmict(hz, &clock, &tmict_value, &actual_hz);
	if (ret) {
		pr_warn("[UINTR]: failed to compute APIC timer for hz=%u (lapic_timer_period=%u)\n",
			hz, lapic_timer_period);
		return -EINVAL;
	}
	pr_info("[UINTR]: APIC timer base_hz=%u, requested_hz=%u, actual_hz=%u, tmict=%u\n",
		clock, hz, actual_hz, tmict_value);

	// Save interrupt flags and disable interrupts
	local_irq_save(flags);

	timer_state = uintr_get_timer_state(UINTR_TIMER_STATE_ORIGINAL);
	if (timer_state && !timer_state->saved) {
		timer_state->lvtt = apic_read(APIC_LVTT);
		timer_state->tdcr = apic_read(APIC_TDCR);
		timer_state->tmict = apic_read(APIC_TMICT);

		pr_info("[UINTR]: timer_state saved. LVTT=0x%x TDCR=0x%x TMICT=0x%x\n",
			timer_state->lvtt, timer_state->tdcr, timer_state->tmict);

		timer_state->saved = true;
	}
	uintr_put_timer_state(timer_state);

	/*
	 * Configure APIC timer:
	 * - APIC_LVTT: Set to one-shot or periodic timer mode with the specified vector, initially masked
	 * - APIC_TDCR: Set divider to 1
	 * - APIC_TMICT: Set initial count value
	 */
	lvtt_value = APIC_LVT_TIMER_PERIODIC | APIC_LVT_MASKED | uinv;
	apic_write(APIC_LVTT, lvtt_value);
	apic_write(APIC_TDCR, APIC_TDR_DIV_1);
	apic_write(APIC_TMICT, tmict_value);
	current->thread.lapic_timer_used = true;

	// Save the new timer state (after uintr_register_timer_int)
	struct uintr_timer_saved_state *new_timer_state =
		uintr_get_timer_state(UINTR_TIMER_STATE_PROGRAMMED);
	if (new_timer_state && !new_timer_state->saved) {
		new_timer_state->lvtt = lvtt_value;
		new_timer_state->tdcr = APIC_TDR_DIV_1;
		new_timer_state->tmict = tmict_value;
		new_timer_state->saved = true;
	}
	uintr_put_timer_state(new_timer_state);

	pr_info("[UINTR]: timer_state configured. APIC_LVTT: 0x%x, APIC_TDCR: 0x%x, APIC_TMICT: 0x%x, APIC_TMCCT: 0x%x\n",
		apic_read(APIC_LVTT), apic_read(APIC_TDCR), apic_read(APIC_TMICT),
		apic_read(APIC_TMCCT));

	// Restore interrupt flags
	local_irq_restore(flags);

	return 0;
}

/*
 * Unregister the periodic local APIC timer interrupt on the current CPU.
 *
 * Restores previously saved APIC timer register state.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE0(uintr_unregister_timer_int)
{
	pr_info("[UINTR]: uintr_unregister_timer_int called. cpu: %d\n", smp_processor_id());

	unsigned long flags;
	struct uintr_timer_saved_state *timer_state;
	u32 old_lvtt, old_tdcr, old_tmict;
	int cpu;

	timer_state = uintr_get_timer_state(UINTR_TIMER_STATE_ORIGINAL);
	cpu = smp_processor_id();

	if (!timer_state->saved) {
		pr_warn("[UINTR]: no saved timer state to restore on CPU%d\n", cpu);
		uintr_put_timer_state(timer_state);
		return -EINVAL;
	}
	
	pr_info("[UINTR]: timer_state before restore: cpu: %d, LVTT: 0x%x, TDCR: 0x%x, TMICT: 0x%x\n",
		cpu, apic_read(APIC_LVTT), apic_read(APIC_TDCR), apic_read(APIC_TMICT));

	old_lvtt = timer_state->lvtt;
	old_tdcr = timer_state->tdcr;
	old_tmict = timer_state->tmict;

	local_irq_save(flags);
	apic_write(APIC_LVTT, old_lvtt);
	apic_write(APIC_TDCR, old_tdcr);
	apic_write(APIC_TMICT, old_tmict);
	timer_state->saved = false;
	timer_state->lvtt = 0;
	timer_state->tdcr = 0;
	timer_state->tmict = 0;
	local_irq_restore(flags);

	uintr_put_timer_state(timer_state);

	pr_info("[UINTR]: timer_state after restore: cpu: %d, LVTT: 0x%x, TDCR: 0x%x, TMICT: 0x%x\n",
		cpu, old_lvtt, old_tdcr, old_tmict);

	current->thread.lapic_timer_used = false;
	
	return 0;
}
