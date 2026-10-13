# KaOS graph system guide

KaOS runs WebAssembly nodes connected by directed channels. A Python
orchestrator authorizes graph changes; devices execute them, enforce resource
limits and report evidence of startup or cleanup.

This guide describes the `graph_core` implementation as of 12 October 2026.
The root-level legacy container/monitor implementation and
its LOAD protocol are separate build paths.

| Read | For |
|---|---|
| [Architecture](architecture.md) | Components, operation lifecycles, ownership and data flow |
| [Usage](usage.md) | Build the host, run an addition/removal, write guests and collect results |
| [Assumptions and limitations](limitations.md) | Static device enrollment, peer capacity and resource bounds |
| [Verification and testing](verification.md) | Run checks, interpret evidence and identify remaining qualification work |

## Implementation status

| Area | Status |
|---|---|
| Six graph operations | Implemented: node addition/removal with or without channels, edge addition/removal |
| Runtime and communication | Restricted WebAssembly Micro Runtime (WAMR) guests; CoAP management; best-effort application data; schema 4 |
| Reporting | Cumulative evidence, exact storage ACKs, bounded retirement and host RAM collector |
| Embedded execution | Local four-node ESP32 fixture passed; distributed Wi-Fi and collector qualification remain open |
| Application evaluation | Ventilation policies, Quality of Control (QoC) criteria and experiment parameters remain open |
| Final formal qualification | Combined implementation safety/liveness checks remain open |
