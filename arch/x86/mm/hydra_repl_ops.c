#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/hydra_util.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/hydra_pti.h>

void pgtable_track_set_pgd(pgd_t *pgdp, pgd_t pgd)
{
	pgd_t *user_entry;
	struct mm_struct *mm = NULL;

	native_set_pgd(pgdp, pgd);

	user_entry = hydra_get_user_pgd_entry(pgdp);
	if (user_entry) {
		WRITE_ONCE(*user_entry, __pgd(pgd_val(pgd)));
	}

	if (virt_addr_valid(pgdp))
		mm = virt_to_page(pgdp)->pt_owner_mm;
}

void pgtable_track_set_p4d(p4d_t *p4dp, p4d_t p4d)
{
	pgd_t *user_entry;
	struct mm_struct *mm = NULL;

	native_set_p4d(p4dp, p4d);

	if (!pgtable_l5_enabled()) {
		user_entry = hydra_get_user_pgd_entry((pgd_t *)p4dp);
		if (user_entry) {
			WRITE_ONCE(*user_entry, __pgd(p4d_val(p4d)));
		}
	}

	if (virt_addr_valid(p4dp))
		mm = virt_to_page(p4dp)->pt_owner_mm;
}

void pgtable_track_set_pud(pud_t *pudp, pud_t pud)
{
	struct mm_struct *mm = NULL;

	native_set_pud(pudp, pud);

	if (virt_addr_valid(pudp))
		mm = virt_to_page(pudp)->pt_owner_mm;
}

void pgtable_track_set_pmd(pmd_t *pmdp, pmd_t pmd)
{
	struct page *page;
	struct page *cur;
	struct mm_struct *mm;
	unsigned long offset;

	native_set_pmd(pmdp, pmd);

	if (!virt_addr_valid(pmdp))
		return;

	page = virt_to_page(pmdp);
	mm = page->pt_owner_mm;

	if (!READ_ONCE(page->next_replica)) {
		return;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	if (!pmd_present(pmd)) {
		for_each_replica(page, cur) {
			pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
			pmd_t old_repl = *replica_entry;

			if (pmd_present(old_repl) &&
			    !pmd_trans_huge(old_repl) &&
			    !pmd_devmap(old_repl) &&
			    !pmd_bad(old_repl)) {
				pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_repl);
				struct page *pte_page = virt_to_page(pte_base);
				struct mm_struct *owner_mm = pte_page->pt_owner_mm;

				native_set_pmd(replica_entry, __pmd(0));

				if (owner_mm)
					mm_dec_nr_ptes(owner_mm);
				hydra_defer_pte_page_free(owner_mm, pte_page);
			} else {
				native_set_pmd(replica_entry, __pmd(0));
			}
		}
		return;
	}

	if (pmd_trans_huge(pmd) || pmd_devmap(pmd)) {
		for_each_replica(page, cur) {
			pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
			pmd_t old_repl = *replica_entry;

			if (pmd_present(old_repl) &&
			    !pmd_trans_huge(old_repl) &&
			    !pmd_devmap(old_repl) &&
			    !pmd_bad(old_repl)) {
				pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_repl);
				struct page *pte_page = virt_to_page(pte_base);
				struct mm_struct *owner_mm = pte_page->pt_owner_mm;

				native_set_pmd(replica_entry, __pmd(0));

				if (owner_mm)
					mm_dec_nr_ptes(owner_mm);
				hydra_defer_pte_page_free(owner_mm, pte_page);
			} else {
				pmd_t new_repl = pmd_mkold(pmd);
				native_set_pmd(replica_entry, new_repl);
			}
		}
		return;
	}

	for_each_replica(page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		pmd_t old_repl = *replica_entry;

		if (pmd_present(old_repl) &&
		    (pmd_trans_huge(old_repl) || pmd_devmap(old_repl))) {
			native_set_pmd(replica_entry, __pmd(0));
		} else if (pmd_present(old_repl) &&
			   !pmd_trans_huge(old_repl) &&
			   !pmd_devmap(old_repl) &&
			   !pmd_bad(old_repl)) {
			pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_repl);
			struct page *pte_page = virt_to_page(pte_base);
			struct mm_struct *owner_mm = pte_page->pt_owner_mm;

			native_set_pmd(replica_entry, __pmd(0));

			if (owner_mm)
				mm_dec_nr_ptes(owner_mm);
			hydra_defer_pte_page_free(owner_mm, pte_page);
		}
	}

}

void pgtable_repl_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *start_pte_page;
	struct page *cur;
	struct mm_struct *mm;
	long offset;

	native_set_pte(ptep, pteval);

	if (!virt_addr_valid(ptep))
		return;

	start_pte_page = virt_to_page(ptep);
	mm = start_pte_page->pt_owner_mm;

	if (!READ_ONCE(start_pte_page->next_replica)) {
		return;
	}

	offset = (long)ptep - (long)page_to_virt(start_pte_page);

	if (!pte_present(pteval)) {
		for_each_replica(start_pte_page, cur) {
			pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
			native_set_pte(rp, __pte(0));
		}
	} else {
		for_each_replica(start_pte_page, cur) {
			pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
			pte_t repl_val = pte_mkold(pteval);
			native_set_pte(rp, repl_val);
		}
	}

}

pte_t pgtable_repl_get_pte(pte_t *ptep)
{
	struct page *pte_page;
	struct page *cur;
	struct mm_struct *mm;
	unsigned long offset;
	pte_t master_pte;
	pteval_t extra_flags;

	if (!ptep)
		return __pte(0);

	pte_page = virt_to_page(ptep);
	master_pte = *ptep;
	mm = pte_page->pt_owner_mm;

	if (!READ_ONCE(pte_page->next_replica)) {
		return master_pte;
	}

	if (!pte_present(master_pte)) {
		return master_pte;
	}

	extra_flags = 0;
	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	for_each_replica(pte_page, cur) {
		pte_t *replica_pte = (pte_t *)(page_address(cur) + offset);
		pte_t replica_val = *replica_pte;

		if (pte_present(replica_val)) {
			extra_flags |= pte_flags(replica_val);
		}
	}


	return pte_set_flags(master_pte, extra_flags);
}

pte_t ptep_get_and_clear(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	struct page *start_pte_page;
	struct page *cur;
	long offset;
	pteval_t pteval;
	pteval_t flags;

	start_pte_page = virt_to_page(ptep);

	flags = pte_flags(*ptep);
	pteval = pte_val(native_ptep_get_and_clear(ptep));

	if (!mm || !mm->lazy_repl_enabled) {
		page_table_check_pte_clear(mm, addr, native_make_pte(pteval));
		return native_make_pte(pteval);
	}

	if (!READ_ONCE(start_pte_page->next_replica)) {
		page_table_check_pte_clear(mm, addr, native_make_pte(pteval));
		return native_make_pte(pteval);
	}

	offset = (long)ptep - (long)page_to_virt(start_pte_page);

	for_each_replica(start_pte_page, cur) {
		pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
		pte_t repl_pte = native_ptep_get_and_clear(rp);

		if (pte_present(repl_pte)) {
			flags |= pte_flags(repl_pte);
		}
	}


	page_table_check_pte_clear(mm, addr, native_make_pte(pteval));
	return pte_set_flags(native_make_pte(pteval), flags);
}

void ptep_set_wrprotect(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	struct page *start_pte_page;
	struct page *cur;
	long offset;

	start_pte_page = virt_to_page(ptep);

	clear_bit(_PAGE_BIT_RW, (unsigned long *)&ptep->pte);

	if (!mm || !mm->lazy_repl_enabled) {
		return;
	}

	if (!READ_ONCE(start_pte_page->next_replica)) {
		return;
	}

	offset = (long)ptep - (long)page_to_virt(start_pte_page);

	for_each_replica(start_pte_page, cur) {
		pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
		if (pte_present(*rp)) {
			clear_bit(_PAGE_BIT_RW, (unsigned long *)&rp->pte);
		}
	}

}

pmd_t pmdp_huge_get_and_clear(struct mm_struct *mm, unsigned long addr,
			      pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur;
	unsigned long offset;
	pmdval_t val;
	pmdval_t flags;

	val = pmd_val(native_pmdp_get_and_clear(pmdp));
	flags = pmd_flags(__pmd(val));

	if (!virt_addr_valid(pmdp)) {
		page_table_check_pmd_clear(mm, addr, __pmd(val));
		return __pmd(val);
	}

	pmd_page = virt_to_page(pmdp);

	if (!READ_ONCE(pmd_page->next_replica)) {
		page_table_check_pmd_clear(mm, addr, __pmd(val));
		return __pmd(val);
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		pmd_t old_entry = native_pmdp_get_and_clear(replica_entry);

		if (pmd_present(old_entry) &&
		    !pmd_trans_huge(old_entry) &&
		    !pmd_devmap(old_entry) &&
		    !pmd_bad(old_entry)) {
			pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_entry);
			struct page *pte_page = virt_to_page(pte_base);
			struct mm_struct *owner_mm = pte_page->pt_owner_mm;

			if (owner_mm)
				mm_dec_nr_ptes(owner_mm);
			hydra_defer_pte_page_free(owner_mm, pte_page);
		} else if (pmd_trans_huge(old_entry) || pmd_devmap(old_entry)) {
			flags |= pmd_flags(old_entry);
		}
	}


	page_table_check_pmd_clear(mm, addr, __pmd(val));
	return pmd_set_flags(__pmd(val), flags);
}

void pmdp_set_wrprotect(struct mm_struct *mm,
			unsigned long addr, pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur;
	unsigned long offset;

	clear_bit(_PAGE_BIT_RW, (unsigned long *)pmdp);

	if (!virt_addr_valid(pmdp)) {
		return;
	}

	pmd_page = virt_to_page(pmdp);

	if (!READ_ONCE(pmd_page->next_replica)) {
		return;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);

		if (pmd_present(*replica_entry) && pmd_trans_huge(*replica_entry)) {
			clear_bit(_PAGE_BIT_RW, (unsigned long *)replica_entry);
		}
	}

}

pmd_t hydra_pmdp_establish(pmd_t *pmdp, pmd_t pmd)
{
	struct page *pmd_page;
	struct page *cur;
	struct mm_struct *mm;
	unsigned long offset;
	pmdval_t val;
	pmdval_t flags;
	bool propagate;
	pmd_t repl_val;

	if (IS_ENABLED(CONFIG_SMP))
		val = pmd_val(xchg(pmdp, pmd));
	else {
		val = pmd_val(*pmdp);
		WRITE_ONCE(*pmdp, pmd);
	}

	flags = pmd_flags(__pmd(val));

	if (!virt_addr_valid(pmdp))
		return __pmd(val);

	pmd_page = virt_to_page(pmdp);
	mm = pmd_page->pt_owner_mm;

	if (!READ_ONCE(pmd_page->next_replica)) {
		return __pmd(val);
	}

	propagate = !pmd_present(pmd) ||
		    pmd_trans_huge(pmd) ||
		    pmd_devmap(pmd);

	if (!propagate) {
		offset = ((unsigned long)pmdp) & ~PAGE_MASK;

		for_each_replica(pmd_page, cur) {
			pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
			pmd_t old_repl = *replica_entry;

			if (pmd_present(old_repl) &&
			    (pmd_trans_huge(old_repl) || pmd_devmap(old_repl))) {
				native_set_pmd(replica_entry, __pmd(0));
			}
		}


		return __pmd(val);
	}

	if (pmd_val(pmd) & _PAGE_PRESENT)
		repl_val = pmd_mkold(pmd);
	else
		repl_val = __pmd(0);

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		pmd_t old_repl;

		if (IS_ENABLED(CONFIG_SMP))
			old_repl = __pmd(pmd_val(xchg(replica_entry, repl_val)));
		else {
			old_repl = *replica_entry;
			WRITE_ONCE(*replica_entry, repl_val);
		}

		if (pmd_present(old_repl) &&
		    !pmd_trans_huge(old_repl) &&
		    !pmd_devmap(old_repl) &&
		    !pmd_bad(old_repl)) {
			pte_t *pte_base = (pte_t *)pmd_page_vaddr(old_repl);
			struct page *pte_page = virt_to_page(pte_base);
			struct mm_struct *owner_mm = pte_page->pt_owner_mm;

			if (owner_mm)
				mm_dec_nr_ptes(owner_mm);
			hydra_defer_pte_page_free(owner_mm, pte_page);
		} else if (pmd_trans_huge(old_repl) || pmd_devmap(old_repl)) {
			flags |= pmd_flags(old_repl);
		}

	}


	return pmd_set_flags(__pmd(val), flags);
}

pmd_t hydra_get_pmd(pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur;
	struct mm_struct *mm;
	unsigned long offset;
	pmdval_t val;

	if (!pmdp)
		return __pmd(0);

	if (!virt_addr_valid(pmdp))
		return *pmdp;

	pmd_page = virt_to_page(pmdp);
	mm = pmd_page->pt_owner_mm;

	if (!READ_ONCE(pmd_page->next_replica)) {
		return *pmdp;
	}

	val = pmd_val(*pmdp);

	if (!pmd_present(__pmd(val)) || !pmd_trans_huge(__pmd(val))) {
		return __pmd(val);
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		pmd_t replica_val = *replica_entry;

		if (pmd_present(replica_val) && pmd_trans_huge(replica_val)) {
			val |= pmd_flags(replica_val);
		}
	}


	return __pmd(val);
}
EXPORT_SYMBOL(hydra_get_pmd);

int ptep_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pte_t *ptep)
{
	struct page *start_pte_page;
	struct page *cur;
	long offset;
	int ret = 0;

	start_pte_page = virt_to_page(ptep);

	if (pte_young(*ptep))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)&ptep->pte);

	if (!READ_ONCE(start_pte_page->next_replica)) {
		return ret;
	}

	offset = (long)ptep - (long)page_to_virt(start_pte_page);

	for_each_replica(start_pte_page, cur) {
		pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
		if (pte_present(*rp) && pte_young(*rp)) {
			ret |= test_and_clear_bit(_PAGE_BIT_ACCESSED,
						  (unsigned long *) &rp->pte);
		}
	}


	return ret;
}

int pmdp_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur;
	unsigned long offset;
	int ret = 0;

	if (pmd_young(*pmdp))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)pmdp);

	if (!virt_addr_valid(pmdp)) {
		return ret;
	}

	pmd_page = virt_to_page(pmdp);

	if (!READ_ONCE(pmd_page->next_replica)) {
		return ret;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);

		if (pmd_present(*replica_entry) && pmd_trans_huge(*replica_entry)) {
			if (pmd_young(*replica_entry)) {
				if (test_and_clear_bit(_PAGE_BIT_ACCESSED,
						       (unsigned long *)replica_entry))
					ret = 1;
			}
		}
	}


	return ret;
}
