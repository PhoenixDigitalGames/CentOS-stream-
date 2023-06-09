#include <linux/linkage.h>
#include <asm/export.h>

/*
 * Most CPUs support enhanced REP MOVSB/STOSB instructions. It is
 * recommended to use this when possible and we do use them by default.
 * If enhanced REP MOVSB/STOSB is not available, try to use fast string.
 * Otherwise, use original.
 */

/*
 * Zero a page.
 * %rdi	- page
 */
SYM_FUNC_START(clear_page_rep)
	movl $4096/8,%ecx
	xorl %eax,%eax
	rep stosq
	RET
SYM_FUNC_END(clear_page_rep)
EXPORT_SYMBOL_GPL(clear_page_rep)

SYM_FUNC_START(clear_page_orig)
	xorl   %eax,%eax
	movl   $4096/64,%ecx
	.p2align 4
.Lloop:
	decl	%ecx
#define PUT(x) movq %rax,x*8(%rdi)
	movq %rax,(%rdi)
	PUT(1)
	PUT(2)
	PUT(3)
	PUT(4)
	PUT(5)
	PUT(6)
	PUT(7)
	leaq	64(%rdi),%rdi
	jnz	.Lloop
	nop
	RET
SYM_FUNC_END(clear_page_orig)
EXPORT_SYMBOL_GPL(clear_page_orig)

SYM_FUNC_START(clear_page_erms)
	movl $4096,%ecx
	xorl %eax,%eax
	rep stosb
	RET
SYM_FUNC_END(clear_page_erms)
EXPORT_SYMBOL_GPL(clear_page_erms)
