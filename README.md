# KaOS

KaOS executes WebAssembly nodes connected by directed channels on bounded
devices. Start with the [project guide](guide/README.md) and
[build instructions](guide/usage.md).

| Directory | Contents |
| --- | --- |
| `graph_core/` | Production C implementation, interfaces, runtime and platform support |
| `guide/` | Architecture, usage and verification documentation |
| `verification/` | Tests, guest workloads, models and recorded evidence |
| `implementation_rails/` | Design contracts, implementation plans and history |

The root-level container/monitor implementation, its tests and the unused
unicoap stack were retired on 22 October 2026. Use `CORE_BUILD_HOST` and
`kaos_host`; the P2 artifact fixture remains for regression tests.
The sibling `sim_env` graph runners are the supported simulation entry points.
Old LOAD scenarios and guests use a different ABI and are not migrated by this
cleanup. Historical records retain the filenames and commands used in their runs.
