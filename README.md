# Hydra: NUMA-Aware Page Table Replication for Linux 6.4.0

A Linux 6.4.0 kernel patch that replicates user-space page tables across NUMA nodes so each node walks a local copy during TLB misses.

## References

Gao, B., Kang, Q., Tee, H.-W., Chu, K. T. N., Sanaee, A., and Jevdjic, D. *Scalable and Effective Page-table and TLB Management on NUMA Systems.* In Proceedings of the 2024 USENIX Annual Technical Conference (USENIX ATC '24), pp. 445-461. https://www.usenix.org/conference/atc24/presentation/gao-bin-scalable

Original implementation: https://github.com/julianw1010/Hydra-v6.5

This repository is a substantially reworked reimplementation derived from the original Hydra-v6.5 research prototype. It targets reliable operation on contemporary multi-socket machines with real workloads, and adds debugging and introspection infrastructure.

## How It Works

When replication is enabled for a process, the kernel allocates a separate copy of its page table tree on each configured NUMA node. All page table writes (set_pte, set_pmd, set_pud, set_p4d, set_pgd) are intercepted through the paravirt_ops interface and broadcast to every replica. On context switch, the kernel loads CR3 from the replica local to the CPU's NUMA node, so subsequent TLB misses walk node-local memory.

Replication is lazy: replica page table entries are populated on demand when a fault occurs on a non-owner node. A configurable prefetch window (default: up to 512 PTEs per fault) amortizes the cost of initial population.

TLB flushes are filtered by node mask. When a page table modification only affects replicas on a subset of nodes, the flush IPI is sent only to CPUs on those nodes.

## Prerequisites

Tested on Ubuntu 24.04.04 LTS. Run the prepare script to install build dependencies and generate the kernel configuration:

```bash
./prepare.sh
```

This installs build tools and development libraries, copies the running kernel's config, detects the NUMA node count via `numactl --hardware`, sets the required config options, and runs `make olddefconfig`. See `prepare.sh` for the full package list.

## Kernel Configuration

Two config options are required (set automatically by `prepare.sh`):

**PARAVIRT_XXL** must be enabled. Hydra uses the paravirt_ops hooks to intercept page table writes and broadcast them to replicas.

**HYDRA_NUMA_NODE_COUNT** must match the number of NUMA nodes on your system (check with `numactl --hardware`). The kernel will refuse to boot on mismatch. This controls the size of per-mm replica pointer arrays and the per-node page table cache.

To adjust manually:

```
make menuconfig
# Processor type and features -> Paravirtualization -> PARAVIRT_XXL: Y
# Processor type and features -> NUMA -> HYDRA_NUMA_NODE_COUNT: your node count
```

## Building and Installation

```bash
make -j$(nproc)
./install.sh
```

`install.sh` removes any previous `6.4.0-hydra` images from `/boot`, builds the kernel, and installs modules and image. Reboot into the new kernel after installation.

## Usage

### Boot-time replication

The `hydra` boot parameter auto-enables replication for all new user processes. Children inherit replication from their parents.

```
# In your bootloader config (e.g. GRUB_CMDLINE_LINUX):
hydra
```

Replication can also be enabled from the command line using [numactl-hydra](https://github.com/julianw1010/numactl-hydra):

```bash
numactl-hydra -r all ./my_application
```

### Syscall interface

Replication can be enabled per process via the syscalls added to the 64-bit syscall table:

```c
#include <unistd.h>
#include <sys/syscall.h>

// Enable replication (syscall 400)
syscall(400, 1, NULL, 0);

// Query replication status (syscall 401)
syscall(401, &policy, nmask, maxnode, addr, flags);
```

### Runtime enablement

Replication can be enabled on an already-running process. The kernel migrates existing page tables to the owner node, allocates replica PGDs, and issues an IPI to all CPUs in the mm's cpumask to reload CR3.

## Inheritance and execve

Child processes inherit replication state from their parent during `fork`. Replication state also survives `execve`: the new mm enables replication after the new binary is loaded, if the parent had it enabled or if `sysctl_hydra_auto_enable` is set.

## Page Table Isolation (PTI)

Hydra supports kernels with KPTI enabled. Replica PGDs are allocated at the correct order (two pages for kernel and user halves), and user-half PGD entries are propagated alongside kernel-half entries. PCID and INVPCID are disabled at boot to simplify CR3 switching between replicas.

## Transparent Huge Pages

Hydra works with THP under any defrag policy. THP split operations free stale PTE replicas before repopulating, and huge PMD entries are broadcast to all replicas. Accessed and dirty bits are aggregated across replicas for correct huge page fault handling.

## Per-Node Page Table Cache

Hydra maintains a per-node free list of zeroed page table pages. Freed page table pages are returned to their node's cache instead of the buddy allocator when possible, and new allocations check the cache first. The cache reduces allocation latency and ensures node-local placement.

Cache statistics and control are available through `/proc/hydra/cache`. Writing `-1` drains all caches; writing a positive number `N` pre-populates `N` pages per node.

## procfs Interface

All runtime state is exposed under `/proc/hydra/`:

**cache** shows per-node cache statistics (count, hits, misses, returns). Writable for cache management.

**verify** enables or disables fault-time consistency verification. When enabled, every fault checks that all page table pages walked from the active CR3 reside on the expected node, and panics on mismatch. Write `1` to enable, `0` to disable.

**repl_order** controls how many PTEs are copied per replication fault. The value is log2, so `0` copies a single PTE and `9` (the default) copies up to 512 PTEs from the same page table page. Write a value between `0` and `9`.

**tlbflush_opt** controls the TLB flush optimization mode:

- `0` off, no optimization.
- `1` (default) precise node-mask filtering, single-PMD ranges only.
- `2` precise node-mask filtering, all ranges.
- `3` all-or-none filtering.

## License

GPL-2.0 (same as the Linux kernel).
