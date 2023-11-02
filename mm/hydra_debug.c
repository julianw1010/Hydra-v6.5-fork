#include <linux/syscalls.h>
#include <linux/printk.h>
#include <linux/mm.h>
#include <linux/hydra_debug.h>
#include <linux/hydra_util.h>

#include <asm/pgtable.h>

int do_hydra_dump_ll(struct mm_struct *mm, unsigned long addr) {
	int ret = 0;
	int node = 0;
	struct page *page;
	pte_t *ptep;
	struct vm_area_struct *vma;
	pid_t tgid = current ? current->tgid : -1;

	if (!mm || !mm->lazy_repl_enabled) {
		return -EINVAL;
	}

	vma = find_vma(mm, addr);
	if (!vma) {
		printk(KERN_INFO "Hydra dump: (PID %d, address %lx): EFAULT\n",
		       tgid, addr);
		ret = -EFAULT;
		goto exit;
	}

	printk(KERN_INFO "Hydra dump: (PID %d, address %lx): Master node %ld, VMA start %lx, VMA end %lx\n",
	       tgid, addr, vma->master_pgd_node, vma->vm_start, vma->vm_end);
	for (node = 0; node < NUMA_NODE_COUNT; ++node) {
		ptep = hydra_find_pte(mm, addr, node);
		if (!HYDRA_FIND_BAD(ptep)) {
			page = virt_to_page(ptep);
			printk(KERN_INFO
			       "Hydra dump: (PID %d, address %lx): Node %d, PTE %lx @ %px, page %px, next replica %px\n",
			       tgid, addr, node, ptep->pte, ptep, page,
			       page->next_replica);
		} else {
			printk(KERN_INFO
			       "Hydra dump: (PID %d, address %lx): Node %d, result %lu\n",
			       tgid, addr, node, (unsigned long)ptep);
		}
	}

exit:
	return ret;
}

SYSCALL_DEFINE1(hydra_dump_ll, unsigned long, addr)
{
	int ret = 0;
	struct mm_struct *mm = current->mm;
	if (!mm->lazy_repl_enabled) {
		return -EINVAL;
	}
	down_read(&mm->mmap_lock);
	ret = do_hydra_dump_ll(mm, addr);
	up_read(&mm->mmap_lock);
	return ret;
}
