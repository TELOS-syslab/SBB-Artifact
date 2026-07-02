#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/syscalls.h>

#include <asm/apic.h>
#include <asm/cpufeature.h>
#include <asm/io.h>
#include <asm/msr-index.h>
#include <asm/processor.h>
#include <asm/smp.h>
#include <asm/uintr.h>

struct lapic_ctl_file {
	struct task_struct *task;
};

/* Per-CPU variable to save the original timer state (before uintr_register_timer_int) */
static DEFINE_PER_CPU(struct uintr_timer_saved_state, uintr_timer_state);

/* Per-CPU variable to save the new timer state (after uintr_register_timer_int) */
static DEFINE_PER_CPU(struct uintr_timer_saved_state, uintr_timer_new_state);

static struct uintr_timer_saved_state *__uintr_select_timer_state(enum uintr_timer_state_slot slot)
{
	switch (slot) {
	case UINTR_TIMER_STATE_ORIGINAL:
		return get_cpu_ptr(&uintr_timer_state);
	case UINTR_TIMER_STATE_PROGRAMMED:
		return get_cpu_ptr(&uintr_timer_new_state);
	default:
		return NULL;
	}
}

struct uintr_timer_saved_state *uintr_get_timer_state(enum uintr_timer_state_slot slot)
{
	return __uintr_select_timer_state(slot);
}
EXPORT_SYMBOL_GPL(uintr_get_timer_state);

void uintr_put_timer_state(struct uintr_timer_saved_state *state)
{
	if (!state)
		return;

	put_cpu_ptr(state);
}
EXPORT_SYMBOL_GPL(uintr_put_timer_state);

static inline phys_addr_t lapic_phys_addr(void)
{
	u64 msr;

	if (rdmsrl_safe(MSR_IA32_APICBASE, &msr))
		return 0;

	return msr & MSR_IA32_APICBASE_BASE;
}

/*
 * read_timer_msr_ipi - Read timer MSR values and print them
 * 
 * @unused: Unused callback argument required by smp_call_function_single().
 */
void read_timer_msr_ipi(void *unused)
{
	u32 lvtt, tdcr, tmict, tmcct;
	u64 tsc_deadline;
	int cpu = smp_processor_id();

	lvtt = apic_read(APIC_LVTT);
	tdcr = apic_read(APIC_TDCR);
	tmict = apic_read(APIC_TMICT);
	tmcct = apic_read(APIC_TMCCT);
	rdmsrl(MSR_IA32_TSC_DEADLINE, tsc_deadline);

	pr_info("[UINTR]: read_timer_msr_ipi called. cpu: %d, LVTT: 0x%x, TDCR: 0x%x, TMICT: 0x%x, TMCCT: 0x%x, TSC_DEADLINE: 0x%llx\n",
		cpu, lvtt, tdcr, tmict, tmcct, tsc_deadline);
}

int uintr_compute_apic_tmict(u32 req_hz, u32 *base_hz, u32 *tmict, u32 *actual_hz)
{
	u64 apic_timer_hz;

	if (!req_hz || !lapic_timer_period)
		return -EINVAL;

	/*
	 * lapic_timer_period is APIC ticks per jiffy, so base_hz = period * HZ.
	 */
	apic_timer_hz = (u64)lapic_timer_period * HZ;
	if (!apic_timer_hz)
		return -EINVAL;

	*tmict = (u32)(apic_timer_hz / req_hz);
	if (!*tmict)
		return -EINVAL;

	*base_hz = (u32)apic_timer_hz;
	*actual_hz = *base_hz / *tmict;

	return 0;
}

void lapic_timer_switch(struct task_struct *prev_p, struct task_struct *next_p)
{
#ifdef CONFIG_X86_LOCAL_APIC
	unsigned long flags;
	bool prev_used, next_used;
	int cpu_id = smp_processor_id();
	pid_t prev_pid = prev_p->pid;
	pid_t next_pid = next_p->pid;

	if (!boot_cpu_has(X86_FEATURE_APIC))
		return;

	prev_used = prev_p->thread.lapic_timer_used;
	next_used = next_p->thread.lapic_timer_used;

	// If both tasks are using the timer, warn
	if (prev_used && next_used) {
		pr_warn("[UINTR]: lapic_timer_switch called with both tasks have lapic_timer_used. cpu: %d, prev_pid: %d, next_pid: %d\n",
			cpu_id, prev_pid, next_pid);
		return;
	}

	// If both tasks are not using the timer, do nothing
	if (!prev_used && !next_used)
		return;

	local_irq_save(flags);
	// If prev is using the timer and next is not, restore the original timer state
	if (prev_used && !next_used) {
		struct uintr_timer_saved_state *timer_state = uintr_get_timer_state(UINTR_TIMER_STATE_ORIGINAL);
		if (timer_state && timer_state->saved) {
			u32 lvtt = timer_state->lvtt;
			u32 tdcr = timer_state->tdcr;
			u32 tmict = timer_state->tmict;

			apic_write(APIC_LVTT, lvtt);
			apic_write(APIC_TDCR, tdcr);
			apic_write(APIC_TMICT, tmict);

			// pr_info("[UINTR]: lapic_timer_switch called with prev_used && !next_used. cpu: %d, prev_pid: %d, next_pid: %d, New LVTT: %x, TDCR: %x, TMICT: %x\n",
			// 	cpu_id, prev_pid, next_pid, lvtt, tdcr, tmict);
		}
		uintr_put_timer_state(timer_state);
	} else if (!prev_used && next_used) {
		// If prev is not using the timer and next is using the timer, program the new timer state
		struct uintr_timer_saved_state *new_timer_state = uintr_get_timer_state(UINTR_TIMER_STATE_PROGRAMMED);
		if (new_timer_state && new_timer_state->saved) {
			u32 lvtt = new_timer_state->lvtt;
			u32 tdcr = new_timer_state->tdcr;
			u32 tmict = new_timer_state->tmict;

			apic_write(APIC_LVTT, lvtt);
			apic_write(APIC_TDCR, tdcr);
			apic_write(APIC_TMICT, tmict);

			// pr_info("[UINTR]: lapic_timer_switch called with !prev_used && next_used. cpu: %d, prev_pid: %d, next_pid: %d, New LVTT: %x, TDCR: %x, TMICT: %x\n",
			// 	cpu_id, prev_pid, next_pid, lvtt, tdcr, tmict);
		}
		uintr_put_timer_state(new_timer_state);
	}
	local_irq_restore(flags);
#endif
}

/*********************************
* File Operations Implementation
**********************************/

static int lapic_ctl_open(struct inode *inode, struct file *file)
{
	struct lapic_ctl_file *ctx;

	if (!boot_cpu_has(X86_FEATURE_APIC))
		return -EOPNOTSUPP;

	if (x2apic_mode)
		return -EOPNOTSUPP;

	if (!READ_ONCE(current->thread.lapic_ctl_enabled))
		return -EPERM;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->task = get_task_struct(current);
	file->private_data = ctx;

	current->thread.lapic_ctl_open_count++;

	return nonseekable_open(inode, file);
}

static int lapic_ctl_release(struct inode *inode, struct file *file)
{
	struct lapic_ctl_file *ctx = file->private_data;

	if (ctx) {
		if (ctx->task->thread.lapic_ctl_open_count)
			ctx->task->thread.lapic_ctl_open_count--;
		put_task_struct(ctx->task);
		kfree(ctx);
	}

	return 0;
}

static int lapic_ctl_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	phys_addr_t lapic_phys;

	if (!READ_ONCE(current->thread.lapic_ctl_enabled))
		return -EPERM;

	if (size > PAGE_SIZE || vma->vm_pgoff)
		return -EINVAL;

	lapic_phys = lapic_phys_addr();
	if (!lapic_phys)
		return -EOPNOTSUPP;

	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return remap_pfn_range(vma, vma->vm_start, lapic_phys >> PAGE_SHIFT,
			       PAGE_SIZE, vma->vm_page_prot);
}

static const struct file_operations lapic_ctl_fops = {
	.owner		= THIS_MODULE,
	.open		= lapic_ctl_open,
	.release	= lapic_ctl_release,
	.mmap		= lapic_ctl_mmap,
	.llseek		= noop_llseek,
};

static struct miscdevice lapic_ctl_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "lapic_ctl",
	.fops	= &lapic_ctl_fops,
	.mode	= 0600,
};

static int __init lapic_ctl_init(void)
{
	if (!boot_cpu_has(X86_FEATURE_APIC))
		return 0;

	return misc_register(&lapic_ctl_miscdev);
}
subsys_initcall(lapic_ctl_init);

/*********************************
* Syscalls Implementation
**********************************/

/*
 * Read timer-related MSRs on a target CPU.
 *
 * @cpu: target CPU ID.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE1(read_timer_msr, int, cpu)
{
	int ret;

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return -EINVAL;

	ret = smp_call_function_single(cpu, read_timer_msr_ipi, NULL, 1);
	if (ret)
		return ret;

	return 0;
}

/*
 * Enable LAPIC control access for the current task.
 *
 * Allows the task to open and mmap `/dev/lapic_ctl` if APIC is supported
 * and x2APIC mode is not active.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE0(register_lapic_ctl)
{
    pr_info("[UINTR]: register_lapic_ctl called. cpu: %d\n", smp_processor_id());

	if (!boot_cpu_has(X86_FEATURE_APIC))
		return -EOPNOTSUPP;

	if (x2apic_mode)
		return -EOPNOTSUPP;

	current->thread.lapic_ctl_enabled = true;
	current->thread.lapic_ctl_open_count = 0;
	return 0;
}

/*
 * Disable LAPIC control access for the current task.
 *
 * Refuses to disable while there are active lapic_ctl opens for the task.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
SYSCALL_DEFINE0(unregister_lapic_ctl)
{
	pr_info("[UINTR]: unregister_lapic_ctl called. cpu: %d\n", smp_processor_id());

	if (!current->thread.lapic_ctl_enabled)
		return 0;

	if (current->thread.lapic_ctl_open_count)
		return -EBUSY;

    current->thread.lapic_ctl_enabled = false;
    current->thread.lapic_ctl_open_count = 0;
	return 0;
}
