#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/gfp.h>
#include <linux/page-flags.h>
#include <linux/nodemask.h>
#include <linux/hydra_util.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/hydra_pti.h>

struct page *repl_alloc_page_on_node(size_t nid, unsigned int order)
{
	nodemask_t nm = NODE_MASK_NONE;
	struct page *p;

	node_set(nid, nm);
	p = __alloc_pages((GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_THISNODE), order, nid, &nm);

	if (p) {
		p->next_replica = NULL;
		p->pt_owner_mm = NULL;
		p->mitosis_tracking = NULL;
	}

	return p;
}

pgtable_t repl_pte_alloc_one(struct mm_struct *mm, unsigned long address,
			     size_t nid)
{
	struct page *pte;

	pte = hydra_cache_pop(nid);
	if (!pte) {
		pte = repl_alloc_page_on_node(nid, 0);
		if (!pte)
			return NULL;
	}

	if (!pgtable_pte_page_ctor(pte)) {
		if (!hydra_cache_push(pte, nid))
			__free_page(pte);
		return NULL;
	}

	pte->pt_owner_mm = mm;
	pte->mitosis_tracking = NULL;
	return pte;
}

pmd_t *repl_pmd_alloc_one(struct mm_struct *mm, unsigned long addr, size_t nid)
{
	struct page *page;

	page = hydra_cache_pop(nid);
	if (!page) {
		page = repl_alloc_page_on_node(nid, 0);
		if (!page)
			return NULL;
	}

	if (!pgtable_pmd_page_ctor(page)) {
		if (!hydra_cache_push(page, nid))
			__free_page(page);
		return NULL;
	}

	page->pt_owner_mm = mm;
	return (pmd_t *)page_address(page);
}

pud_t *repl_pud_alloc_one(struct mm_struct *mm, unsigned long addr, size_t nid)
{
	struct page *page;

	page = hydra_cache_pop(nid);
	if (!page) {
		page = repl_alloc_page_on_node(nid, 0);
		if (!page)
			return NULL;
	}

	page->pt_owner_mm = mm;
	return (pud_t *)page_address(page);
}

p4d_t *repl_p4d_alloc_one(struct mm_struct *mm, unsigned long addr, size_t nid)
{
	struct page *page;

	page = hydra_cache_pop(nid);
	if (!page) {
		page = repl_alloc_page_on_node(nid, 0);
		if (!page)
			return NULL;
	}

	page->pt_owner_mm = mm;
	return (p4d_t *)page_address(page);
}
