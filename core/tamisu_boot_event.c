#include <linux/err.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/string.h>

#include "klog.h" // IWYU pragma: keep
#include "daemon_boot.h"
#include "daemon.h"
#include "tamisu_selinux.h"

bool tamisu_boot_completed __read_mostly = false;

void on_post_fs_data(void)
{
	static bool done = false;
	if (done) {
		pr_info("on_post_fs_data already done\n");
		return;
	}
	done = true;
	pr_info("on_post_fs_data!\n");

	// sanity check, this may influence the performance
	tamisu_stop_input_hook_runtime();
	// Keep this as a fallback in case second_stage sid cache is delayed.
	tamisu_file_sid = tamisu_get_tamisu_file_sid();
	pr_info("tamisu_file sid: %u\n", tamisu_file_sid);
}

void on_module_mounted(void)
{
	pr_info("on_module_mounted!\n");
}

void on_boot_completed(void)
{
	tamisu_boot_completed = true;
	pr_info("on_boot_completed!\n");
}
