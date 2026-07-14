#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/seq_file.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

static LIST_HEAD(hydra_live_list);
static LIST_HEAD(hydra_hist_list);
static DEFINE_SPINLOCK(hydra_stats_lock);
static unsigned long hydra_stats_next_id;

struct hydra_stats *hydra_stats_attach(struct mm_struct *mm)
{
	struct hydra_stats *s;

	if (mm->hydra_stats)
		return mm->hydra_stats;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;

	INIT_LIST_HEAD(&s->list);
	s->pid = current->pid;
	get_task_comm(s->comm, current);
	s->mm = mm;
	s->master_node = -1;

	spin_lock(&hydra_stats_lock);
	s->id = ++hydra_stats_next_id;
	list_add_tail(&s->list, &hydra_live_list);
	spin_unlock(&hydra_stats_lock);

	mm->hydra_stats = s;
	return s;
}

void hydra_stats_mark_enabled(struct mm_struct *mm, int master_node)
{
	struct hydra_stats *s = mm->hydra_stats;

	if (!s)
		return;

	s->master_node = master_node;
	WRITE_ONCE(s->ever_enabled, 1);

	if (mm == current->mm) {
		s->pid = current->pid;
		get_task_comm(s->comm, current);
	}
}

void hydra_stats_detach(struct mm_struct *mm)
{
	struct hydra_stats *s = mm->hydra_stats;
	int i, count = 0;

	if (!s)
		return;

	mm->hydra_stats = NULL;

	if (!s->ever_enabled) {
		spin_lock(&hydra_stats_lock);
		list_del(&s->list);
		spin_unlock(&hydra_stats_lock);
		kfree(s);
		return;
	}

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (mm->repl_pgd[i] && mm->repl_pgd[i] != mm->pgd)
			count++;
	}
	count++;

	printk(KERN_INFO "HYDRA: disabled page table replication for mm %px on %d nodes\n",
	       mm, count);

	spin_lock(&hydra_stats_lock);
	list_move_tail(&s->list, &hydra_hist_list);
	spin_unlock(&hydra_stats_lock);
}

static void hydra_bump_max(atomic_long_t *maxp, long cur)
{
	long mx = atomic_long_read(maxp);

	while (cur > mx) {
		long prev = atomic_long_cmpxchg(maxp, mx, cur);

		if (prev == mx)
			break;
		mx = prev;
	}
}

static void hydra_pt_account_mm(struct page *page, int delta)
{
	struct mm_struct *mm;
	struct hydra_stats *s;
	int nid, lvl;

	lvl = page->pt_level;
	if (lvl < 0 || lvl >= HYDRA_PT_NR_LEVELS)
		return;

	mm = page->pt_owner_mm;
	if (!mm)
		return;

	s = mm->hydra_stats;
	if (!s)
		return;

	nid = page_to_nid(page);
	if (nid < 0 || nid >= NUMA_NODE_COUNT)
		return;

	if (delta > 0)
		hydra_bump_max(&s->pt_max[nid][lvl],
			       atomic_long_inc_return(&s->pt_cur[nid][lvl]));
	else
		atomic_long_dec(&s->pt_cur[nid][lvl]);
}

void hydra_pt_account(struct page *page, int delta)
{
	int lvl;

	if (!page)
		return;

	lvl = page->pt_level;
	if (lvl < 0 || lvl >= HYDRA_PT_NR_LEVELS)
		return;

	hydra_pt_account_mm(page, delta);
}

void hydra_vma_attach(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	struct hydra_stats *s;
	int node;

	if (!mm)
		return;
	if (mm->lazy_repl_enabled)
		hydra_pud_owner_claim(mm, vma->vm_start, vma->vm_end,
				      vma->master_pgd_node);
	s = mm->hydra_stats;
	if (!s)
		return;
	node = vma->master_pgd_node;
	if (node < 0 || node >= NUMA_NODE_COUNT)
		return;
	hydra_bump_max(&s->vma_owner_max[node],
		       atomic_long_inc_return(&s->vma_owner_cur[node]));
}

void hydra_vma_detach(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	struct hydra_stats *s;
	int node;

	if (!mm)
		return;
	s = mm->hydra_stats;
	if (!s)
		return;
	node = vma->master_pgd_node;
	if (node < 0 || node >= NUMA_NODE_COUNT)
		return;
	atomic_long_dec(&s->vma_owner_cur[node]);
}

void hydra_vma_chown(struct vm_area_struct *vma, int node)
{
	struct mm_struct *mm = vma->vm_mm;
	struct hydra_stats *s = mm ? mm->hydra_stats : NULL;
	int old = vma->master_pgd_node;

	if (s && old != node &&
	    old >= 0 && old < NUMA_NODE_COUNT &&
	    node >= 0 && node < NUMA_NODE_COUNT) {
		atomic_long_dec(&s->vma_owner_cur[old]);
		hydra_bump_max(&s->vma_owner_max[node],
			       atomic_long_inc_return(&s->vma_owner_cur[node]));
	}
	WRITE_ONCE(vma->master_pgd_node, node);
	if (mm && mm->hydra_pud_owner)
		hydra_pud_owner_stamp(mm, vma->vm_start, vma->vm_end, node);
}

static const char * const hydra_level_name[HYDRA_PT_NR_LEVELS] = {
	"PGD", "P4D", "PUD", "PMD", "PTE",
};

#define HYDRA_RULE \
	"========================================================================"

static void hydra_print_section(struct seq_file *m, const char *name)
{
	seq_printf(m, "\n  %s\n", name);
	seq_puts(m, "  ------------------------------------------------------------------\n");
}

static void hydra_print_kv(struct seq_file *m, const char *label, long val)
{
	seq_printf(m, "    %-40s %12ld\n", label, val);
}

static void hydra_print_sub(struct seq_file *m, const char *label, long val)
{
	seq_printf(m, "      %-38s %12ld\n", label, val);
}

static void hydra_print_group(struct seq_file *m, const char *name)
{
	seq_printf(m, "      %s:\n", name);
}

static void hydra_print_sub2(struct seq_file *m, const char *label, long val)
{
	seq_printf(m, "        %-36s %12ld\n", label, val);
}

static void hydra_print_sub_pct(struct seq_file *m, const char *label,
				long val, long total)
{
	if (total > 0) {
		long p10 = (val * 1000 + total / 2) / total;

		seq_printf(m, "      %-38s %12ld   (%ld.%ld%%)\n",
			   label, val, p10 / 10, p10 % 10);
	} else {
		seq_printf(m, "      %-38s %12ld\n", label, val);
	}
}

static void hydra_print_ratio(struct seq_file *m, const char *label,
			      long num, long den)
{
	if (den > 0) {
		long x10 = (num * 10 + den / 2) / den;

		seq_printf(m, "    %-40s %10ld.%ld\n", label, x10 / 10, x10 % 10);
	} else {
		seq_printf(m, "    %-40s %12s\n", label, "n/a");
	}
}

static void hydra_print_node_header(struct seq_file *m)
{
	char buf[12];
	int n;

	seq_puts(m, "        ");
	for (n = 0; n < NUMA_NODE_COUNT; n++) {
		scnprintf(buf, sizeof(buf), "n%d", n);
		seq_printf(m, " %7s", buf);
	}
	seq_putc(m, '\n');
}

static void hydra_print_node_matrix(struct seq_file *m,
				    atomic_long_t mat[NUMA_NODE_COUNT][NUMA_NODE_COUNT])
{
	char buf[12];
	int from, to;

	hydra_print_node_header(m);
	for (from = 0; from < NUMA_NODE_COUNT; from++) {
		scnprintf(buf, sizeof(buf), "n%d", from);
		seq_printf(m, "    %-4s", buf);
		for (to = 0; to < NUMA_NODE_COUNT; to++)
			seq_printf(m, " %7ld", atomic_long_read(&mat[from][to]));
		seq_putc(m, '\n');
	}
}

static void hydra_print_pt_table(struct seq_file *m, struct hydra_stats *s,
				 bool history)
{
	int node, lvl;

	hydra_print_node_header(m);
	for (lvl = 0; lvl < HYDRA_PT_NR_LEVELS; lvl++) {
		seq_printf(m, "    %-4s", hydra_level_name[lvl]);
		for (node = 0; node < NUMA_NODE_COUNT; node++)
			seq_printf(m, " %7ld",
				   atomic_long_read(history ?
						    &s->pt_max[node][lvl] :
						    &s->pt_cur[node][lvl]));
		seq_putc(m, '\n');
	}
}

static void hydra_stats_print(struct seq_file *m, struct hydra_stats *s,
			      bool history)
{
	seq_printf(m, "%s\n", HYDRA_RULE);
	seq_printf(m, "  MM record #%lu\n", s->id);
	seq_printf(m, "%s\n", HYDRA_RULE);
	seq_printf(m, "    %-40s %d\n", "pid", s->pid);
	seq_printf(m, "    %-40s %s\n", "comm", s->comm);
	seq_printf(m, "    %-40s %px\n", "mm", s->mm);
	seq_printf(m, "    %-40s %d\n", "master node", s->master_node);

	hydra_print_section(m, "THP / page-table events");
	hydra_print_kv(m, "THP splits", atomic_long_read(&s->thp_split));
	hydra_print_kv(m, "THP collapses", atomic_long_read(&s->thp_collapse));
	hydra_print_kv(m, "THP pgtable deposits",
		       atomic_long_read(&s->deposits));
	hydra_print_kv(m, "THP pgtable withdrawals",
		       atomic_long_read(&s->withdrawals));

	{
		long mf = atomic_long_read(&s->master_faults);
		long mfw = atomic_long_read(&s->master_faults_write);
		long mfp = atomic_long_read(&s->master_faults_present);
		long rf = atomic_long_read(&s->replica_faults);
		long rfw = atomic_long_read(&s->replica_faults_write);
		long rfp = atomic_long_read(&s->replica_faults_present);
		long rsm = atomic_long_read(&s->replica_serviced_on_master);

		hydra_print_section(m, "Replication faults");
		seq_puts(m,
			 "  (each of the two breakdowns below sums to the total independently)\n");
		hydra_print_kv(m, "Master faults", mf);
		hydra_print_group(m, "by access");
		hydra_print_sub2(m, "read", mf - mfw);
		hydra_print_sub2(m, "write", mfw);
		hydra_print_group(m, "by fault type");
		hydra_print_sub2(m, "not-present (major/fill)", mf - mfp);
		hydra_print_sub2(m, "present (permission/minor)", mfp);
		hydra_print_kv(m, "Replica faults", rf);
		hydra_print_group(m, "by access");
		hydra_print_sub2(m, "read", rf - rfw);
		hydra_print_sub2(m, "write", rfw);
		hydra_print_group(m, "by fault type");
		hydra_print_sub2(m, "not-present (major/fill)", rf - rfp);
		hydra_print_sub2(m, "present (permission/minor)", rfp);
		hydra_print_sub_pct(m, "serviced on master", rsm, rf);
	}

	{
		long pc = atomic_long_read(&s->pte_entries_copied);
		long pp = atomic_long_read(&s->pte_entries_prefetched);
		long pf = atomic_long_read(&s->pte_copy_faults);
		long mc = atomic_long_read(&s->pmd_entries_copied);
		long mp = atomic_long_read(&s->pmd_entries_prefetched);
		long mff = atomic_long_read(&s->pmd_copy_faults);

		hydra_print_section(m, "Master -> replica entry copying");
		hydra_print_kv(m, "PTE entries copied (4KB)", pc);
		hydra_print_sub(m, "of which prefetched", pp);
		hydra_print_kv(m, "PTE copy faults", pf);
		hydra_print_ratio(m, "PTE entries per copy fault", pc, pf);
		hydra_print_kv(m, "PMD entries copied (2MB)", mc);
		hydra_print_sub(m, "of which prefetched", mp);
		hydra_print_kv(m, "PMD copy faults", mff);
		hydra_print_ratio(m, "PMD entries per copy fault", mc, mff);
	}

	{
		long tlb_sent = atomic_long_read(&s->tlb_shootdowns);
		long tlb_saved = atomic_long_read(&s->tlb_shootdowns_saved);

		hydra_print_section(m, "TLB shootdowns (remote-CPU IPIs)");
		hydra_print_kv(m, "Total shootdowns (with optimization)", tlb_sent);
		hydra_print_kv(m, "Shootdowns saved by node-scoping", tlb_saved);
		hydra_print_kv(m, "Shootdowns without optimization (est)",
			       tlb_sent + tlb_saved);
	}

	hydra_print_section(m, "TLB broadcasts (INVLPGB, no IPIs)");
	hydra_print_kv(m, "Total INVLPGB instructions",
		       atomic_long_read(&s->tlb_broadcasts));

	hydra_print_section(m,
		"autoNUMA migrations: 4KB base pages  [rows = source node, cols = dest node]");
	hydra_print_node_matrix(m, s->numa_migrate_4k);

	hydra_print_section(m,
		"autoNUMA migrations: 2MB THP pages  [rows = source node, cols = dest node]");
	hydra_print_node_matrix(m, s->numa_migrate_2m);

	hydra_print_section(m, history ?
		"Page tables: MAX watermark  [rows = level, cols = node]" :
		"Page tables: current  [rows = level, cols = node]");
	hydra_print_pt_table(m, s, history);

	{
		int node;

		hydra_print_section(m, history ?
			"VMA owner distribution: MAX watermark  [cols = owner node]" :
			"VMA owner distribution: current  [cols = owner node]");
		hydra_print_node_header(m);
		seq_printf(m, "    %-4s", "VMAs");
		for (node = 0; node < NUMA_NODE_COUNT; node++)
			seq_printf(m, " %7ld",
				   atomic_long_read(history ?
						    &s->vma_owner_max[node] :
						    &s->vma_owner_cur[node]));
		seq_putc(m, '\n');
	}

	seq_putc(m, '\n');
}

static void *hydra_live_start(struct seq_file *m, loff_t *pos)
{
	spin_lock(&hydra_stats_lock);
	m->private = &hydra_live_list;
	return seq_list_start(&hydra_live_list, *pos);
}

static void *hydra_hist_start(struct seq_file *m, loff_t *pos)
{
	spin_lock(&hydra_stats_lock);
	m->private = &hydra_hist_list;
	return seq_list_start(&hydra_hist_list, *pos);
}

static void *hydra_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	return seq_list_next(v, (struct list_head *)m->private, pos);
}

static void hydra_seq_stop(struct seq_file *m, void *v)
{
	spin_unlock(&hydra_stats_lock);
}

static int hydra_seq_show(struct seq_file *m, void *v)
{
	struct hydra_stats *s = list_entry(v, struct hydra_stats, list);

	if (!READ_ONCE(s->ever_enabled))
		return SEQ_SKIP;

	hydra_stats_print(m, s, m->private == &hydra_hist_list);
	return 0;
}

static const struct seq_operations hydra_live_seq_ops = {
	.start	= hydra_live_start,
	.next	= hydra_seq_next,
	.stop	= hydra_seq_stop,
	.show	= hydra_seq_show,
};

static const struct seq_operations hydra_hist_seq_ops = {
	.start	= hydra_hist_start,
	.next	= hydra_seq_next,
	.stop	= hydra_seq_stop,
	.show	= hydra_seq_show,
};

int hydra_status_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &hydra_live_seq_ops);
}

int hydra_history_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &hydra_hist_seq_ops);
}
