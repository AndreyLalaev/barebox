// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2017 Theobroma Systems Design und Consulting GmbH
 * (C) Copyright 2016 Alexander Graf <agraf@suse.de>
 */

#ifndef _SETJMP_H_
#define _SETJMP_H_	1

#include <asm/types.h>

/*
 * This really should be opaque, but the EFI implementation wrongly
 * assumes that a 'struct jmp_buf_data' is defined.
 */
struct jmp_buf_data {
#if defined(__aarch64__)
	u64  regs[13];
#else
	u32  regs[10];  /* r4-r9, sl, fp, sp, lr */
#endif
};

typedef struct jmp_buf_data jmp_buf[1];

int setjmp(jmp_buf jmp) __attribute__((returns_twice));
void longjmp(jmp_buf jmp, int ret) __attribute__((noreturn));

int initjmp(jmp_buf jmp, void __attribute__((noreturn)) (*func)(void), void *stack_top);

#include <asm-generic/setjmp.h>

#endif /* _SETJMP_H_ */
