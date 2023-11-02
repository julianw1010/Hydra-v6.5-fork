#ifndef _LINUX_HYDRA_UTIL_H
#define _LINUX_HYDRA_UTIL_H

#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/hydra_debug.h>
#include <linux/nodemask.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#define HYDRA_FIND_PGD_NONE ((void *)0x1)
#define HYDRA_FIND_P4D_NONE ((void *)0x11)
#define HYDRA_FIND_PUD_NONE ((void *)0x21)
#define HYDRA_FIND_PMD_NONE ((void *)0x31)

#define HYDRA_FIND_BAD(r) (((unsigned long)(r) & 1) == 1)

static inline pte_t *hydra_find_pte(struct mm_struct *mm, unsigned long address, int node) {
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset_node(mm, address, node);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return HYDRA_FIND_PGD_NONE;

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		return HYDRA_FIND_P4D_NONE;

	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return HYDRA_FIND_PUD_NONE;

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)) ||
	    unlikely(pmd_trans_huge(*pmd)))
		return HYDRA_FIND_PMD_NONE;

	return pte_offset_kernel(pmd, address);
}

static inline int hydra_collect_repl_nodes(struct page *const ptpage, nodemask_t *nodemask) {
  struct page *current_page = ptpage;
  unsigned int replicas_seen = 1;

  do {
		if (!current_page) {
			break;
		}
		if (replicas_seen >= NUMA_NODE_COUNT * 2) {
			WARN(1, "xxxxx replicas_seen = %d > NUMA_NODE_COUNT = %d", replicas_seen, NUMA_NODE_COUNT);
			if (current) {
				do_send_sig_info(SIGKILL, SEND_SIG_PRIV, current, true);
			}
			return 1;
		}
		node_set(page_to_nid(current_page), *nodemask);
		current_page = current_page->next_replica;
		if (current_page == NULL) break;
		++replicas_seen;
		if (current_page != ptpage && replicas_seen == NUMA_NODE_COUNT){
			node_set(page_to_nid(current_page), *nodemask);
			break;
		}
	} while (current_page != ptpage);
	return 0;
}

static inline int hydra_calculate_tlbflush_nodemask(struct page *const ptpage, nodemask_t *nodemask) {
	switch (sysctl_hydra_tlbflush_opt) {
	case 1:
		return hydra_collect_repl_nodes(ptpage, nodemask);
	case 2:
		if (ptpage->next_replica && ptpage->next_replica != ptpage) {
			nodes_setall(*nodemask);
		}
		return 0;
	}
	return 1;
}

#endif
