# Native import allowlist: implementation-plan addendum

Decision recorded on 20 October 2026. Status: implemented with host verification
and ESP32 cross-build evidence; physical native execution and target resource/
timing qualification remain open. The implementation record below distinguishes
the original design requirements from measured results.

This supplements the [graph operation implementation plan](graph_operation_implementation_plan.md),
particularly its application interfaces, R01 ABI qualification and application
capability checks. It was recorded separately while P8 implementation continued. It does not
mark any P8 gate complete or make this increment a prerequisite for existing
deterministic workload verification.

## Agreed minimal interface

Retain the legacy approach of declaring which named native functions a container
may import. KaOS receives an explicit per-node authorized import list and links
permitted imports against a fixed host registry of native functions and ABI
signatures.

Illustrative declaration and binding, not a selected wire encoding:

```text
Node declaration:
    imports: ["temperature_read"]

Host registry:
    (module name, "temperature_read") -> native function + ABI signature

Guest:
    imports temperature_read()
```

"Handles" in this decision means import identifiers/names, not guest-visible
resource objects or native pointers. The complete linkage identity includes
the import module and function name; the expected signature comes from the
trusted host registry.

KaOS controls which functions a container may call. Each native function owns
its hardware or simulation behavior, argument interpretation, result encoding,
errors and timing contract. The allowlist grants access at function granularity:
if a permitted function accepts a sensor ID, the list alone does not constrain
the IDs the caller may supply. Any narrower argument-level permission belongs
to that native implementation.

Do not introduce a generic resource_read/resource_call interface, resource
handle table, discovery, generic cache, subscription system or Matter dependency
for this increment. Those were discussed alternatives, not selected work.
Preserve the current graph guest entrypoints and lifecycle/channel ABI; this
decision does not require compatibility with legacy setup/main binaries.

## Enforcement and lifecycle requirements

- The authorized node declaration supplies the allowlist. A guest's own import
  section is a request to link, not permission to grant itself access.
- Use a fixed registry supplied by trusted host code. Requests select registered
  names; they cannot supply native addresses or load arbitrary native code.
- Validate every guest import against that node's allowed set and the registry
  before instantiation or guest execution. Check module, name and actual Wasm
  function type. Reject ungranted, unknown or incompatible imports.
- Runtime-wide WAMR registration is linkage machinery, not per-container
  authorization. Registering a function for one container must not enable a
  second container to import it without a grant. The legacy loader's per-module
  registration TODO must not be carried forward as an isolation guarantee.
- Keep the allowed set bound to the exact node instance for its lifetime.
  No dynamic grant/revocation operation is required for this first increment.
- Native functions must validate guest arguments, including memory ranges for
  pointer arguments. Guest pointers must not outlive their calls. Access to
  instance-specific state must use trusted caller attribution.
- Calls must have bounded execution, or bounded cancellation where needed, so
  they cannot prevent control/cleanup progress. Existing disposal and CLEANED
  conditions still require imports and any tracked native work to have drained;
  terminating Wasm alone is insufficient. Do not introduce asynchronous native
  work without explicit ownership and cleanup.

## Implementation work

1. Define a bounded import-list representation in the node declaration and
   authorized request path. Carry it unchanged through orchestration, decoding,
   admission and runtime preparation; account for its storage. Select and record
   the encoding, import-count/name bounds and schema compatibility treatment.
2. Define the host registry and register available functions at runtime startup.
   Record how existing standard `kaos` imports relate to the additional native
   allowlist, preserving current guest behavior. Choose the native import
   namespace explicitly rather than silently assuming legacy `env`.
3. Extend the restricted artifact validator and preparation path to apply the
   node-specific allowlist and signature checks before instantiation. Preserve
   existing memory, no-start/constructor, no-WASI and no-guest-thread restrictions.
   An import grant must not bypass the restricted profile.
4. Add one bounded native sensor fixture and a guest that imports it. Keep
   sensor implementation details outside the graph protocol core. Host and
   embedded implementations of a shared import must honor the same guest ABI.
5. Document the implemented declaration, import syntax, native-function contract
   and limitations in `guide/` when implementation lands. Record measured costs
   and qualification evidence separately from this design decision.

Implementation touchpoints are `graph_core/runtime/profile.c`,
`graph_core/runtime/kaos_wamr.c`, the node request/admission/runtime interfaces,
and the sibling Python node descriptor/encoder. The increment preserves the
existing P8 paths and their tests. Exact sensor policy, acquisition scheduling, freshness, plant
parameters and QoC criteria remain governed by their existing application gates.

## Acceptance checks for the increment

- A declared, registered, correctly typed import links and runs.
- An undeclared import fails before guest/native execution, including when
  another resident node is allowed to use that globally registered function.
- Unknown names, wrong modules, wrong signatures, malformed declarations and
  exceeded list bounds fail without leaking reservations.
- Existing guests using only the standard `kaos` imports continue to work.
- Removal during a native call respects bounded shutdown and drains ownership
  before disposal; a replacement node cannot inherit the previous node's grants.
- The sensor fixture checks valid results and invalid guest arguments through
  the real Wasm/native interface. Qualification distinguishes ABI/linking
  success from physical sensor availability or application correctness.

## Implementation record: 20 October 2026

Selected the fixed `kaos_native` namespace and at most four unique 1–32-byte
printable ASCII names. Each admission record and runtime slot owns its copy.
Schema 4 retains the original empty-grant encoding; a nonempty final array
extends node bodies from 9/11 to 10/12 fields. Older decoders reject the larger
shape. Existing total request limits remain in force.

The runtime adapter adds an optional `prepare_with_imports` callback; a grant
requires it and cannot fall back to unrestricted preparation. Restricted
profile checks enforce names/modules, then WAMR's loaded-import introspection
checks actual function types against the trusted registry before instantiation.
This avoids implementing a second Wasm type-section parser. Standard `kaos`
imports also receive actual-type validation. There are no new guest handles,
dynamic registry operations or asynchronous native jobs.

The shared synthetic `fixture_temperature_read` import uses a checked i32
offset and four-byte output. Sensor 0 writes 23000 immediately; sensor 1 waits
cancellably for 100 ms and writes 24000. Both return 4 on success. Native
execution retains the worker and arena; cancellation releases the mailbox
while waiting and disposal occurs only after the call returns. These values
qualify the ABI, not a physical sensor.

New checks exercise authorized request encoding/decoding, malformed bounds,
copied grants, neighboring-instance isolation, wrong module/name/type, invalid
guest arguments, replacement without inherited grants, and authorized removal
while the native call is active. Existing assertions remain unchanged. The
only existing-test edit is the explicitly approved designated-field initializer
in `p5_test.c`.

The four-node ESP32 compiler layout adds 136 B per admission record (3264 B
for the admission pool) and 544 B across runtime slots; the global 32 KiB WAMR
arena is unchanged. The firmware cross-build passes. Physical heap/stack
peaks, timing and sensor availability remain unqualified.

See [the guide](../guide/native-imports.md) for the API and ownership walkthrough
and [continuation verification](../verification/graph_core/continuation_verification.json)
for commands, logs, hashes, review findings and the exact acceptance scope.
