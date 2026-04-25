#include <linux/mm.h>
#include <linux/sched.h>
#include <asm/pgtable.h>
#include <linux/hydra_util.h>

int sysctl_hydra_verify_enabled;
EXPORT_SYMBOL(sysctl_hydra_verify_enabled);

void hydra_verify_fault_walk(struct mm_struct *mm, unsigned long address,
			     int expected_node)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int pg_node;

	if (!sysctl_hydra_verify_enabled)
		return;

	if (!mm->lazy_repl_enabled)
		return;

	if (expected_node < 0 || expected_node >= NUMA_NODE_COUNT)
		return;

	pgd = mm->repl_pgd[expected_node];
	if (!pgd)
		return;

	pgd = pgd_offset_pgd(pgd, address);
	if (pgd_none(*pgd) || !pgd_present(*pgd))
		return;

	if (pgtable_l5_enabled()) {
		unsigned long child_phys = pgd_val(*pgd) & PTE_PFN_MASK;
		if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
			pg_node = page_to_nid(pfn_to_page(child_phys >> PAGE_SHIFT));
			if (pg_node != expected_node) {
				pr_err("HYDRA: verify: P4D on node %d expected %d addr=0x%lx comm=%s pid=%d\n",
				       pg_node, expected_node, address,
				       current->comm, current->pid);
				BUG();
			}
		}
	}

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || !p4d_present(*p4d))
		return;

	{
		unsigned long pud_phys = p4d_val(*p4d) & PTE_PFN_MASK;
		if (pud_phys && pfn_valid(pud_phys >> PAGE_SHIFT)) {
			pg_node = page_to_nid(pfn_to_page(pud_phys >> PAGE_SHIFT));
			if (pg_node != expected_node) {
				pr_err("HYDRA: verify: PUD on node %d expected %d addr=0x%lx comm=%s pid=%d\n",
				       pg_node, expected_node, address,
				       current->comm, current->pid);
				BUG();
			}
		}
	}

	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || !pud_present(*pud) || pud_trans_huge(*pud))
		return;

	{
		unsigned long pmd_phys = pud_val(*pud) & PTE_PFN_MASK;
		if (pmd_phys && pfn_valid(pmd_phys >> PAGE_SHIFT)) {
			pg_node = page_to_nid(pfn_to_page(pmd_phys >> PAGE_SHIFT));
			if (pg_node != expected_node) {
				pr_err("HYDRA: verify: PMD on node %d expected %d addr=0x%lx comm=%s pid=%d\n",
				       pg_node, expected_node, address,
				       current->comm, current->pid);
				BUG();
			}
		}
	}

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd) || !pmd_present(*pmd) || pmd_trans_huge(*pmd))
		return;

	pte = pte_offset_kernel(pmd, address);
	if (!virt_addr_valid(pte))
		return;

	pg_node = page_to_nid(virt_to_page(pte));
	if (pg_node != expected_node) {
		pr_err("HYDRA: verify: PTE on node %d expected %d addr=0x%lx comm=%s pid=%d\n",
		       pg_node, expected_node, address,
		       current->comm, current->pid);
		BUG();
	}
}
EXPORT_SYMBOL(hydra_verify_fault_walk);
