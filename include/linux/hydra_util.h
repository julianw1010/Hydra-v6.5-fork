#ifndef _LINUX_HYDRA_UTIL_H
#define _LINUX_HYDRA_UTIL_H

#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/nodemask.h>
#include <linux/atomic.h>
#include <linux/highmem.h>
#include <linux/bitmap.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>




void migrate_pgtables_to_node(struct mm_struct *mm, pgd_t *pgd, int target_node);
void hydra_reload_cr3(void *info);
int hydra_enable_replication(struct mm_struct *mm);

#define for_each_replica(start, cur)                                     \
    for (unsigned int __repl_n = 1,                                      \
             __repl_go = 1;                                              \
         __repl_go; __repl_go = 0)                                       \
        for (cur = READ_ONCE((start)->next_replica);                     \
             cur && cur != (start) && __repl_n < NUMA_NODE_COUNT * 2;    \
             cur = READ_ONCE(cur->next_replica), ++__repl_n)

static inline int hydra_alloc_node(struct mm_struct *mm)
{
	if (!mm->lazy_repl_enabled)
		return numa_node_id();
	if (current->hydra_fault_target_node >= 0)
		return current->hydra_fault_target_node;
	return numa_node_id();
}

struct hydra_node_scope {
	int saved;
	bool active;
};

static inline struct hydra_node_scope
hydra_enter_node_scope(struct mm_struct *mm, int node)
{
	struct hydra_node_scope s = { .saved = -1, .active = false };

	if (mm->lazy_repl_enabled) {
		s.saved = current->hydra_fault_target_node;
		s.active = true;
		current->hydra_fault_target_node = node;
	}
	return s;
}

static inline void hydra_exit_node_scope(struct hydra_node_scope *s)
{
	if (s->active)
		current->hydra_fault_target_node = s->saved;
}

#define HYDRA_FIND_PGD_NONE ((void *)0x1)
#define HYDRA_FIND_P4D_NONE ((void *)0x11)
#define HYDRA_FIND_PUD_NONE ((void *)0x21)
#define HYDRA_FIND_PMD_NONE ((void *)0x31)

#define HYDRA_FIND_BAD(r) (((unsigned long)(r) & 1) == 1)

struct mitosis_pte_tracking {
    DECLARE_BITMAP(propagated, PTRS_PER_PTE);
    DECLARE_BITMAP(ever_accessed, PTRS_PER_PTE);
    atomic_long_t propagation_count;
    atomic_long_t access_count;
};

struct hydra_cache_head {
	spinlock_t lock;
	struct page *head;
	atomic_t count;
	atomic64_t hits;
	atomic64_t misses;
	atomic64_t returns;
	atomic64_t evictions;
} ____cacheline_aligned_in_smp;

extern struct hydra_cache_head hydra_cache[NUMA_NODE_COUNT];

extern bool hydra_cache_push(struct page *page, int node);
extern struct page *hydra_cache_pop(int node);
extern int hydra_cache_drain_node(int node);
extern int hydra_cache_drain_all(void);
extern void hydra_defer_pte_page_free(struct mm_struct *mm, struct page *page);
extern void hydra_drain_deferred_pages(struct mm_struct *mm);

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

static inline int hydra_collect_repl_nodes(struct page *const ptpage, nodemask_t *nodemask)
{
	struct page *cur;

	node_set(page_to_nid(ptpage), *nodemask);

	for_each_replica(ptpage, cur) {
		node_set(page_to_nid(cur), *nodemask);
	}

	return 0;
}

static inline int hydra_calculate_tlbflush_nodemask(struct page *const ptpage,
						    nodemask_t *nodemask)
{
	switch (sysctl_hydra_tlbflush_opt) {
	case 1:
	case 2:
		return hydra_collect_repl_nodes(ptpage, nodemask);
	case 3:
		if (ptpage->next_replica && ptpage->next_replica != ptpage) {
			nodes_clear(*nodemask);
			nodes_or(*nodemask, *nodemask, node_online_map);
		}
		return 0;
	}
	return 1;
}

static inline void hydra_chain_lock(struct page *master)
{
	bit_spin_lock(PG_hydra_chain_locked, (unsigned long *)&master->flags);
}

static inline void hydra_chain_unlock(struct page *master)
{
	bit_spin_unlock(PG_hydra_chain_locked, (unsigned long *)&master->flags);
}

static inline void hydra_link_page_to_replica_chain(struct page *existing_page,
						    struct page *new_page)
{
	struct page *cur;
	struct page *next_repl;
	int chain_len = 0;

	if (!existing_page || !new_page || existing_page == new_page)
		return;

	if (WARN_ON(page_to_nid(new_page) == page_to_nid(existing_page)))
		return;

	hydra_chain_lock(existing_page);

	for (cur = READ_ONCE(existing_page->next_replica);
	     cur && cur != existing_page;
	     cur = READ_ONCE(cur->next_replica)) {
		chain_len++;
		if (cur == new_page)
			goto out_unlock;
		if (WARN_ON(page_to_nid(cur) == page_to_nid(new_page)))
			goto out_unlock;
		if (WARN_ON(chain_len >= NUMA_NODE_COUNT))
			goto out_unlock;
	}

	next_repl = existing_page->next_replica;
	new_page->next_replica = next_repl ? next_repl : existing_page;
	smp_wmb();
	existing_page->next_replica = new_page;

out_unlock:
	hydra_chain_unlock(existing_page);
}

static inline void hydra_break_chain(struct page *page)
{
	struct page *cur, *next;

	if (!page || !page->next_replica)
		return;

	hydra_chain_lock(page);
	cur = page->next_replica;
	page->next_replica = NULL;
	hydra_chain_unlock(page);

	while (cur && cur != page) {
		next = READ_ONCE(cur->next_replica);
		WRITE_ONCE(cur->next_replica, NULL);
		cur = next;
	}
}

extern int sysctl_hydra_verify_enabled;
void hydra_verify_fault_walk(struct mm_struct *mm, unsigned long address,
			     int expected_node);

static inline bool hydra_try_return_page(struct page *page)
{
	int nid = page_to_nid(page);
	bool from_cache = PageHydraFromCache(page);

	ClearPageHydraFromCache(page);
	page->next_replica = NULL;

	if (from_cache && hydra_cache_push(page, nid))
		return true;

	return false;
}


static inline struct page *hydra_alloc_pt_page(struct mm_struct *mm,
					       gfp_t gfp, unsigned int order)
{
	struct page *page;
	int node;

	if (mm->lazy_repl_enabled) {
		node = hydra_alloc_node(mm);
		gfp |= __GFP_THISNODE;

		if (order == 0) {
			page = hydra_cache_pop(node);
			if (page) {
				page->pt_owner_mm = mm;
				return page;
			}
		}

		page = alloc_pages_node(node, gfp, order);
	} else {
		page = alloc_pages(gfp, order);
	}

	if (page)
		page->pt_owner_mm = mm;

	return page;
}


#endif /* _LINUX_HYDRA_UTIL_H */
