# Agent guidance

## Project layout

| Folder | Role |
| --- | --- |
| `graph_core/` | Source code of KaOS: graph operations, host execution, communication, and runtime integration. `include/` contains its interfaces, `runtime/` contains WAMR integration, and `ports/` contains POSIX and ESP-IDF platform support. |
| `guide/` | Human-readable project documentation. Explain architecture, usage, limitations, and verification clearly for people using or maintaining KaOS. Start with `guide/README.md`. |
| `implementation_rails/` | Implementation plans and history for agents carrying out implementation. Includes design decisions, contracts, open questions, phase records, and verification inventories. These materials do not have to be human-readable; prioritize precise, useful context for implementation agents. |
| `verification/` | Verification programs, fixtures, models, scripts, and recorded evidence. `graph_core/` contains host and ESP-IDF checks, guest workloads, capacity probes, and results for the graph implementation. |

The legacy root implementation and vendored unicoap stack were retired.
Current production C sources live under `graph_core/`.

Hidden directories hold repository or local tooling state: `.git/` is Git
metadata; `.agents/`, `.codex/`, and `.aws/` are local configuration directories;
`.ruff_cache/` is generated lint cache. They are not project source or
documentation destinations.

## Where to put changes

- Implement graph-system behavior in `graph_core/` and keep corresponding
  verification in `verification/graph_core/`.
- Put explanations intended for people in `guide/`. Update them when changes
  affect documented behavior or usage. Keep implementation plans, unfinished work
  and open design questions in `implementation_rails/`, not in the guide.
  Document current limitations in the guide without turning them into a backlog.
- Put implementation planning, agent handoff context, and implementation history
  in `implementation_rails/`. Consult relevant plans and contracts before changing
  the implementation, and record decisions and progress there as appropriate.
- Keep generated build and cache files separate from source changes.
- Follow the C formatting and readability rules below and `.clang-format`.

## C formatting and readability

Use the repository's [.clang-format](.clang-format) for first-party C sources
and headers. The pinned formatter is **clang-format 14.0.4**, available in the
current development environment at `/opt/wasi-sdk/bin/clang-format`. Use the
same version in the editor and automated checks; upgrade it as a separate,
reviewed formatting change.

Use four spaces, no tabs, a 100-column target, and a separate line for function
opening braces. Control-flow opening braces stay with the condition. Keep
include order, string literals and comments intact. Put guarded statements and loop bodies on
their own lines; do not pack several independent statements onto one line.
Use braces for multi-statement bodies and wherever omitting them obscures which
condition or loop owns a statement. A simple one-statement guard may omit braces.

### Semantic grouping

The formatter handles layout; the author chooses meaningful statement groups.
Use one empty line between phases such as validation, lookup, reservation,
publication, execution and cleanup. Keep a declaration beside the checks and
operations that use it. Separate consecutive loops when they serve different
purposes, especially a validation pass followed by a mutation pass. Do not add
blank lines between every assignment, or between a comment and its subject.

Prefer early `return` for rejected input and early `continue` for irrelevant loop
entries when they let the main operation read at a shallower indentation level:

```c
for (size_t i = 0; i < count; ++i) {
    entry *current = &entries[i];
    if (!current->occupied)
        continue;

    if (!ready(current))
        return false;

    publish(current);
}
```

Aim for no more than two or three nested control-flow levels in ordinary code.
This is a reading aid, not a hard limit: channel/role iteration, state alternatives
and resource scopes can justify deeper nesting. Do not introduce helper modules,
temporary state or callbacks solely to meet an indentation target. Retain a
short `if`/`else` when it communicates two related outcomes more clearly.

### Preserve behavior while improving layout

- Preserve condition evaluation order and short-circuiting, especially parser
  reads, callbacks, clock samples and capacity checks.
- A guard may skip only the statements guarded before the edit. In a loop,
  `continue` must not bypass an existing common tail, unlock, free or notification.
- Keep lock acquisition/release, allocation ownership, publication, cleanup and
  error precedence in their existing order. Do not move declarations or reads
  across a lock boundary for visual convenience.
- Keep bounded loops and lifecycle passes bounded. Formatting must not introduce
  retries, alter deadlines, reorder records or change wire formats.
- Review semantic edits separately from mechanical formatting. Compare against
  the actual working-tree baseline and run the affected verification profiles.

### Commands and scope

From the repository root:

```sh
KAOS_CLANG_FORMAT=/opt/wasi-sdk/bin/clang-format
"$KAOS_CLANG_FORMAT" --version

## Format graph implementation sources and headers.
rg --files graph_core -g '*.c' -g '*.h' -0 |
    xargs -0 "$KAOS_CLANG_FORMAT" -i

## Check the same files without modifying them; suitable for CI.
rg --files graph_core -g '*.c' -g '*.h' -0 |
    xargs -0 "$KAOS_CLANG_FORMAT" --dry-run --Werror
```

Configure the editor to use that binary and repository style for format-on-save.
Production C code lives in `graph_core/`. Exclude external dependencies,
generated build/cache files and saved
verification snapshots from bulk formatting. Existing test sources remain
protected by the project's explicit approval requirement, including changes
that would only reformat them. New tests should follow this style.
