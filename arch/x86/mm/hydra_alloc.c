#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/page-flags.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

bool hydra_try_return_page(struct page *page)
{
	int nid = page_to_nid(page);
	bool from_cache = PageHydraFromCache(page);

	ClearPageHydraFromCache(page);
	page->next_replica = NULL;

	if (from_cache && hydra_cache_push(page, nid))
		return true;

	return false;
}

void hydra_dtor_free_page(struct page *page)
{
	hydra_pagetable_dtor(page);
	hydra_pt_account(page, -1);

	if (hydra_try_return_page(page))
		return;

	__free_page(page);
}

struct page *hydra_alloc_pt_page_near(struct mm_struct *mm, gfp_t gfp,
				      void *parent)
{
	int node;
	struct page *page = NULL;

	if (parent && virt_addr_valid(parent))
		node = page_to_nid(virt_to_page(parent));
	else
		node = numa_node_id();

	if (mm && READ_ONCE(mm->lazy_repl_enabled))
		page = hydra_cache_pop(node);
	if (!page)
		page = alloc_pages_node(node, gfp | __GFP_THISNODE, 0);
	if (page)
		page->pt_owner_mm = mm;

	return page;
}
