# Assumptions and limitations

## Device enrollment is fixed for a running host

`core_host_config.peers` lists the device IDs enrolled to participate in graph
operations with this host. It must include the local device exactly once.
The authority and optional collector have separate identities and are not
entries in this list.

Enrollment is supplied at initialization. `core_admission_configure()` rejects
reconfiguration once the admission pool is configured; the current host API
does not support adding or removing enrolled devices while it is running.
Changing membership therefore requires restarting with a new configuration.
Nodes and channels can still be added and removed dynamically among enrolled
devices.

The list describes configured membership, not current network reachability or
existing channel connections. A peer can be enrolled before any channel uses
it, and it remains enrolled if it becomes unreachable. Network addresses are
configured separately in the platform's routing table. Admission requires
both owners of a requested channel to be enrolled; deployments must configure
the participating hosts and their routes consistently.

For example, host A can enroll `{A, B, C}` while sharing a channel only with B.
It can later accept a channel involving C without changing enrollment. Adding
a previously unenrolled device D requires a configuration change and restart
on the affected hosts.

## Peer capacity is bounded

`CORE_MAX_PEERS` is currently **8**, including the local device: each host can
configure at most **7 remote device peers**. This is a compile-time limit,
not a limit that can be raised through runtime configuration. It applies to
each host's enrollment list; a deployment in which every device enrolls every
other device is consequently limited to eight participating devices.

Increasing this bound requires changing and rebuilding the implementation,
and checking the resulting memory use and platform routing capacity. The
authority and collector do not consume peer entries, but need their own
configured routes.

See [host configuration](../../graph_core/include/kaos_host.h),
[peer limit and enrollment contract](../../graph_core/include/kaos_admission.h)
and [enrollment validation](../../graph_core/kaos_admission.c).

## Graph capacity is also bounded

Peer enrollment does not guarantee that a graph operation will fit. Each host
also has fixed capacities for node reservations, endpoint reservations,
admission records, ports and channels per request. The selected build profile
sets these bounds in [kaos_limits.h](../../graph_core/include/kaos_limits.h).
The configured memory budget imposes an additional admission limit.

An operation can therefore be rejected for insufficient capacity even when
all participating devices are enrolled. Cleanup and retained operation
evidence can continue to occupy capacity after execution stops.

See [verification and testing](verification.md) for the remaining platform and
system qualification work.
