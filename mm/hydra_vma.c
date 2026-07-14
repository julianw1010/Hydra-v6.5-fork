#include <linux/mm.h>
#include <linux/xarray.h>
#include <linux/hydra.h>
#include "internal.h"

void hydra_pud_owner_claim(struct mm_struct *mm, unsigned long start,
			   unsigned long end, int node)
{
	unsigned long idx;
	void *old;

	for (idx = start >> PUD_SHIFT; idx <= (end - 1) >> PUD_SHIFT; idx++) {
		old = xa_cmpxchg(mm->hydra_pud_owner, idx, NULL,
				 xa_mk_value(node), GFP_KERNEL);
		if (xa_is_err(old)) {
			pr_emerg("HYDRA: pud owner claim failed for mm %px pud index %lx node %d\n",
				 mm, idx, node);
			BUG();
		}
	}
}

unsigned long hydra_gup_fast_end(struct mm_struct *mm, unsigned long start,
				 unsigned long end)
{
	void *entry = xa_load(mm->hydra_pud_owner, start >> PUD_SHIFT);
	unsigned long owner, addr;

	if (!entry)
		return start;

	owner = xa_to_value(entry);
	addr = (start & PUD_MASK) + PUD_SIZE;
	while (addr < end) {
		entry = xa_load(mm->hydra_pud_owner, addr >> PUD_SHIFT);
		if (!entry || xa_to_value(entry) != owner)
			return addr;
		addr += PUD_SIZE;
	}

	return end;
}

void hydra_pud_owner_stamp(struct mm_struct *mm, unsigned long start,
			   unsigned long end, int node)
{
	unsigned long idx;
	void *old;

	for (idx = start >> PUD_SHIFT; idx <= (end - 1) >> PUD_SHIFT; idx++) {
		old = xa_store(mm->hydra_pud_owner, idx, xa_mk_value(node),
			       GFP_KERNEL);
		if (xa_is_err(old)) {
			pr_emerg("HYDRA: pud owner stamp failed for mm %px pud index %lx node %d\n",
				 mm, idx, node);
			BUG();
		}
	}
}

static int hydra_lookup_pud_owner(struct mm_struct *mm,
				  unsigned long addr,
				  struct vm_area_struct *exclude)
{
	unsigned long pud_start = addr & PUD_MASK;
	unsigned long pud_end = pud_start + PUD_SIZE;
	struct vm_area_struct *existing;
	VMA_ITERATOR(vmi, mm, pud_start);

	for_each_vma_range(vmi, existing, pud_end) {
		if (existing == exclude)
			continue;
		return existing->master_pgd_node;
	}

	return -1;
}

bool hydra_stack_expand_conflict(struct mm_struct *mm,
				 struct vm_area_struct *vma,
				 unsigned long start, unsigned long end)
{
	struct vm_area_struct *existing;
	VMA_ITERATOR(vmi, mm, start);

	if (start >= end)
		return false;

	for_each_vma_range(vmi, existing, end) {
		if (existing == vma)
			continue;
		if (existing->master_pgd_node != vma->master_pgd_node)
			return true;
	}

	return false;
}

void hydra_fixup_pud_nodes(struct mm_struct *mm,
			   struct vm_area_struct *vma)
{
	unsigned long pud_boundary;
	int node, next_owner, cur_owner;
	struct vm_area_struct *cur;

	if (!mm->lazy_repl_enabled)
		return;

	cur = vma;
	while (cur) {
		int did_split = 0;

		node = hydra_lookup_pud_owner(mm, cur->vm_start, cur);
		if (node >= 0)
			hydra_vma_chown(cur, node);
		cur_owner = cur->master_pgd_node;

		pud_boundary = (cur->vm_start & PUD_MASK) + PUD_SIZE;
		while (pud_boundary < cur->vm_end) {
			next_owner = hydra_lookup_pud_owner(mm, pud_boundary, cur);
			if (next_owner < 0 || next_owner == cur_owner) {
				pud_boundary += PUD_SIZE;
				continue;
			}

			{
				VMA_ITERATOR(vmi, mm, pud_boundary);
				if (__split_vma(&vmi, cur, pud_boundary, 0)) {
					pr_emerg("HYDRA: fixup_pud_nodes cannot split VMA at %lx to separate PUD owners (OOM); one-master-per-PUD invariant would be violated\n",
						 pud_boundary);
					BUG();
				}
			}
			did_split = 1;
			break;
		}

		hydra_pud_owner_stamp(mm, cur->vm_start, cur->vm_end, cur_owner);

		if (!did_split)
			break;

		cur = find_vma(mm, pud_boundary);
	}
}
