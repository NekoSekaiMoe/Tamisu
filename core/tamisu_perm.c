#include <linux/cred.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "tamisu_internal.h"

#ifndef TAMISU_MANAGER_PACKAGE
#define TAMISU_MANAGER_PACKAGE "me.dabao1955.tamisu"
#endif

#define TAMISU_DAEMON_NAME "tamisu_daemon"
#define TAMISU_ZYGISKD_NAME "zygiskd"

/*
 * Read current task's cmdline (NUL-separated argv) into a caller-provided
 * buffer. Returns the number of bytes written (not including trailing NUL
 * padding), or a negative errno on failure. Safe to call from ioctl context
 * where current->mm is the caller's own mm.
 */
static int read_current_cmdline(char *buf, size_t buflen)
{
	struct mm_struct *mm;
	unsigned long arg_start, arg_end;
	size_t len;
	int ret = 0;

	mm = get_task_mm(current);
	if (!mm)
		return -EINVAL;

	if (!mmget_not_zero(mm)) {
		mmput(mm);
		return -EINVAL;
	}

	arg_start = mm->arg_start;
	arg_end = mm->arg_end;
	len = arg_end - arg_start;
	if (len == 0 || len > buflen - 1) {
		if (len > buflen - 1)
			ret = -ENAMETOOLONG;
		else
			ret = -EINVAL;
		mmput(mm);
		return ret;
	}

	if (copy_from_user(buf, (const char __user *)arg_start, len)) {
		mmput(mm);
		return -EFAULT;
	}

	buf[len] = '\0';
	mmput(mm);
	return (int)len;
}

/*
 * Returns true if current task's cmdline starts with one of the trusted
 * process names (manager app package, our daemon, our zygisk daemon).
 */
static bool is_trusted_process(void)
{
	char buf[256];
	int n;
	const char *trusted[] = { TAMISU_MANAGER_PACKAGE,
				  TAMISU_DAEMON_NAME, TAMISU_ZYGISKD_NAME };
	size_t i;

	n = read_current_cmdline(buf, sizeof(buf));
	if (n <= 0)
		return false;

	for (i = 0; i < ARRAY_SIZE(trusted); i++) {
		size_t tlen = strlen(trusted[i]);
		if ((size_t)n >= tlen &&
		    strncmp(buf, trusted[i], tlen) == 0 &&
		    (buf[tlen] == '\0' || buf[tlen] == ':' ||
		     buf[tlen] == ' ' || buf[tlen] == '\n'))
			return true;
	}
	return false;
}

/*
 * Permission model: callers must be either a trusted process (the manager
 * app, tamisu_daemon, or zygiskd) or uid 0 (root fallback). The cmdline check
 * gates the common case; the uid 0 fallback covers init-second-stage and any
 * future in-kernel callers that lack a meaningful cmdline.
 */
bool only_root(void)
{
	return current_uid().val == 0;
}

bool trusted_caller(void)
{
	if (current_uid().val == 0)
		return true;
	return is_trusted_process();
}

bool always_allow(void)
{
	return true; // No permission check
}

bool injected_app(void)
{
	return is_appuid(current_uid().val);
}
