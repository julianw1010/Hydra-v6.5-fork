#ifndef _ASM_X86_HYDRA_PTI_H
#define _ASM_X86_HYDRA_PTI_H

#include <asm/pgtable.h>
#include <asm/cpufeature.h>

#ifdef CONFIG_PAGE_TABLE_ISOLATION

static inline bool hydra_pti_active(void)
{
	return static_cpu_has(X86_FEATURE_PTI);
}

static inline pgd_t *hydra_kernel_to_user_pgd(pgd_t *kernel_pgd)
{
	return (pgd_t *)((unsigned long)kernel_pgd + PAGE_SIZE);
}

static inline pgd_t *hydra_get_user_pgd_entry(pgd_t *kernel_pgdp)
{
	unsigned long offset;
	pgd_t *kernel_pgd_base;
	int index;

	if (!hydra_pti_active())
		return NULL;

	offset = ((unsigned long)kernel_pgdp) & (PAGE_SIZE - 1);
	index = offset / sizeof(pgd_t);

	if (index >= 256)
		return NULL;

	kernel_pgd_base = (pgd_t *)((unsigned long)kernel_pgdp & PAGE_MASK);
	return (pgd_t *)((unsigned long)kernel_pgd_base + PAGE_SIZE + offset);
}

#else

static inline bool hydra_pti_active(void)
{
	return false;
}

static inline pgd_t *hydra_kernel_to_user_pgd(pgd_t *kernel_pgd)
{
	return NULL;
}

static inline pgd_t *hydra_get_user_pgd_entry(pgd_t *kernel_pgdp)
{
	return NULL;
}

#endif

static inline int hydra_pgd_alloc_order(void)
{
	return hydra_pti_active() ? 1 : 0;
}

#endif
