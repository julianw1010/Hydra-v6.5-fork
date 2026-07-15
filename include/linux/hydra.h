#ifndef _LINUX_HYDRA_H
#define _LINUX_HYDRA_H

#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/nodemask.h>
#include <linux/atomic.h>
#include <linux/highmem.h>
#include <linux/bitmap.h>
#include <linux/jump_label.h>
#include <linux/xarray.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#ifndef CONFIG_MMU_GATHER_RCU_TABLE_FREE
#error "hydra next_replica walkers are preemptible-RCU readers and need the RCU-freed page-table batch selected by CONFIG_MMU_GATHER_RCU_TABLE_FREE (selected on x86 under CONFIG_PARAVIRT); the plain page batch does not give a grace period"
#endif

DECLARE_STATIC_KEY_FALSE(hydra_repl_ever_enabled);

void hydra_reload_cr3(void *info);
int hydra_enable_replication(struct mm_struct *mm);
int hydra_repl_fault(struct vm_fault *vmf, int fault_node);
bool hydra_move_normal_pmd(struct vm_area_struct *vma, unsigned long old_addr,
			   pmd_t *old_pmd, pmd_t *new_pmd);
void hydra_break_chain_range(struct mm_struct *mm,
			     unsigned long start, unsigned long end,
			     unsigned long floor, unsigned long ceiling);
void hydra_pud_owner_claim(struct mm_struct *mm, unsigned long start,
			   unsigned long end, int node);
void hydra_pud_owner_stamp(struct mm_struct *mm, unsigned long start,
			   unsigned long end, int node);

#define HYDRA_WALK_NONE ((void *)0x1)

#define HYDRA_WALK_BAD(r) (((unsigned long)(r) & 1) == 1)

static inline pgd_t *hydra_pgd_offset(struct mm_struct *mm,
				       unsigned long address,
				       unsigned long node)
{
	if (mm->lazy_repl_enabled)
		return pgd_offset_node(mm, address, node);
	return pgd_offset(mm, address);
}

static inline pmd_t *hydra_walk_to_pmd(struct mm_struct *mm,
				       unsigned long address, int node)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset_node(mm, address, node);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return (pmd_t *)HYDRA_WALK_NONE;

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		return (pmd_t *)HYDRA_WALK_NONE;

	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return (pmd_t *)HYDRA_WALK_NONE;

	return pmd_offset(pud, address);
}

static inline pte_t *hydra_walk_to_pte(struct mm_struct *mm,
				       unsigned long address, int node)
{
	pmd_t *pmd;

	pmd = hydra_walk_to_pmd(mm, address, node);
	if (HYDRA_WALK_BAD(pmd))
		return (pte_t *)pmd;

	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)) ||
	    unlikely(pmd_trans_huge(*pmd)))
		return (pte_t *)HYDRA_WALK_NONE;

	return pte_offset_kernel(pmd, address);
}

struct hydra_cache_head {
	spinlock_t lock;
	struct page *head;
	atomic_t count;
	atomic64_t hits;
	atomic64_t misses;
	atomic64_t returns;
} ____cacheline_aligned_in_smp;

extern struct hydra_cache_head hydra_cache[NUMA_NODE_COUNT];

extern bool hydra_cache_push(struct page *page, int node, bool count_stats);
extern struct page *hydra_cache_pop(int node, bool count_stats);
extern int hydra_cache_drain_node(int node);
extern int hydra_cache_drain_all(void);

static inline int hydra_collect_repl_nodes(struct page *const ptpage,
					   nodemask_t *nodemask)
{
	struct page *cur_page;
	struct page *start_page;

	start_page = ptpage;
	cur_page = ptpage;

	rcu_read_lock();
	do {
		node_set(page_to_nid(cur_page), *nodemask);
		cur_page = cur_page->next_replica;
	} while (cur_page && cur_page != start_page);
	rcu_read_unlock();

	return 0;
}

static inline void hydra_chain_lock(struct page *master)
{
	bit_spin_lock(PG_hydra_chain_locked, (unsigned long *)&master->flags);
}

static inline void hydra_chain_unlock(struct page *master)
{
	bit_spin_unlock(PG_hydra_chain_locked, (unsigned long *)&master->flags);
}

void hydra_link_page_to_replica_chain(struct page *existing_page,
				      struct page *new_page);
void hydra_break_chain(struct page *page);

struct mmu_gather;
extern void hydra_free_replica_chain(struct page *primary, struct mmu_gather *tlb);
void hydra_free_chain_node_rcu(struct page *page);
void hydra_cache_count_return(struct mm_struct *owner_mm, int node);

bool hydra_try_return_page(struct page *page);
void hydra_dtor_free_page(struct page *page);
void hydra_pagetable_dtor(struct page *page);
bool hydra_cache_return_table(struct page *page);
struct page *hydra_alloc_pt_page_near(struct mm_struct *mm, gfp_t gfp,
				      void *parent);

#include <linux/list.h>
#include <linux/sched.h>

enum hydra_pt_level {
	HYDRA_PT_PGD = 0,
	HYDRA_PT_P4D,
	HYDRA_PT_PUD,
	HYDRA_PT_PMD,
	HYDRA_PT_PTE,
	HYDRA_PT_NR_LEVELS,
};

struct hydra_stats {
	struct list_head list;
	unsigned long id;
	int pid;
	char comm[TASK_COMM_LEN];
	void *mm;
	int master_node;
	int ever_enabled;

	atomic_long_t thp_split;
	atomic_long_t thp_collapse;
	atomic_long_t deposits;
	atomic_long_t withdrawals;

	atomic_long_t master_faults;
	atomic_long_t master_faults_write;
	atomic_long_t master_faults_present;
	atomic_long_t replica_faults;
	atomic_long_t replica_faults_write;
	atomic_long_t replica_faults_present;
	atomic_long_t replica_serviced_on_master;

	atomic_long_t pte_entries_copied;
	atomic_long_t pte_entries_prefetched;
	atomic_long_t pte_copy_faults;
	atomic_long_t pmd_entries_copied;
	atomic_long_t pmd_entries_prefetched;
	atomic_long_t pmd_copy_faults;

	atomic_long_t tlb_shootdowns;
	atomic_long_t tlb_shootdowns_saved;

	atomic_long_t numa_migrate_4k[NUMA_NODE_COUNT][NUMA_NODE_COUNT];
	atomic_long_t numa_migrate_2m[NUMA_NODE_COUNT][NUMA_NODE_COUNT];

	atomic_long_t pt_cur[NUMA_NODE_COUNT][HYDRA_PT_NR_LEVELS];
	atomic_long_t pt_max[NUMA_NODE_COUNT][HYDRA_PT_NR_LEVELS];

	atomic_long_t vma_owner_cur[NUMA_NODE_COUNT];
	atomic_long_t vma_owner_max[NUMA_NODE_COUNT];
};

struct hydra_stats *hydra_stats_attach(struct mm_struct *mm);
void hydra_stats_mark_enabled(struct mm_struct *mm, int master_node);
void hydra_stats_detach(struct mm_struct *mm);
void hydra_pt_account(struct page *page, int delta);
void hydra_vma_attach(struct vm_area_struct *vma);
void hydra_vma_detach(struct vm_area_struct *vma);
void hydra_vma_chown(struct vm_area_struct *vma, int node);
int hydra_status_open(struct inode *inode, struct file *file);
int hydra_history_open(struct inode *inode, struct file *file);

static inline void hydra_stats_copied_pte(struct mm_struct *mm, long copied)
{
	struct hydra_stats *s = mm->hydra_stats;

	if (!s || copied <= 0)
		return;
	atomic_long_add(copied, &s->pte_entries_copied);
	if (copied > 1)
		atomic_long_add(copied - 1, &s->pte_entries_prefetched);
	atomic_long_inc(&s->pte_copy_faults);
}

static inline void hydra_stats_copied_pmd(struct mm_struct *mm, long copied)
{
	struct hydra_stats *s = mm->hydra_stats;

	if (!s || copied <= 0)
		return;
	atomic_long_add(copied, &s->pmd_entries_copied);
	if (copied > 1)
		atomic_long_add(copied - 1, &s->pmd_entries_prefetched);
	atomic_long_inc(&s->pmd_copy_faults);
}

static inline void hydra_stats_tlb(struct mm_struct *mm, long sent,
				   long broadcast)
{
	struct hydra_stats *s = mm->hydra_stats;

	if (!s)
		return;
	if (sent > 0)
		atomic_long_add(sent, &s->tlb_shootdowns);
	if (broadcast > sent)
		atomic_long_add(broadcast - sent, &s->tlb_shootdowns_saved);
}

static inline void hydra_stats_thp_split(struct mm_struct *mm)
{
	if (mm->hydra_stats)
		atomic_long_inc(&mm->hydra_stats->thp_split);
}

static inline void hydra_stats_thp_collapse(struct mm_struct *mm)
{
	if (mm->hydra_stats)
		atomic_long_inc(&mm->hydra_stats->thp_collapse);
}

static inline void hydra_stats_deposit(struct mm_struct *mm)
{
	if (mm->hydra_stats)
		atomic_long_inc(&mm->hydra_stats->deposits);
}

static inline void hydra_stats_withdraw(struct mm_struct *mm)
{
	if (mm->hydra_stats)
		atomic_long_inc(&mm->hydra_stats->withdrawals);
}

static inline void hydra_stats_mark_serviced(struct mm_struct *mm)
{
	if (mm->hydra_stats)
		current->hydra_master_serviced = 1;
}

struct hydra_fault_ctx {
	struct hydra_stats *s;
	bool replica;
	bool write;
	bool present;
	unsigned int saved;
};

static inline struct hydra_fault_ctx hydra_stats_fault_begin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned int flags)
{
	struct hydra_fault_ctx c = { .s = mm->hydra_stats };

	if (c.s) {
		c.replica = mm->lazy_repl_enabled &&
			    (numa_node_id() != vma->master_pgd_node);
		c.write = !!(flags & FAULT_FLAG_WRITE);
		c.present = !!(flags & FAULT_FLAG_PROT);
		if (c.replica) {
			c.saved = current->hydra_master_serviced;
			current->hydra_master_serviced = 0;
		}
	}
	return c;
}

static inline void hydra_stats_fault_end(struct hydra_fault_ctx c)
{
	struct hydra_stats *s = c.s;

	if (!s)
		return;
	if (c.replica) {
		atomic_long_inc(&s->replica_faults);
		if (c.write)
			atomic_long_inc(&s->replica_faults_write);
		if (c.present)
			atomic_long_inc(&s->replica_faults_present);
		if (current->hydra_master_serviced)
			atomic_long_inc(&s->replica_serviced_on_master);
		current->hydra_master_serviced = c.saved;
	} else {
		atomic_long_inc(&s->master_faults);
		if (c.write)
			atomic_long_inc(&s->master_faults_write);
		if (c.present)
			atomic_long_inc(&s->master_faults_present);
	}
}

static inline void hydra_stats_numa(struct mm_struct *mm, bool huge,
				    int from, int to)
{
	struct hydra_stats *s = mm->hydra_stats;

	if (!s)
		return;
	if (from < 0 || from >= NUMA_NODE_COUNT ||
	    to < 0 || to >= NUMA_NODE_COUNT)
		return;
	if (huge)
		atomic_long_inc(&s->numa_migrate_2m[from][to]);
	else
		atomic_long_inc(&s->numa_migrate_4k[from][to]);
}

#endif
