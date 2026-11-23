# nftables backport assessment

Assessment only. No modules were built or loaded and no router flash was written.

A limited module-only nftables backport is a plausible, narrower research target than replacing the complete BCM6858 kernel and restoring Ethernet/Wi-Fi. It does not imply that a recent nf_tables subtree can be copied into Linux 4.1 unchanged, or that a successful build is runtime compatible.

The extracted stock config has CONFIG_NF_TABLES disabled. The available Broadcom 4.1 source in work/os/kbuild/kernel/linux-4.1/include/net/net_namespace.h conditionally embeds struct netns_nftables nft inside struct net when CONFIG_NF_TABLES or CONFIG_NF_TABLES_MODULE is defined. net/netfilter/nf_tables_api.c directly accesses net->nft. Enabling the option only in an external-module build would therefore change the assumed struct layout relative to that configuration. The exact vendor layout still needs verification; this source evidence establishes an ABI concern, not a tested crash. Existing module ABI probes covered other structures, not this nftables-enabled layout.

A candidate approach is to retain all original kernel structure layouts and keep nftables state privately in the module, with a shared accessor for family/expression modules. The kernel has namespaces disabled, so initially supporting only the initial network namespace reduces the scope. Whether the required exported symbols and registration hooks suffice must be checked before loading a module. The available nfnetlink source already has the nftables subsystem/group IDs and registration machinery, which is encouraging but not a complete compatibility proof. Existing nf_conn layout corrections from docs/05 remain necessary for modules that touch conntrack.

Recommended feasibility gates:
1. Compile a minimal core using the original kernel headers/configuration layouts, module-private state and checked exported symbols. Do not rebuild the full kernel or assume missing BCM6858 BSP source is available.
2. From a temporary RAM boot, load/unload, create/delete a table and counter, then prove counters reflect real packets without disrupting management.
3. Validate basic IPv4/IPv6 filtering, connection tracking and NAT with compatible nft userspace.
4. Inventory the exact firewall4-generated expressions/chain types required. Backport only needed features or adapt the generator where feasible. Inet-family stateful NAT is a post-4.1 feature (Linux 5.2); netfilter hooks, NAT/conntrack APIs, set implementations and transaction semantics may introduce further dependencies. Basic nftables support alone does not establish current firewall4 compatibility.
5. Check Broadcom flow-cache/Runner behavior, especially whether filtering decisions and rule changes remain effective for accelerated traffic. Validate enforcement independently from throughput and successful ruleset loading.

Replacing nft userspace alone supplies no kernel implementation. A full modern nf_tables transplant also needs dependency and security-fix maintenance; security fixes that depend on absent kernel infrastructure may need adaptation. Scope the target by required firewall behavior rather than promising every feature of a current kernel.

Sources: the local stock configuration and Broadcom 4.1 source above; https://www.netfilter.org/projects/nftables/manpage.html (nft is a userspace interface to kernel nf_tables and documents inet NAT availability); https://docs.kernel.org/process/stable-api-nonsense.html (kernel internal ABI/layout constraints).
