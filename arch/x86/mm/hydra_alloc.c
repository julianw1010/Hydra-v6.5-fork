#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/page-flags.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

struct page *hydra_alloc_page_on_node(size_t nid, unsigned int order)
{
	nodemask_t nm = NODE_MASK_NONE;
	struct page *p;

	node_set(nid, nm);
	p = __alloc_pages((GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_THISNODE), order, nid, &nm);

	if (p) {
		p->next_replica = NULL;
		p->pt_owner_mm = NULL;
	}

	return p;
}

bool hydra_try_return_page(struct page *page)
{
	int nid = page_to_nid(page);
	bool from_cache = PageHydraFromCache(page);
	struct mm_struct *owner = page->pt_owner_mm;
	bool count_stats = owner && READ_ONCE(owner->lazy_repl_enabled);

	ClearPageHydraFromCache(page);
	page->next_replica = NULL;

	if (from_cache && hydra_cache_push(page, nid, count_stats))
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
	struct page *page;

	if (parent && virt_addr_valid(parent))
		node = page_to_nid(virt_to_page(parent));
	else
		node = numa_node_id();

	page = hydra_cache_pop(node, mm && READ_ONCE(mm->lazy_repl_enabled));
	if (!page)
		page = alloc_pages_node(node, gfp | __GFP_THISNODE, 0);
	if (page)
		page->pt_owner_mm = mm;

	return page;
}
