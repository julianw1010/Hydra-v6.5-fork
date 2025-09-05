/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _XT_CONNMARK_H_target
#define _XT_CONNMARK_H_target

#include <linux/netfilter/xt_CONNMARK.h>

enum {
	XT_CONNMARK_SET,
	XT_CONNMARK_SAVE,
	XT_CONNMARK_RESTORE,
};

#define D_SHIFT_LEFT    0
#define D_SHIFT_RIGHT   1

struct xt_connmark_info {
	__u32 ctmark, ctmask, nfmask;
	__u8 mode;
};

struct xt_connmark_mtinfo1 {
	__u32 mark, mask;
	__u8 invert;
};

struct xt_connmark_tginfo1 {
	__u32 ctmark, ctmask, nfmask;
	__u8 mode;
};

struct xt_connmark_tginfo2 {
	__u32 ctmark, ctmask, nfmask;
	__u8 mode, shift_dir, shift_bits;
};


#endif /*_XT_CONNMARK_H_target*/
