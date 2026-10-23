# KaOS graph system guide

KaOS runs WebAssembly nodes connected by directed channels. A Python
orchestrator authorizes graph changes; devices execute them, enforce resource
limits and report evidence of startup or cleanup.

This guide describes the `graph_core` implementation as of 23 October 2026.
The legacy root-level container/monitor implementation and its LOAD build path
were retired on 22 October 2026; use the graph host and graph simulator.

| Read | For |
|---|---|
| [Architecture](architecture.md) | Components, operation lifecycles, ownership and data flow |
| [Usage](usage.md) | Build the host, run an addition/removal, write guests and collect results |
| [Assumptions and limitations](limitations.md) | Static device enrollment, peer capacity and resource bounds |

## Current capabilities

| Area | Status |
|---|---|
| Six graph operations | Implemented: node addition/removal with or without channels, edge addition/removal |
| Runtime and communication | Restricted WebAssembly Micro Runtime (WAMR) guests; CoAP management; best-effort application data; schema 4 |
| Transport scheduling | Separate management/application sockets; bounded receive/transmit rounds; paced artifact requests |
| Reporting | Cumulative evidence, exact storage ACKs, bounded retirement and host RAM collector |
| Protocol experiments | Shared `kaos_host` runner, six-operation workload, concurrent authority calls and bounded directed-link impairment |
| Deterministic workloads | Local/distributed pipeline and feedback; explicit transform replacement; observed values and offline publication-to-GET budgets |
| Alternative channels | A-or-B startup, delayed joining, optional expiry/cleanup and explicit branch removal with continued selected-branch samples |
| Mixed dependencies | Either input plus required output gates startup; later output removal ends output service while input reads continue |
| Native imports | Bounded per-instance grants; checked names/types/memory; synthetic sensor and cancellation/removal verification |
| Application stalls | Real-WAMR producer/consumer fixtures exercise EMPTY, FULL and receiver drops; fallback behavior is application-defined |
| Embedded execution | Local four-node ESP32 fixture passed; network-host cross-build passed. These results do not establish distributed Wi-Fi capacity. |
| Formal evidence | Two bounded composed configurations pass; results apply to the modeled finite scenarios |
