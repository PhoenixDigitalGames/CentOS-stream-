/*
 * Trampoline to trace irqs off. (otherwise CALLER_ADDR1 might crash)
 * Copyright 2008 by Steven Rostedt, Red Hat, Inc
 *  (inspired by Andi Kleen's thunk_64.S)
 * Subject to the GNU public license, v.2. No warranty of any kind.
 */
	#include <linux/linkage.h>
	#include <asm/asm.h>
	#include <asm/export.h>

	/* put return address in eax (arg1) */
	.macro THUNK name, func, put_ret_addr_in_eax=0
	.globl \name
\name:
	pushl %eax
	pushl %ecx
	pushl %edx

	.if \put_ret_addr_in_eax
	/* Place EIP in the arg1 */
	movl 3*4(%esp), %eax
	.endif

	call \func
	popl %edx
	popl %ecx
	popl %eax
	RET
	_ASM_NOKPROBE(\name)
	.endm

#ifdef CONFIG_TRACE_IRQFLAGS
	THUNK trace_hardirqs_on_thunk,trace_hardirqs_on_caller,1
	THUNK trace_hardirqs_off_thunk,trace_hardirqs_off_caller,1
#endif

#ifdef CONFIG_PREEMPTION
	THUNK ___preempt_schedule, preempt_schedule
	THUNK ___preempt_schedule_notrace, preempt_schedule_notrace
	EXPORT_SYMBOL(___preempt_schedule)
	EXPORT_SYMBOL(___preempt_schedule_notrace)
#endif

