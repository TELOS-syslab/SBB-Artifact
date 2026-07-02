// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <asm/io.h>
#include <asm/page.h>
#include <asm/uintr.h>

#define UINTR_UPID_CTL_IOCTL_MAGIC 'U'
#define UINTR_UPID_CTL_GET_OFFSET _IOR(UINTR_UPID_CTL_IOCTL_MAGIC, 0x01, int)

static int upid_page_ctl_get_offset(int *idx_out)
{
	struct uintr_upid *base;
	struct uintr_upid *upid;
	long idx;

	if (!current->thread.upid_ctx || !current->thread.upid_ctx->upid)
		return -ENODEV;

	base = READ_ONCE(upid_page);
	if (!base)
		return -ENODEV;

	upid = current->thread.upid_ctx->upid;
	idx = upid - base;
	if (idx < 0 || idx >= (PAGE_SIZE / (long)sizeof(*base)))
		return -ERANGE;

	*idx_out = (int)idx;
	return 0;
}

static int upid_page_ctl_open(struct inode *inode, struct file *file)
{
	if (!READ_ONCE(upid_page))
		return -ENODEV;

	/*
	 * Keep the gate simple: only tasks that already have UINTR context
	 * can open the control node.
	 */
	if (!current->thread.upid_ctx || !current->thread.upid_ctx->upid)
		return -EPERM;

	return nonseekable_open(inode, file);
}

static int upid_page_ctl_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	struct uintr_upid *page = READ_ONCE(upid_page);

	if (!page)
		return -ENODEV;

	if (size > PAGE_SIZE || vma->vm_pgoff)
		return -EINVAL;

	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

	return remap_pfn_range(vma, vma->vm_start, virt_to_phys(page) >> PAGE_SHIFT,
			       PAGE_SIZE, vma->vm_page_prot);
}

static long upid_page_ctl_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	int idx;
	int ret;

	switch (cmd) {
	case UINTR_UPID_CTL_GET_OFFSET:
		ret = upid_page_ctl_get_offset(&idx);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &idx, sizeof(idx)))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations upid_page_ctl_fops = {
	.owner		= THIS_MODULE,
	.open		= upid_page_ctl_open,
	.mmap		= upid_page_ctl_mmap,
	.unlocked_ioctl	= upid_page_ctl_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice upid_page_ctl_miscdev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "upid_page_ctl",
	.fops		= &upid_page_ctl_fops,
	.mode		= 0600,
};

static int __init upid_page_ctl_init(void)
{
	return misc_register(&upid_page_ctl_miscdev);
}
subsys_initcall(upid_page_ctl_init);
