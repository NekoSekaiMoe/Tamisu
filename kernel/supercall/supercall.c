#include <asm/unistd.h>
#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/printk.h>
#include <linux/sched/signal.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "arch.h"
#include "hook/syscall_hook.h"
#include "infra/file_wrapper.h"
#include "klog.h" // IWYU pragma: keep
#include "runtime/tamisu_daemon.h"
#include "selinux/selinux.h"
#include "supercall/supercall.h"
#include "supercall/internal.h"

struct tamisu_install_fd_tw {
	struct callback_head cb;
	int __user *outp;
};

static void tamisu_install_fd_tw_func(struct callback_head *cb)
{
	struct tamisu_install_fd_tw *tw =
	    container_of(cb, struct tamisu_install_fd_tw, cb);
	int fd = tamisu_install_fd();
	pr_info("[%d] install tamisu fd: %d\n", current->pid, fd);

	if (copy_to_user(tw->outp, &fd, sizeof(fd))) {
		pr_err("install fd reply err\n");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		close_fd(fd);
#else
		ksys_close(fd);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	}

	kfree(tw);
}

// downstream: make sure to pass arg as reference, this can allow us to extend
// things.
int tamisu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
			  void __user **arg)
{
	struct tamisu_install_fd_tw *tw;

	if (magic1 != TAMISU_INSTALL_MAGIC1)
		return 0;

#ifdef CONFIG_TAMISU_DEBUG
	pr_info("sys_reboot: intercepted call! magic: 0x%x id: %d\n", magic1,
		magic2);
#endif // #ifdef CONFIG_TAMISU_DEBUG

	// Check if this is a request to install tamisu fd
	if (magic2 == TAMISU_INSTALL_MAGIC2) {
		tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
		if (!tw)
			return 0;

		tw->outp = (int __user *)*arg;
		tw->cb.func = tamisu_install_fd_tw_func;

		if (task_work_add(current, &tw->cb, TWA_RESUME)) {
			kfree(tw);
			pr_warn("install fd add task_work failed\n");
		}

		return 0;
	}

	return 0;
}

// Reboot hook for installing fd
static int reboot_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = PT_REAL_REGS(regs);
	int magic1 = (int)PT_REGS_PARM1(real_regs);
	int magic2 = (int)PT_REGS_PARM2(real_regs);
	int cmd = (int)PT_REGS_PARM3(real_regs);
	void __user **arg = (void __user **)&PT_REGS_SYSCALL_PARM4(real_regs);

	return tamisu_handle_sys_reboot(magic1, magic2, cmd, arg);
}

static struct kprobe reboot_kp = {
    .symbol_name = REBOOT_SYMBOL,
    .pre_handler = reboot_handler_pre,
};

void tamisu_supercalls_init(void)
{
	int rc;

	tamisu_supercall_dump_commands();
	rc = register_kprobe(&reboot_kp);
	if (rc) {
		pr_err("reboot kprobe failed: %d\n", rc);
	} else {
		pr_info("reboot kprobe registered successfully\n");
	}
}

void tamisu_supercalls_exit(void)
{
	unregister_kprobe(&reboot_kp);
}

// IOCTL dispatcher
static long anon_tamisu_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	return tamisu_supercall_handle_ioctl(cmd, (void __user *)arg);
}

// File release handler
static int anon_tamisu_release(struct inode *inode, struct file *filp)
{
	pr_info("tamisu fd released\n");
	return 0;
}

// File operations structure
static const struct file_operations anon_tamisu_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = anon_tamisu_ioctl,
    .compat_ioctl = anon_tamisu_ioctl,
    .release = anon_tamisu_release,
};

// Install tamisu fd to current process
int tamisu_install_fd(void)
{
	struct file *filp;
	int fd;

	// Get unused fd
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		pr_err("tamisu_install_fd: failed to get unused fd\n");
		return fd;
	}

	// Create anonymous inode file.
	//
	// The name is visible via readlink("/proc/<pid>/fd/<n>") and is what
	// userspace (tamisu_daemon, manager JNI) scans to discover the driver fd after
	// the reboot-syscall handshake. The "[tamisu]" name keeps Tamisu
	// distinct from Tamisu's "[tamisu_driver]" so the two can coexist.
	filp = anon_inode_getfile("[tamisu]", &anon_tamisu_fops, NULL,
				  O_RDWR | O_CLOEXEC);
	if (IS_ERR(filp)) {
		pr_err("tamisu_install_fd: failed to create anon inode file\n");
		put_unused_fd(fd);
		return PTR_ERR(filp);
	}

	// Install fd
	fd_install(fd, filp);

	pr_info("tamisu fd installed: %d for pid %d\n", fd, current->pid);

	return fd;
}
