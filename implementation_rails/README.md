# Implementation records

For current behavior and runnable commands, start with [the guide](../guide/README.md).
This directory retains design context and implementation history, including
filenames, capacities and commands that were valid only at a particular phase.

## Current planning and constraints

- [Implementation plan](graph_operation_implementation_plan.md): current status,
  open decisions, remaining engineering/qualification work and chronological progress.
- [Simplification plan](graph_core_simplification_plan.md): completed refactoring
  and the pending application-CoAP contract.
- [Native imports plan](graph_native_imports_plan.md): native-function access.
- [Verification inventory](graph_operation_verification_inventory.csv): coverage
  and remaining obligations.

## Historical records retained in place

P2–P7 contracts, the P4 memory record and completed sections of the implementation
plan remain at their original paths. They combine historical qualification with
design constraints still referenced by the current implementation; moving or
deleting them solely because a phase is complete would obscure those references.
Use the guide for current build commands, especially after retirement of the
P3/P4 executable aliases.

Verification JSON summaries and their `verification/graph_core/results/` evidence
remain together at their existing paths. Old source paths and hashes describe
the original run, not the current checkout. They are deliberately excluded from
generated-cache cleanup and are not ignored by Git.

See [repository cleanup](repository_cleanup.md) for the retirement boundary.
