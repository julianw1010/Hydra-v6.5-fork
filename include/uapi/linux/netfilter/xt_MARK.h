/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _XT_MARK_H_target
#define _XT_MARK_H_target

#include <linux/netfilter/xt_MARK.h>

struct xt_mark_info {
	unsigned long mark, mask;
	__u8 invert;
};

struct xt_mark_mtinfo1 {
	__u32 mark, mask;
	__u8 invert;
};

struct xt_mark_tginfo2 {
	__u32 mark, mask;
};

#endif /*_XT_MARK_H_target */
