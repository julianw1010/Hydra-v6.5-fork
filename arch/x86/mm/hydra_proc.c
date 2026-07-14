#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/hydra.h>

extern int sysctl_hydra_repl_order;
extern int sysctl_hydra_tlbflush_opt;

static struct proc_dir_entry *hydra_dir;

struct hydra_int_knob {
	const char *name;
	int *val;
	int min;
	int max;
};

static const struct hydra_int_knob hydra_repl_order_knob = {
	"repl_order", &sysctl_hydra_repl_order, 0, 9,
};

static const struct hydra_int_knob hydra_tlbflush_opt_knob = {
	"tlbflush_opt", &sysctl_hydra_tlbflush_opt, 0, 1,
};

static int hydra_proc_parse_long(const char __user *ubuf, size_t count,
				 long *val)
{
	char buf[32];
	size_t len;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, val))
		return -EINVAL;

	return 0;
}

static int hydra_knob_show(struct seq_file *m, void *v)
{
	const struct hydra_int_knob *knob = m->private;

	seq_printf(m, "%d\n", *knob->val);
	return 0;
}

static int hydra_knob_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_knob_show, pde_data(inode));
}

static ssize_t hydra_knob_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	const struct hydra_int_knob *knob = pde_data(file_inode(file));
	long val;
	int ret;

	ret = hydra_proc_parse_long(ubuf, count, &val);
	if (ret)
		return ret;

	if (val < knob->min || val > knob->max)
		return -EINVAL;

	*knob->val = (int)val;

	pr_info("HYDRA: %s set to %d\n", knob->name, *knob->val);

	return count;
}

static const struct proc_ops hydra_knob_ops = {
	.proc_open	= hydra_knob_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_knob_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static void hydra_cache_print_row(struct seq_file *m, const char *label,
				  const long long *vals, long long total)
{
	int node;

	seq_printf(m, " %-11s", label);
	for (node = 0; node < NUMA_NODE_COUNT; node++)
		seq_printf(m, " %10lld", vals[node]);
	seq_printf(m, " %10lld\n", total);
}

static int hydra_cache_show(struct seq_file *m, void *v)
{
	long pages[NUMA_NODE_COUNT];
	long long hits[NUMA_NODE_COUNT], misses[NUMA_NODE_COUNT];
	long long returns[NUMA_NODE_COUNT];
	long total_pages = 0;
	long long total_hits = 0, total_misses = 0, total_returns = 0;
	char buf[24];
	long mib10;
	int node;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		struct hydra_cache_head *c = &hydra_cache[node];

		pages[node] = atomic_read(&c->count);
		hits[node] = atomic64_read(&c->hits);
		misses[node] = atomic64_read(&c->misses);
		returns[node] = atomic64_read(&c->returns);

		total_pages += pages[node];
		total_hits += hits[node];
		total_misses += misses[node];
		total_returns += returns[node];
	}

	seq_puts(m, " Hydra per-node page-table page cache\n");
	seq_puts(m, " write N > 0: add N pages to the cache of every online node\n");
	seq_puts(m, " write -1:    drain all nodes\n");
	seq_puts(m, " hits/misses/returns count only mms with replication enabled\n");
	seq_puts(m, " rows = cache metric,  cols = NUMA node\n");
	seq_puts(m, " --------------------------------------------------------------------------------------------------------------\n");

	seq_printf(m, " %-11s", "");
	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		scnprintf(buf, sizeof(buf), "n%d", node);
		seq_printf(m, " %10s", buf);
	}
	seq_printf(m, " %10s\n", "TOTAL");

	seq_printf(m, " %-11s", "pages");
	for (node = 0; node < NUMA_NODE_COUNT; node++)
		seq_printf(m, " %10ld", pages[node]);
	seq_printf(m, " %10ld\n", total_pages);

	seq_printf(m, " %-11s", "size (MiB)");
	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		mib10 = pages[node] * (PAGE_SIZE / 1024) * 10 / 1024;
		scnprintf(buf, sizeof(buf), "%ld.%ld", mib10 / 10, mib10 % 10);
		seq_printf(m, " %10s", buf);
	}
	mib10 = total_pages * (PAGE_SIZE / 1024) * 10 / 1024;
	scnprintf(buf, sizeof(buf), "%ld.%ld", mib10 / 10, mib10 % 10);
	seq_printf(m, " %10s\n", buf);

	hydra_cache_print_row(m, "hits", hits, total_hits);
	hydra_cache_print_row(m, "misses", misses, total_misses);
	hydra_cache_print_row(m, "returns", returns, total_returns);

	return 0;
}

static int hydra_cache_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_cache_show, NULL);
}

static ssize_t hydra_cache_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	long val, added;
	int node, drained, ret;

	ret = hydra_proc_parse_long(ubuf, count, &val);
	if (ret)
		return ret;

	if (val == -1) {
		drained = hydra_cache_drain_all();
		pr_info("HYDRA: cache drained %d pages\n", drained);
		return count;
	}

	if (val <= 0)
		return -EINVAL;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		if (!node_online(node))
			continue;

		for (added = 0; added < val; added++) {
			struct page *page;

			page = alloc_pages_node(node,
				GFP_KERNEL | __GFP_ZERO | __GFP_THISNODE, 0);
			if (!page)
				return -ENOMEM;

			BUG_ON(page_to_nid(page) != node);

			page->next_replica = NULL;
			page->pt_owner_mm = NULL;

			if (!hydra_cache_push(page, node, false))
				BUG();
		}
	}

	pr_info("HYDRA: cache populated %ld pages on each of %d nodes\n",
		val, num_online_nodes());

	return count;
}

static const struct proc_ops hydra_cache_ops = {
	.proc_open	= hydra_cache_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_cache_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const struct proc_ops hydra_status_ops = {
	.proc_open	= hydra_status_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};

static const struct proc_ops hydra_history_ops = {
	.proc_open	= hydra_history_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};

static int __init hydra_proc_init(void)
{
	hydra_dir = proc_mkdir("hydra", NULL);
	if (!hydra_dir)
		return -ENOMEM;

	if (!proc_create("cache", 0644, hydra_dir, &hydra_cache_ops))
		goto fail;

	if (!proc_create_data("repl_order", 0644, hydra_dir, &hydra_knob_ops,
			      (void *)&hydra_repl_order_knob))
		goto fail;

	if (!proc_create_data("tlbflush_opt", 0644, hydra_dir, &hydra_knob_ops,
			      (void *)&hydra_tlbflush_opt_knob))
		goto fail;

	if (!proc_create("status", 0444, hydra_dir, &hydra_status_ops))
		goto fail;

	if (!proc_create("history", 0444, hydra_dir, &hydra_history_ops))
		goto fail;

	return 0;

fail:
	remove_proc_subtree("hydra", NULL);
	return -ENOMEM;
}
late_initcall(hydra_proc_init);
