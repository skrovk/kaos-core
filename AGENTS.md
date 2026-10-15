# Agent guidance

## Project layout

| Folder | Role |
| --- | --- |
| `graph_core/` | Source code of KaOS: graph operations, host execution, communication, and runtime integration. `include/` contains its interfaces, `runtime/` contains WAMR integration, and `ports/` contains POSIX and ESP-IDF platform support. |
| `guide/` | Human-readable project documentation. Explain architecture, usage, limitations, and verification clearly for people using or maintaining KaOS. Start with `guide/README.md`. |
| `implementation_rails/` | Implementation plans and history for agents carrying out implementation. Includes design decisions, contracts, open questions, phase records, and verification inventories. These materials do not have to be human-readable; prioritize precise, useful context for implementation agents. |
| `verification/` | Verification programs, fixtures, models, scripts, and recorded evidence. `graph_core/` contains host and ESP-IDF checks, guest workloads, capacity probes, and results for the graph implementation. |
| `include/` | Headers for the root-level legacy KaOS implementation. |
| `port/` | Platform support for the legacy implementation, including x86 and Xtensa code and platform interfaces in `include/`. |
| `test/` | Tests and supporting headers/scripts for the legacy implementation. `build/` contains generated build artifacts. |
| `unicoap/` | CoAP implementation, with protocol drivers, public and private headers, IANA definitions, and its own documentation. |

The root-level C sources and build configuration belong to the legacy
container/monitor implementation, which has a separate build path from
`graph_core/`.

Hidden directories hold repository or local tooling state: `.git/` is Git
metadata; `.agents/`, `.codex/`, and `.aws/` are local configuration directories;
`.ruff_cache/` is generated lint cache. They are not project source or
documentation destinations.

## Where to put changes

- Implement graph-system behavior in `graph_core/` and keep corresponding
  verification in `verification/graph_core/`.
- Put explanations intended for people in `guide/`. Update them when changes
  affect documented behavior or usage.
- Put implementation planning, agent handoff context, and implementation history
  in `implementation_rails/`. Consult relevant plans and contracts before changing
  the implementation, and record decisions and progress there as appropriate.
- Keep generated build and cache files separate from source changes.
