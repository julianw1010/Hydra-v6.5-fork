#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/page-flags.h>
#include <linux/nodemask.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>
#include <linux/hugetlb.h>
#include <linux/hydra_util.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include <asm/hydra_pti.h>

static struct page *migrate_pgtable_page(struct page *page, int target_node)
{
	struct page *new_page;
	void *old_addr, *new_addr;

	if (page_to_nid(page) == target_node)
		return NULL;

	new_page = hydra_cache_pop(target_node);
	if (new_page) {
		new_page->next_replica = NULL;
		new_page->pt_owner_mm = page->pt_owner_mm;
		new_page->mitosis_tracking = NULL;
	} else {
		new_page = alloc_pages_node(target_node,
					    GFP_KERNEL_ACCOUNT | __GFP_ZERO,
					    0);
		if (!new_page) {
			pr_warn("[HYDRA]: migrate_pgtable_page: alloc failed on node %d\n",
				target_node);
			return NULL;
		}

		new_page->next_replica = NULL;
		new_page->pt_owner_mm = page->pt_owner_mm;
		new_page->mitosis_tracking = NULL;
	}

	old_addr = page_address(page);
	new_addr = page_address(new_page);
	memcpy(new_addr, old_addr, PAGE_SIZE);

	return new_page;
}

static void migrate_pte_pages(struct mm_struct *mm, pmd_t *pmd,
			      unsigned long addr, unsigned long end,
			      int target_node)
{
	pte_t *old_pte, *new_pte;
	struct page *old_page, *new_page;
	spinlock_t *ptl;
	pmd_t old_pmd;
	int old_node;

	old_pmd = *pmd;
	if (pmd_none(old_pmd) || pmd_trans_huge(old_pmd))
		return;

	old_pte = pte_offset_map(pmd, addr);
	if (!old_pte)
		return;

	old_page = virt_to_page(old_pte);
	pte_unmap(old_pte);

	old_node = page_to_nid(old_page);
	if (old_node == target_node)
		return;

	new_page = migrate_pgtable_page(old_page, target_node);
	if (!new_page)
		return;

	if (!pgtable_pte_page_ctor(new_page)) {
		printk("[HYDRA]: migrate_pte_pages: pte page ctor failed\n");
		BUG();
	}

	ptl = pmd_lock(mm, pmd);

	if (!pmd_same(*pmd, old_pmd)) {
		printk("[HYDRA]: migrate_pte_pages: pmd changed\n");
		BUG();
	}

	new_pte = (pte_t *)page_address(new_page);
	pmd_populate(mm, pmd, new_page);

	spin_unlock(ptl);

	pgtable_pte_page_dtor(old_page);
	__free_page(old_page);
}

static void migrate_pmd_pages(struct mm_struct *mm, pud_t *pud,
			      unsigned long addr, unsigned long end,
			      int target_node)
{
	pmd_t *old_pmd, *new_pmd;
	struct page *old_page, *new_page;
	spinlock_t *ptl;
	pud_t old_pud;
	unsigned long next;
	int old_node;

	old_pud = *pud;
	if (pud_none(old_pud) || pud_huge(old_pud))
		return;

	old_pmd = pmd_offset(pud, addr);
	old_page = virt_to_page(old_pmd);

	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*old_pmd) || pmd_trans_huge(*old_pmd))
			continue;

		migrate_pte_pages(mm, old_pmd, addr, next, target_node);

	} while (old_pmd++, addr = next, addr != end);

	old_node = page_to_nid(old_page);
	if (old_node == target_node)
		return;

	new_page = migrate_pgtable_page(old_page, target_node);
	if (!new_page)
		return;

	if (!pgtable_pmd_page_ctor(new_page)) {
		printk("[HYDRA]: migrate_pmd_pages: pmd page ctor failed\n");
		BUG();
	}

	new_page->pmd_huge_pte = old_page->pmd_huge_pte;
	old_page->pmd_huge_pte = NULL;

	ptl = pud_lock(mm, pud);

	if (!pud_same(*pud, old_pud)) {
		printk("[HYDRA]: migrate_pmd_pages: pud changed\n");
		BUG();
	}

	new_pmd = (pmd_t *)page_address(new_page);
	pud_populate(mm, pud, new_pmd);

	spin_unlock(ptl);

	pgtable_pmd_page_dtor(old_page);
	__free_page(old_page);
}

static void migrate_pud_pages(struct mm_struct *mm, p4d_t *p4d,
			      unsigned long addr, unsigned long end,
			      int target_node)
{
	pud_t *old_pud, *new_pud;
	struct page *old_page, *new_page;
	p4d_t old_p4d;
	unsigned long next;
	int old_node;

	old_p4d = *p4d;
	if (p4d_none(old_p4d))
		return;

	old_pud = pud_offset(p4d, addr);
	old_page = virt_to_page(old_pud);

	do {
		next = pud_addr_end(addr, end);
		if (pud_none(*old_pud) || pud_huge(*old_pud))
			continue;

		migrate_pmd_pages(mm, old_pud, addr, next, target_node);

	} while (old_pud++, addr = next, addr != end);

	old_node = page_to_nid(old_page);
	if (old_node == target_node)
		return;

	new_page = migrate_pgtable_page(old_page, target_node);
	if (!new_page)
		return;

	spin_lock(&mm->page_table_lock);

	if (!p4d_same(*p4d, old_p4d)) {
		printk("[HYDRA]: migrate_pud_pages: p4d changed\n");
		BUG();
	}

	new_pud = (pud_t *)page_address(new_page);

	if (pgtable_l5_enabled())
		p4d_populate(mm, p4d, new_pud);
	else
		set_pgd((pgd_t *)p4d, __pgd(_PAGE_TABLE | __pa(new_pud)));

	spin_unlock(&mm->page_table_lock);

	__free_page(old_page);
}

static void migrate_p4d_pages(struct mm_struct *mm, pgd_t *pgd,
			      unsigned long addr, unsigned long end,
			      int target_node)
{
	p4d_t *old_p4d, *new_p4d;
	struct page *old_page, *new_page;
	pgd_t old_pgd;
	unsigned long next;
	int old_node;

	old_pgd = *pgd;
	if (pgd_none(old_pgd))
		return;

	old_p4d = p4d_offset(pgd, addr);

	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none(*old_p4d))
			continue;

		migrate_pud_pages(mm, old_p4d, addr, next, target_node);

	} while (old_p4d++, addr = next, addr != end);

	if (!pgtable_l5_enabled())
		return;

	old_page = virt_to_page(p4d_offset(pgd, 0));

	old_node = page_to_nid(old_page);
	if (old_node == target_node)
		return;

	new_page = migrate_pgtable_page(old_page, target_node);
	if (!new_page)
		return;

	spin_lock(&mm->page_table_lock);

	if (!pgd_same(*pgd, old_pgd)) {
		printk("[HYDRA]: migrate_p4d_pages: pgd changed\n");
		BUG();
	}

	new_p4d = (p4d_t *)page_address(new_page);
	pgd_populate(mm, pgd, new_p4d);

	spin_unlock(&mm->page_table_lock);

	__free_page(old_page);
}

void migrate_pgtables_to_node(struct mm_struct *mm, pgd_t *pgd,
			      int target_node)
{
	unsigned long addr, next, end;
	pgd_t *pgdp;

	addr = 0;
	end = TASK_SIZE;
	pgdp = pgd;

	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgdp) || pgd_bad(*pgdp))
			goto next;

		migrate_p4d_pages(mm, pgdp, addr, next, target_node);

next:
		pgdp++;
		addr = next;
	} while (addr != end);

	flush_tlb_mm(mm);
}

void hydra_reload_cr3(void *info)
{
	struct mm_struct *mm = info;
	if (this_cpu_read(cpu_tlbstate.loaded_mm) == mm) {
		int node = numa_node_id();
		unsigned long new_cr3 = __pa(mm->repl_pgd[node]);
		write_cr3(new_cr3);
	}
}

int hydra_enable_replication(struct mm_struct *mm)
{
	int i, primary_node;

	if (mm->lazy_repl_enabled)
		return 0;

	mmap_write_lock(mm);

	if (mm->lazy_repl_enabled) {
		mmap_write_unlock(mm);
		return 0;
	}

	primary_node = page_to_nid(virt_to_page(mm->pgd));

	{
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			WRITE_ONCE(vma->master_pgd_node, primary_node);
		}
	}

	migrate_pgtables_to_node(mm, mm->pgd, primary_node);

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (i == primary_node) {
			mm->repl_pgd[i] = mm->pgd;
		} else {
			mm->repl_pgd[i] = repl_pgd_alloc(mm, i);
			if (!mm->repl_pgd[i])
				mm->repl_pgd[i] = mm->pgd;
		}
	}

	WRITE_ONCE(mm->lazy_repl_enabled, true);
	smp_wmb();
	on_each_cpu_mask(mm_cpumask(mm), hydra_reload_cr3, mm, 1);
	smp_mb();
	flush_tlb_mm(mm);

	{
		int count = 0;
		for (i = 0; i < NUMA_NODE_COUNT; i++) {
			if (mm->repl_pgd[i] && mm->repl_pgd[i] != mm->pgd)
				count++;
		}
		count++;
		printk(KERN_INFO "HYDRA: Enabled page table replication for mm %px on %d nodes\n", mm, count);
	}

	mmap_write_unlock(mm);

	return 0;
}
EXPORT_SYMBOL(hydra_enable_replication);

static long kernel_set_pgtlbreplpolicy(int mode, const unsigned long __user *nmask,
				       unsigned long maxnode)
{
	if (!mode) {
		printk("[HYDRA]: Disabling page table replication is not supported.\n");
		return -EOPNOTSUPP;
	}

	return hydra_enable_replication(current->mm);
}

static int kernel_get_pgtlbreplpolicy(int __user *policy,
				      unsigned long __user *nmask,
				      unsigned long maxnode,
				      unsigned long addr,
				      unsigned long flags)
{
	int err = 0;

	printk("get info from numactl");

	return err;
}

SYSCALL_DEFINE3(set_pgtblreplpolicy, int, mode, const unsigned long __user *, nmask,
		unsigned long, maxnode)
{
	return kernel_set_pgtlbreplpolicy(mode, nmask, maxnode);
}

SYSCALL_DEFINE5(get_pgtblreplpolicy, int __user *, policy,
		unsigned long __user *, nmask, unsigned long, maxnode,
		unsigned long, addr, unsigned long, flags)
{
	return kernel_get_pgtlbreplpolicy(policy, nmask, maxnode, addr, flags);
}
