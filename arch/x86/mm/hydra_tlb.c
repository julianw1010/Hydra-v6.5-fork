#include <linux/mm.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <asm/tlb.h>

void flush_tlb_vma_range(struct vm_area_struct *vma, unsigned long start,
			 unsigned long end, unsigned int stride_shift,
			 bool freed_tables)
{
	struct mm_struct *mm = vma->vm_mm;
	nodemask_t nodemask;
	int master_node;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if (!mm->lazy_repl_enabled || !sysctl_hydra_tlbflush_opt)
		goto broadcast;

	master_node = vma->master_pgd_node;

	pgd = pgd_offset_node(mm, start, master_node);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		goto broadcast;

	p4d = p4d_offset(pgd, start);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		goto broadcast;

	pud = pud_offset(p4d, start);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		goto broadcast;

	pmd = pmd_offset(pud, start);
	if (pmd_none(*pmd))
		goto broadcast;

	nodes_clear(nodemask);

	if (!pmd_trans_huge(*pmd)) {
		if (unlikely(pmd_bad(*pmd)))
			goto broadcast;

		if ((start & PMD_MASK) == ((end - 1) & PMD_MASK)) {
			pte = pte_offset_kernel(pmd, start);
			hydra_collect_repl_nodes(virt_to_page(pte), &nodemask);
			goto do_flush;
		}
	}

	if ((start & PUD_MASK) != ((end - 1) & PUD_MASK))
		goto broadcast;
	hydra_collect_repl_nodes(virt_to_page(pmd), &nodemask);

do_flush:
	flush_tlb_mm_node_range(mm, start, end, stride_shift,
				freed_tables, &nodemask);
	return;

broadcast:
	flush_tlb_mm_node_range(mm, start, end, stride_shift,
				freed_tables, NULL);
}
