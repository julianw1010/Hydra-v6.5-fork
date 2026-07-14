#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/cpumask.h>
#include <linux/nodemask.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>

void hydra_reload_cr3(void *info)
{
	struct mm_struct *mm = info;
	if (this_cpu_read(cpu_tlbstate.loaded_mm) == mm) {
		int node = numa_node_id();
		unsigned long cur_cr3 = __read_cr3();
		unsigned long new_pa = __pa(mm->repl_pgd[node]);

		if ((cur_cr3 & PAGE_MASK) != new_pa) {
			native_write_cr3(new_pa | (cur_cr3 & ~PAGE_MASK));
			__flush_tlb_all();
		}
	}
}

int hydra_enable_replication(struct mm_struct *mm)
{
	int i, primary_node;

	if (mm->lazy_repl_enabled)
		return 0;

	static_branch_enable(&hydra_repl_ever_enabled);

	mmap_write_lock(mm);

	if (mm->lazy_repl_enabled) {
		mmap_write_unlock(mm);
		return 0;
	}

	primary_node = page_to_nid(virt_to_page(mm->pgd));

	{
		struct xarray *xa = kzalloc(sizeof(*xa), GFP_KERNEL);

		if (!xa) {
			pr_emerg("HYDRA: pud owner map allocation failed for mm %px during enable\n",
				 mm);
			BUG();
		}
		xa_init(xa);
		mm->hydra_pud_owner = xa;
	}

	{
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			hydra_vma_chown(vma, primary_node);
		}
	}

	hydra_stats_mark_enabled(mm, primary_node);

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (i == primary_node) {
			mm->repl_pgd[i] = mm->pgd;
		} else {
			mm->repl_pgd[i] = hydra_repl_pgd_alloc(mm, i);
			if (!mm->repl_pgd[i]) {
				pr_emerg("HYDRA: replica PGD allocation failed for mm %px node %d during enable\n",
					 mm, i);
				BUG();
			}
		}
	}

	WRITE_ONCE(mm->lazy_repl_enabled, true);
	smp_mb();
	on_each_cpu_mask(mm_cpumask(mm), hydra_reload_cr3, mm, 1);
	flush_tlb_mm(mm);

	{
		int count = 0;
		for (i = 0; i < NUMA_NODE_COUNT; i++) {
			if (mm->repl_pgd[i] && mm->repl_pgd[i] != mm->pgd)
				count++;
		}
		count++;
		printk(KERN_INFO "HYDRA: enabled page table replication for mm %px on %d nodes\n", mm, count);
	}

	mmap_write_unlock(mm);

	return 0;
}

SYSCALL_DEFINE0(set_pgtblreplpolicy)
{
	return hydra_enable_replication(current->mm);
}
