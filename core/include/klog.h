#ifndef __TAMISU_H_KLOG
#define __TAMISU_H_KLOG

#include <linux/printk.h>

#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(fmt) "Tamisu: " fmt
#endif // #ifdef pr_fmt

#endif // #ifndef __TAMISU_H_KLOG
