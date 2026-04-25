#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/hydra_util.h>

extern int sysctl_hydra_verify_enabled;
extern int sysctl_hydra_repl_order;
extern int sysctl_hydra_tlbflush_opt;

static struct proc_dir_entry *hydra_dir;

static int hydra_cache_show(struct seq_file *m, void *v)
{
	int node;

	seq_printf(m, "%-6s %8s %12s %12s %12s\n",
		   "node", "count", "hits", "misses", "returns");

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		struct hydra_cache_head *c = &hydra_cache[node];

		seq_printf(m, "%-6d %8d %12lld %12lld %12lld\n",
			   node,
			   atomic_read(&c->count),
			   atomic64_read(&c->hits),
			   atomic64_read(&c->misses),
			   atomic64_read(&c->returns));
	}

	return 0;
}


static int hydra_cache_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_cache_show, NULL);
}

static ssize_t hydra_cache_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;
	int node, added, total, drained;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val == -1) {
		drained = hydra_cache_drain_all();
		pr_info("HYDRA: cache drained %d pages\n", drained);
		return count;
	}

	if (val <= 0 || val > 131072)
		return -EINVAL;

	total = 0;
	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		if (!node_online(node))
			continue;

		added = 0;
		while (added < val) {
			struct page *page;

			page = alloc_pages_node(node,
				GFP_KERNEL | __GFP_ZERO | __GFP_THISNODE, 0);
			if (!page)
				break;

			if (page_to_nid(page) != node) {
				__free_page(page);
				break;
			}

			page->next_replica = NULL;
			page->pt_owner_mm = NULL;

			if (!hydra_cache_push(page, node)) {
				__free_page(page);
				break;
			}

			added++;
		}

		total += added;
	}

	pr_info("HYDRA: cache populated %d pages across %d nodes\n",
		total, num_online_nodes());

	return count;
}

static const struct proc_ops hydra_cache_ops = {
	.proc_open	= hydra_cache_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_cache_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int hydra_verify_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_hydra_verify_enabled);
	return 0;
}

static int hydra_verify_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_verify_show, NULL);
}

static ssize_t hydra_verify_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	sysctl_hydra_verify_enabled = (val != 0) ? 1 : 0;

	pr_info("HYDRA: fault verification %s\n",
		sysctl_hydra_verify_enabled ? "enabled" : "disabled");

	return count;
}

static const struct proc_ops hydra_verify_ops = {
	.proc_open	= hydra_verify_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_verify_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int hydra_repl_order_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_hydra_repl_order);
	return 0;
}

static int hydra_repl_order_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_repl_order_show, NULL);
}

static ssize_t hydra_repl_order_write(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > 9)
		return -EINVAL;

	sysctl_hydra_repl_order = (int)val;

	pr_info("HYDRA: repl_order set to %d\n", sysctl_hydra_repl_order);

	return count;
}

static const struct proc_ops hydra_repl_order_ops = {
	.proc_open	= hydra_repl_order_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_repl_order_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int hydra_tlbflush_opt_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_hydra_tlbflush_opt);
	return 0;
}

static int hydra_tlbflush_opt_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_tlbflush_opt_show, NULL);
}

static ssize_t hydra_tlbflush_opt_write(struct file *file, const char __user *ubuf,
					size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > 3)
		return -EINVAL;

	sysctl_hydra_tlbflush_opt = (int)val;

	pr_info("HYDRA: tlbflush_opt set to %d\n", sysctl_hydra_tlbflush_opt);

	return count;
}

static const struct proc_ops hydra_tlbflush_opt_ops = {
	.proc_open	= hydra_tlbflush_opt_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_tlbflush_opt_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init hydra_proc_init(void)
{
	hydra_dir = proc_mkdir("hydra", NULL);
	if (!hydra_dir)
		return -ENOMEM;

	if (!proc_create("cache", 0644, hydra_dir, &hydra_cache_ops))
		goto fail;

	if (!proc_create("verify", 0644, hydra_dir, &hydra_verify_ops))
		goto fail;

	if (!proc_create("repl_order", 0644, hydra_dir, &hydra_repl_order_ops))
		goto fail;

	if (!proc_create("tlbflush_opt", 0644, hydra_dir, &hydra_tlbflush_opt_ops))
		goto fail;

	return 0;

fail:
	remove_proc_subtree("hydra", NULL);
	return -ENOMEM;
}
late_initcall(hydra_proc_init);
