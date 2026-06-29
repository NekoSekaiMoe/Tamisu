/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tamisu - kernel <-> zygiskd netlink channel (lifecycle event push).
 *
 * Author: Anatdx
 */

#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "feature/zygote_nl.h"
#include "uapi/tamisu.h"
#include "klog.h" // IWYU pragma: keep

static struct sock *tamisu_sock;

void ksu_zygote_nl_emit_specialize(u32 pid, u32 appid)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct tamisu_event *ev;

	if (!tamisu_sock)
		return;

	skb = nlmsg_new(sizeof(*ev), GFP_ATOMIC);
	if (!skb)
		return;

	nlh = nlmsg_put(skb, 0, 0, TAMISU_NL_MSG_EVENT, sizeof(*ev), 0);
	if (!nlh) {
		nlmsg_free(skb);
		return;
	}

	ev = nlmsg_data(nlh);
	ev->type = TAMISU_EV_SPECIALIZE;
	ev->pid = pid;
	ev->appid = appid;

	/* -ESRCH just means no zygiskd is listening yet -- harmless. */
	nlmsg_multicast(tamisu_sock, skb, 0, TAMISU_NL_GROUP_EVENTS, GFP_ATOMIC);
}

/* Ask every listening zygiskd to re-read tamisu_config.json (manager changed
 * it). */
void ksu_zygote_nl_emit_reload(void)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct tamisu_event *ev;

	if (!tamisu_sock)
		return;

	skb = nlmsg_new(sizeof(*ev), GFP_ATOMIC);
	if (!skb)
		return;

	nlh = nlmsg_put(skb, 0, 0, TAMISU_NL_MSG_EVENT, sizeof(*ev), 0);
	if (!nlh) {
		nlmsg_free(skb);
		return;
	}

	ev = nlmsg_data(nlh);
	ev->type = TAMISU_EV_RELOAD;
	ev->pid = 0;
	ev->appid = 0;

	nlmsg_multicast(tamisu_sock, skb, 0, TAMISU_NL_GROUP_EVENTS, GFP_ATOMIC);
}

void ksu_zygote_nl_init(void)
{
	struct netlink_kernel_cfg cfg = {
	    .groups = TAMISU_NL_GROUP_EVENTS,
	};

	tamisu_sock =
	    netlink_kernel_create(&init_net, TAMISU_NETLINK_PROTO, &cfg);
	if (!tamisu_sock)
		pr_err("zygote_nl: netlink_kernel_create failed\n");
	else
		pr_info("zygote_nl: channel up (proto=%d)\n",
			TAMISU_NETLINK_PROTO);
}

void ksu_zygote_nl_exit(void)
{
	if (tamisu_sock) {
		netlink_kernel_release(tamisu_sock);
		tamisu_sock = NULL;
	}
}
