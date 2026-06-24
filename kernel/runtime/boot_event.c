#include <linux/err.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/string.h>

#include "klog.h" // IWYU pragma: keep
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "selinux/selinux.h"

bool ksu_boot_completed __read_mostly = false;

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
	ksu_stop_input_hook_runtime();
	// Keep this as a fallback in case second_stage sid cache is delayed.
	ksu_file_sid = ksu_get_ksu_file_sid();
	pr_info("ksu_file sid: %u\n", ksu_file_sid);
}

void on_module_mounted(void)
{
	pr_info("on_module_mounted!\n");
}

void on_boot_completed(void)
{
	ksu_boot_completed = true;
	pr_info("on_boot_completed!\n");
}
