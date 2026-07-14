#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>
#include <linux/hydra.h>
#include <asm/tlb.h>

struct hydra_cache_head hydra_cache[NUMA_NODE_COUNT] = {
	[0 ... NUMA_NODE_COUNT - 1] = {
		.lock		= __SPIN_LOCK_UNLOCKED(hydra_cache.lock),
		.head		= NULL,
		.count		= ATOMIC_INIT(0),
		.hits		= ATOMIC64_INIT(0),
		.misses		= ATOMIC64_INIT(0),
		.returns	= ATOMIC64_INIT(0),
	}
};

void hydra_pagetable_dtor(struct page *page)
{
	switch (page->pt_level) {
	case HYDRA_PT_PTE:
		pgtable_pte_page_dtor(page);
		break;
	case HYDRA_PT_PMD:
		pgtable_pmd_page_dtor(page);
		break;
	default:
		break;
	}
}

bool hydra_cache_push(struct page *page, int node, bool count_stats)
{
	struct hydra_cache_head *cache;
	unsigned long flags;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return false;

	cache = &hydra_cache[node];

	ClearPageHydraFromCache(page);

	spin_lock_irqsave(&cache->lock, flags);
	page->next_replica = cache->head;
	cache->head = page;
	spin_unlock_irqrestore(&cache->lock, flags);

	atomic_inc(&cache->count);
	if (count_stats)
		atomic64_inc(&cache->returns);

	return true;
}

struct page *hydra_cache_pop(int node, bool count_stats)
{
	struct hydra_cache_head *cache;
	struct page *page;
	unsigned long flags;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return NULL;

	cache = &hydra_cache[node];

	spin_lock_irqsave(&cache->lock, flags);
	page = cache->head;
	if (!page) {
		spin_unlock_irqrestore(&cache->lock, flags);
		if (count_stats)
			atomic64_inc(&cache->misses);
		return NULL;
	}
	cache->head = page->next_replica;
	spin_unlock_irqrestore(&cache->lock, flags);

	atomic_dec(&cache->count);
	if (count_stats)
		atomic64_inc(&cache->hits);

	page->next_replica = NULL;
	SetPageHydraFromCache(page);

	clear_highpage(page);

	return page;
}

int hydra_cache_drain_node(int node)
{
	struct hydra_cache_head *cache;
	struct page *page, *next;
	unsigned long flags;
	int freed = 0;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return 0;

	cache = &hydra_cache[node];

	spin_lock_irqsave(&cache->lock, flags);
	page = cache->head;
	cache->head = NULL;
	spin_unlock_irqrestore(&cache->lock, flags);

	if (!page)
		return 0;

	while (page) {
		next = page->next_replica;
		page->next_replica = NULL;
		ClearPageHydraFromCache(page);
		__free_page(page);
		freed++;
		page = next;
	}

	atomic_sub(freed, &cache->count);

	return freed;
}

int hydra_cache_drain_all(void)
{
	int node, total = 0;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		total += hydra_cache_drain_node(node);
	}

	return total;
}

static void hydra_chain_node_free_rcu(struct rcu_head *head)
{
	struct page *page = container_of(head, struct page, rcu_head);

	hydra_pagetable_dtor(page);

	if (!hydra_try_return_page(page))
		__free_page(page);
}

void hydra_free_chain_node_rcu(struct page *page)
{
	hydra_pt_account(page, -1);
	if (PageHydraFromCache(page))
		hydra_cache_count_return(page->pt_owner_mm, page_to_nid(page));
	page->pt_owner_mm = NULL;
	call_rcu(&page->rcu_head, hydra_chain_node_free_rcu);
}

void hydra_free_replica_chain(struct page *primary, struct mmu_gather *tlb)
{
	struct page *cur_page, *next_page;
	struct page *start_page;

	if (!primary || !primary->next_replica)
		return;

	start_page = primary;

	hydra_chain_lock(primary);
	cur_page = primary->next_replica;
	primary->next_replica = NULL;
	hydra_chain_unlock(primary);

	while (cur_page && cur_page != start_page) {
		struct mm_struct *owner_mm = cur_page->pt_owner_mm;

		next_page = READ_ONCE(cur_page->next_replica);
		cur_page->next_replica = NULL;

		if (owner_mm)
			mm_dec_nr_ptes(owner_mm);

		if (tlb) {
			hydra_pt_account(cur_page, -1);
			if (PageHydraFromCache(cur_page))
				hydra_cache_count_return(owner_mm,
							 page_to_nid(cur_page));
			cur_page->pt_owner_mm = NULL;
			tlb_remove_table(tlb, cur_page);
		} else {
			hydra_free_chain_node_rcu(cur_page);
		}

		cur_page = next_page;
	}
}

void hydra_cache_count_return(struct mm_struct *owner_mm, int node)
{
	if (node < 0 || node >= NUMA_NODE_COUNT)
		return;

	if (owner_mm && READ_ONCE(owner_mm->lazy_repl_enabled))
		atomic64_inc(&hydra_cache[node].returns);
}

bool hydra_cache_return_table(struct page *page)
{
	if (!PageHydraFromCache(page))
		return false;

	hydra_pagetable_dtor(page);

	if (!hydra_cache_push(page, page_to_nid(page), false))
		__free_page(page);

	return true;
}

void hydra_link_page_to_replica_chain(struct page *existing_page,
				      struct page *new_page)
{
	struct page *cur_page;
	struct page *start_page;
	struct page *next_repl;
	int chain_len = 0;

	if (!existing_page || !new_page || existing_page == new_page)
		return;

	BUG_ON(page_to_nid(new_page) == page_to_nid(existing_page));

	hydra_chain_lock(existing_page);

	start_page = existing_page;
	cur_page = READ_ONCE(existing_page->next_replica);

	while (cur_page && cur_page != start_page) {
		chain_len++;
		if (cur_page == new_page)
			goto out_unlock;
		if (page_to_nid(cur_page) == page_to_nid(new_page)) {
			pr_emerg("HYDRA: same-NID race: existing=%px(nid=%d) new=%px(nid=%d) "
				 "master=%px(nid=%d) chain_len=%d cpu=%d\n",
				 cur_page, page_to_nid(cur_page),
				 new_page, page_to_nid(new_page),
				 existing_page, page_to_nid(existing_page),
				 chain_len, smp_processor_id());
			BUG();
		}
		BUG_ON(chain_len >= NUMA_NODE_COUNT);
		cur_page = READ_ONCE(cur_page->next_replica);
	}

	next_repl = existing_page->next_replica;
	new_page->next_replica = next_repl ? next_repl : existing_page;
	smp_wmb();
	existing_page->next_replica = new_page;

out_unlock:
	hydra_chain_unlock(existing_page);
}

void hydra_break_chain(struct page *page)
{
	struct page *cur_page, *next_page;
	struct page *start_page;

	if (!page || !page->next_replica)
		return;

	start_page = page;

	hydra_chain_lock(page);
	cur_page = page->next_replica;
	page->next_replica = NULL;
	hydra_chain_unlock(page);

	while (cur_page && cur_page != start_page) {
		next_page = READ_ONCE(cur_page->next_replica);
		WRITE_ONCE(cur_page->next_replica, NULL);
		cur_page = next_page;
	}
}
