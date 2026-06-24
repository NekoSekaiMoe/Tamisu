#include <linux/cred.h>

#include "supercall/internal.h"

/*
 * Zygisk-only build: there is no manager app and no su allowlist. Every
 * privileged ioctl is gated on root (uid 0) only; the daemon (ksud/zygiskd)
 * always runs as root. "manager_or_root" collapses to root-only as well,
 * since the only legitimate caller is root.
 */
bool only_root(void)
{
	return current_uid().val == 0;
}

bool manager_or_root(void)
{
	return current_uid().val == 0;
}

bool always_allow(void)
{
	return true; // No permission check
}
