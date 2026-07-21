#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/hydra.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include "mm_internal.h"

pgd_t *hydra_repl_pgd_alloc(struct mm_struct *mm, size_t nid)
{
	pgd_t *pgd;
	struct page *page;
	int order = PGD_ALLOCATION_ORDER;
	nodemask_t nm = NODE_MASK_NONE;

	if (order == 0) {
		page = hydra_cache_pop(nid);
		if (page)
			goto got_page;
	}

	node_set(nid, nm);
	page = __alloc_pages(GFP_PGTABLE_USER | __GFP_THISNODE, order, nid, &nm);
	if (!page)
		return NULL;

	page->next_replica = NULL;

got_page:
	page->pt_owner_mm = mm;
	pgd = (pgd_t *)page_address(page);

	spin_lock(&pgd_lock);
	pgd_ctor(mm, pgd);
	spin_unlock(&pgd_lock);

	page->pt_level = HYDRA_PT_PGD;
	hydra_pt_account(page, 1);

	return pgd;
}
