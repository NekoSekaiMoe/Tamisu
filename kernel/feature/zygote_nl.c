/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel <-> zygiskd netlink channel (lifecycle event push).
 *
 * Author: Anatdx
 */

#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "feature/zygote_nl.h"
#include "uapi/yukizygisk.h"
#include "klog.h" // IWYU pragma: keep

static struct sock *yz_sock;

void ksu_zygote_nl_emit_specialize(u32 pid, u32 appid)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct yz_event *ev;

	if (!yz_sock)
		return;

	skb = nlmsg_new(sizeof(*ev), GFP_ATOMIC);
	if (!skb)
		return;

	nlh = nlmsg_put(skb, 0, 0, YZ_NL_MSG_EVENT, sizeof(*ev), 0);
	if (!nlh) {
		nlmsg_free(skb);
		return;
	}

	ev = nlmsg_data(nlh);
	ev->type = YZ_EV_SPECIALIZE;
	ev->pid = pid;
	ev->appid = appid;

	/* -ESRCH just means no zygiskd is listening yet -- harmless. */
	nlmsg_multicast(yz_sock, skb, 0, YZ_NL_GROUP_EVENTS, GFP_ATOMIC);
}

/* Ask every listening zygiskd to re-read yzconfig.json (manager changed it). */
void ksu_zygote_nl_emit_reload(void)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct yz_event *ev;

	if (!yz_sock)
		return;

	skb = nlmsg_new(sizeof(*ev), GFP_ATOMIC);
	if (!skb)
		return;

	nlh = nlmsg_put(skb, 0, 0, YZ_NL_MSG_EVENT, sizeof(*ev), 0);
	if (!nlh) {
		nlmsg_free(skb);
		return;
	}

	ev = nlmsg_data(nlh);
	ev->type = YZ_EV_RELOAD;
	ev->pid = 0;
	ev->appid = 0;

	nlmsg_multicast(yz_sock, skb, 0, YZ_NL_GROUP_EVENTS, GFP_ATOMIC);
}

static void ksu_zygote_nl_recv(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;
	struct yz_event *ev;

	nlh = nlmsg_hdr(skb);
	if (nlh->nlmsg_len < NLMSG_HDRLEN + sizeof(*ev))
		return;

	ev = nlmsg_data(nlh);
	if (ev->type == YZ_EV_REPORT_SPECIALIZE) {
		pr_info("zygote_nl: userspace report pid=%u uid=%u\n", ev->pid,
			ev->appid);
		ksu_zygote_orch_on_userspace_report(ev->pid, ev->appid);
	}
}

void ksu_zygote_nl_init(void)
{
	struct netlink_kernel_cfg cfg = {
	    .groups = YZ_NL_GROUP_EVENTS,
	    .input = ksu_zygote_nl_recv,
	};

	yz_sock = netlink_kernel_create(&init_net, YZ_NETLINK_PROTO, &cfg);
	if (!yz_sock)
		pr_err("zygote_nl: netlink_kernel_create failed\n");
	else
		pr_info("zygote_nl: channel up (proto=%d)\n", YZ_NETLINK_PROTO);
}

void ksu_zygote_nl_exit(void)
{
	if (yz_sock) {
		netlink_kernel_release(yz_sock);
		yz_sock = NULL;
	}
}
