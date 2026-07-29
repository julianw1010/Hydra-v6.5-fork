#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/spinlock.h>
#include <linux/jump_label.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/page.h>

DEFINE_STATIC_KEY_FALSE(hydra_repl_ever_enabled);

static void hydra_wrprotect_pte_one(pte_t *ptep)
{
	pte_t old_pte, new_pte;

	old_pte = READ_ONCE(*ptep);
	do {
		new_pte = pte_wrprotect(old_pte);
	} while (!try_cmpxchg((long *)&ptep->pte, (long *)&old_pte, *(long *)&new_pte));
}

static void hydra_wrprotect_pmd_one(pmd_t *pmdp)
{
	pmd_t old_pmd, new_pmd;

	old_pmd = READ_ONCE(*pmdp);
	do {
		new_pmd = pmd_wrprotect(old_pmd);
	} while (!try_cmpxchg((long *)pmdp, (long *)&old_pmd, *(long *)&new_pmd));
}

static long hydra_set_wrprotect_pte_entry(pte_t *ptep)
{
	struct page *page, *cur;
	unsigned long offset;
	long pages = 1;

	if (!ptep)
		return 0;

	hydra_wrprotect_pte_one(ptep);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled))
		return pages;

	if (!virt_addr_valid(ptep))
		return pages;

	page = virt_to_page(ptep);

	if (!READ_ONCE(page->next_replica))
		return pages;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		pte_t *replica_entry =
			(pte_t *)(page_address(cur) + offset);
		if (pte_val(READ_ONCE(*replica_entry)) & _PAGE_PRESENT)
			hydra_wrprotect_pte_one(replica_entry);
		pages++;
	}
	rcu_read_unlock();

	return pages;
}

static long hydra_set_wrprotect_pmd_entry(pmd_t *pmdp)
{
	struct page *page, *cur;
	unsigned long offset;
	long pages = 1;

	if (!pmdp)
		return 0;

	hydra_wrprotect_pmd_one(pmdp);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled))
		return pages;

	if (!virt_addr_valid(pmdp))
		return pages;

	page = virt_to_page(pmdp);

	if (!READ_ONCE(page->next_replica))
		return pages;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		pmd_t *replica_entry =
			(pmd_t *)(page_address(cur) + offset);
		if (pmd_val(READ_ONCE(*replica_entry)) & _PAGE_PRESENT)
			hydra_wrprotect_pmd_one(replica_entry);
		pages++;
	}
	rcu_read_unlock();

	return pages;
}

void hydra_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *pte_page, *cur;
	unsigned long offset;
	pte_t repl_val;
	long pages = 1;

	native_set_pte(ptep, pteval);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled))
		return;

	if (!virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);

	if (READ_ONCE(pte_page->next_replica)) {
		offset = ((unsigned long)ptep) & ~PAGE_MASK;
		repl_val = (pte_val(pteval) & _PAGE_PRESENT) ? pteval : __pte(0);

		rcu_read_lock();
		for (cur = pte_page->next_replica; cur && cur != pte_page; cur = cur->next_replica) {
			pte_t *rp = (pte_t *)(page_address(cur) + offset);
			native_set_pte(rp, repl_val);
			pages++;
		}
		rcu_read_unlock();
	}

	hydra_stats_pt_write(ptep, HYDRA_PT_PTE, pages);
}

pte_t hydra_get_pte(pte_t *ptep)
{
	struct page *page, *cur;
	unsigned long offset;
	pte_t val;

	if (!ptep)
		return __pte(0);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled))
		return *ptep;

	if (!virt_addr_valid(ptep))
		return *ptep;

	page = virt_to_page(ptep);

	if (!READ_ONCE(page->next_replica))
		return *ptep;

	val = *ptep;
	if (!(pte_val(val) & _PAGE_PRESENT))
		return val;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		pte_t rv = *(pte_t *)(page_address(cur) + offset);
		if (pte_val(rv) & _PAGE_PRESENT)
			val = __pte(pte_val(val) | (pte_val(rv) & PTE_FLAGS_MASK));
	}
	rcu_read_unlock();

	return val;
}

pte_t hydra_ptep_get_and_clear(struct mm_struct *mm, pte_t *ptep)
{
	struct page *page, *cur;
	unsigned long offset;
	pte_t val;
	long pages = 1;

	if (!ptep)
		return __pte(0);

	val = native_ptep_get_and_clear(ptep);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled) ||
	    !virt_addr_valid(ptep))
		goto out;

	page = virt_to_page(ptep);

	if (!READ_ONCE(page->next_replica))
		goto out;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		pte_t old = native_ptep_get_and_clear(
			(pte_t *)(page_address(cur) + offset));
		if (pte_val(old) & _PAGE_PRESENT)
			val = __pte(pte_val(val) | (pte_val(old) & PTE_FLAGS_MASK));
		pages++;
	}
	rcu_read_unlock();

out:
	hydra_stats_pt_write(ptep, HYDRA_PT_PTE, pages);
	return val;
}

void hydra_ptep_set_wrprotect(struct mm_struct *mm,
			      unsigned long addr, pte_t *ptep)
{
	long pages = hydra_set_wrprotect_pte_entry(ptep);

	hydra_stats_pt_write(ptep, HYDRA_PT_PTE, pages);
}

int hydra_ptep_test_and_clear_young(struct vm_area_struct *vma,
				    unsigned long addr, pte_t *ptep)
{
	struct page *page, *cur;
	unsigned long offset;
	int young = 0;
	long pages = 1;

	if (!ptep)
		return 0;

	if (pte_young(*ptep))
		young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					   (unsigned long *)&ptep->pte);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled) ||
	    !virt_addr_valid(ptep))
		goto out;

	page = virt_to_page(ptep);

	if (!READ_ONCE(page->next_replica))
		goto out;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		pte_t *rp = (pte_t *)(page_address(cur) + offset);
		pte_t rv = *rp;

		if ((pte_val(rv) & _PAGE_PRESENT) && pte_young(rv)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED,
					       (unsigned long *)&rp->pte))
				young = 1;
		}
		pages++;
	}
	rcu_read_unlock();

out:
	hydra_stats_pt_write(ptep, HYDRA_PT_PTE, pages);
	return young;
}

void hydra_set_pmd(pmd_t *pmdp, pmd_t pmd)
{
	struct page *page, *cur;
	unsigned long offset;
	pmd_t repl_val;
	long pages = 1;

	native_set_pmd(pmdp, pmd);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled))
		return;

	if (!virt_addr_valid(pmdp))
		return;

	page = virt_to_page(pmdp);

	if (READ_ONCE(page->next_replica)) {
		offset = ((unsigned long)pmdp) & ~PAGE_MASK;

		if ((pmd_flags(pmd) & _PAGE_PRESENT) &&
		    (pmd_trans_huge(pmd) || pmd_leaf(pmd)))
			repl_val = pmd;
		else
			repl_val = __pmd(0);

		rcu_read_lock();
		for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
			pmd_t *rp = (pmd_t *)(page_address(cur) + offset);
			native_set_pmd(rp, repl_val);
			pages++;
		}
		rcu_read_unlock();
	}

	hydra_stats_pt_write(pmdp, HYDRA_PT_PMD, pages);
}

pmd_t hydra_get_pmd(pmd_t *pmdp)
{
	struct page *page, *cur;
	unsigned long offset;
	pmd_t val;

	if (!pmdp)
		return __pmd(0);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled))
		return *pmdp;

	if (!virt_addr_valid(pmdp))
		return *pmdp;

	page = virt_to_page(pmdp);

	if (!READ_ONCE(page->next_replica))
		return *pmdp;

	val = *pmdp;
	if (!(pmd_val(val) & _PAGE_PRESENT))
		return val;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		pmd_t rv = *(pmd_t *)(page_address(cur) + offset);
		if (pmd_val(rv) & _PAGE_PRESENT)
			val = __pmd(pmd_val(val) | (pmd_val(rv) & PTE_FLAGS_MASK));
	}
	rcu_read_unlock();

	return val;
}

pmd_t hydra_pmdp_get_and_clear(struct mm_struct *mm, pmd_t *pmdp)
{
	struct page *page, *cur;
	unsigned long offset;
	pmd_t val;
	long pages = 1;

	if (!pmdp)
		return __pmd(0);

	val = native_pmdp_get_and_clear(pmdp);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled) ||
	    !virt_addr_valid(pmdp))
		goto out;

	page = virt_to_page(pmdp);

	if (!READ_ONCE(page->next_replica))
		goto out;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		pmd_t old = native_pmdp_get_and_clear(
			(pmd_t *)(page_address(cur) + offset));
		if (pmd_val(old) & _PAGE_PRESENT)
			val = __pmd(pmd_val(val) | (pmd_val(old) & PTE_FLAGS_MASK));
		pages++;
	}
	rcu_read_unlock();

out:
	hydra_stats_pt_write(pmdp, HYDRA_PT_PMD, pages);
	return val;
}

void hydra_pmdp_set_wrprotect(struct mm_struct *mm,
			      unsigned long addr, pmd_t *pmdp)
{
	long pages = hydra_set_wrprotect_pmd_entry(pmdp);

	hydra_stats_pt_write(pmdp, HYDRA_PT_PMD, pages);
}

int hydra_pmdp_test_and_clear_young(struct vm_area_struct *vma,
				    unsigned long addr, pmd_t *pmdp)
{
	struct page *page, *cur;
	unsigned long offset;
	int young = 0;
	long pages = 1;

	if (!pmdp)
		return 0;

	if (pmd_young(*pmdp))
		young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					   (unsigned long *)pmdp);

	if (!static_branch_unlikely(&hydra_repl_ever_enabled) ||
	    !virt_addr_valid(pmdp))
		goto out;

	page = virt_to_page(pmdp);

	if (!READ_ONCE(page->next_replica))
		goto out;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		pmd_t *rp = (pmd_t *)(page_address(cur) + offset);
		pmd_t rv = *rp;

		if ((pmd_val(rv) & _PAGE_PRESENT) && pmd_young(rv)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED,
					       (unsigned long *)rp))
				young = 1;
		}
		pages++;
	}
	rcu_read_unlock();

out:
	hydra_stats_pt_write(pmdp, HYDRA_PT_PMD, pages);
	return young;
}

pmd_t hydra_pmdp_establish(pmd_t *pmdp, pmd_t pmd)
{
	struct page *pmd_page, *cur;
	unsigned long offset;
	pmd_t old, repl_val;
	pmdval_t flags = 0;
	long pages = 1;

	if (IS_ENABLED(CONFIG_SMP))
		old = xchg(pmdp, pmd);
	else {
		old = *pmdp;
		WRITE_ONCE(*pmdp, pmd);
	}

	if (!static_branch_unlikely(&hydra_repl_ever_enabled) ||
	    !virt_addr_valid(pmdp))
		goto out;

	pmd_page = virt_to_page(pmdp);

	if (!READ_ONCE(pmd_page->next_replica))
		goto out;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	if ((pmd_flags(pmd) & _PAGE_PRESENT) &&
	    (pmd_trans_huge(pmd) || pmd_leaf(pmd)))
		repl_val = pmd;
	else
		repl_val = __pmd(0);

	rcu_read_lock();
	for (cur = pmd_page->next_replica; cur && cur != pmd_page; cur = cur->next_replica) {
		pmd_t *rp = (pmd_t *)(page_address(cur) + offset);
		pmd_t old_repl;

		if (IS_ENABLED(CONFIG_SMP))
			old_repl = xchg(rp, repl_val);
		else {
			old_repl = *rp;
			WRITE_ONCE(*rp, repl_val);
		}

		if (pmd_trans_huge(old_repl) || pmd_leaf(old_repl))
			flags |= pmd_flags(old_repl);
		pages++;
	}
	rcu_read_unlock();

	old = pmd_set_flags(old, flags);

out:
	hydra_stats_pt_write(pmdp, HYDRA_PT_PMD, pages);
	return old;
}

bool hydra_move_normal_pmd(struct vm_area_struct *vma, unsigned long old_addr,
			   pmd_t *old_pmd, pmd_t *new_pmd)
{
	struct mm_struct *mm = vma->vm_mm;
	struct page *master_page = virt_to_page(old_pmd);
	unsigned long old_off = (unsigned long)old_pmd & ~PAGE_MASK;
	unsigned long new_off = (unsigned long)new_pmd & ~PAGE_MASK;
	spinlock_t *ptl;
	struct page *cur;
	pmd_t pmd;
	bool res = false;

	ptl = pmd_lock(mm, old_pmd);

	pmd = *old_pmd;
	if (unlikely(!pmd_present(pmd) || pmd_leaf(pmd)))
		goto out_unlock;

	rcu_read_lock();
	cur = master_page;
	do {
		pmd_t *k_old = (pmd_t *)(page_address(cur) + old_off);
		pmd_t *k_new = (pmd_t *)(page_address(cur) + new_off);
		pmd_t v = *k_old;

		native_set_pmd(k_new, v);
		native_set_pmd(k_old, __pmd(0));

		cur = READ_ONCE(cur->next_replica);
	} while (cur && cur != master_page);
	rcu_read_unlock();

	res = true;
	flush_tlb_vma_range(vma, old_addr, old_addr + PMD_SIZE, PAGE_SHIFT, true);

out_unlock:
	spin_unlock(ptl);
	return res;
}

void hydra_set_pud(pud_t *pudp, pud_t pudval)
{
	hydra_stats_pt_write(pudp, HYDRA_PT_PUD, 1);
	native_set_pud(pudp, pudval);
}

void hydra_set_p4d(p4d_t *p4dp, p4d_t p4dval)
{
	hydra_stats_pt_write(p4dp, pgtable_l5_enabled() ?
			     HYDRA_PT_P4D : HYDRA_PT_PGD, 1);
	native_set_p4d(p4dp, p4dval);
}

void hydra_set_pgd(pgd_t *pgdp, pgd_t pgdval)
{
	hydra_stats_pt_write(pgdp, HYDRA_PT_PGD, 1);
	native_set_pgd(pgdp, pgdval);
}
