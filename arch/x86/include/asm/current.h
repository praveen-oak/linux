#ifndef _ASM_X86_CURRENT_H
#define _ASM_X86_CURRENT_H

#include <linux/compiler.h>
#include <asm/percpu.h>

#ifndef __ASSEMBLY__
struct task_struct;

DECLARE_PER_CPU(struct task_struct *, current_task);

static atomic_t __global_iowait;

static __always_inline struct task_struct *get_current(void)
{
	return this_cpu_read_stable(current_task);
}

static __always_inline atomic_t *get_global_iowait(void)
{
	return &__global_iowait;
}

#define current get_current()

#define global_iowait get_global_iowait()
#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_CURRENT_H */
