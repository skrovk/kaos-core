# Graph operation design principles

Derived on 2 October 2026 from the agreed answers to [Q01–Q07](graph_operation_design_questions.md). These principles summarize the reasoning behind those decisions and guide later choices; they do not introduce additional requirements or settle unanswered questions. The question document remains the detailed decision record, including provisional choices and accepted limitations.

Here, O is the orchestrator, Kc coordinates an operation, and Ki is a participating device's KaOS. A device may perform both KaOS roles. The DB is the designated evidence/log collector.

## 1. Design against measured embedded constraints

Make resource cost part of the design from the first prototype. Establish finite, configurable limits and qualify them on the actual target, including application execution, wireless networking, control, reporting and cleanup overhead. Supported capacity must account for peak overlap during reconfiguration.

The current baseline is the original ESP32 TTGO T-Display without PSRAM, using Wi-Fi, with at least two resident WebAssembly nodes connected by a local channel. This is a minimum workload, not a measured upper limit. Simulation can establish functional behavior within its model; feasibility claims require physical-board measurements, including distributed placements when assessing networked channels.

Basis: [Q01](graph_operation_design_questions.md#q01).

## 2. Preserve local progress when management is unavailable

Existing applications and admitted work should continue within their resource, dependency and deadline constraints when O or the DB becomes unreachable. Reporting and archiving must use bounded resources without becoming prerequisites for operation completion or consuming cleanup capacity.

This autonomy coexists with explicit management authority: O still allocates identities and authorizes graph changes. Devices continuing after O loses state does not imply that a replacement O can safely resume issuing commands. Recovery of authority remains a separate extension under the crash-stop baseline.

Basis: [Q02](graph_operation_design_questions.md#q02), [Q04](graph_operation_design_questions.md#q04), [Q06](graph_operation_design_questions.md#q06).

## 3. Give decisions clear owners and keep coordination local where possible

O owns the authorized graph, including intended bindings whose commands are still in flight. Devices own actual resource admission, object state and evidence of local closure. A local registry scan cannot establish complete authorized incidence across the system.

Serialize the short decisions that would otherwise conflict: fixing removal scope against new authorizations, and checking, reserving and recording local admission. After admission, independent runtime, endpoint and remote preparation can proceed concurrently. Kc's acceptance does not promise that every Ki has capacity, and remote preparation need not await successful local preparation.

Basis: [Q04](graph_operation_design_questions.md#q04).

## 4. Budget for the entire admitted lifecycle

Admission must cover the resources needed to complete or clean up the authorized work, including partial allocations, pending evidence, optional work and late closure reports. Capacity needed to preserve unfinished obligations cannot be treated as freely reclaimable history. When protected state exhausts the relevant capacity, defer additional admissions requiring it.

Apply this reasoning to metadata as well as RAM and buffers. Counter widths must fit the number of valid updates within their scope. The provisional 8-bit revisions require validated update bounds, enforced before preparation or dispatch and including failure and cleanup paths. Checked arithmetic detects violations; silently wrapping a counter is not an exhaustion policy. Concrete limits remain profile and schema deliverables.

Basis: [Q01](graph_operation_design_questions.md#q01), [Q04](graph_operation_design_questions.md#q04), [Q06](graph_operation_design_questions.md#q06), [Q07](graph_operation_design_questions.md#q07).

## 5. Use stable identities and add semantic metadata only when needed

Give each concrete instance or binding an immutable identity that survives changes in its use and the retirement of its creation report. Distinguish an operation from the object it targets and from internal storage handles. For example, removal 57 can target the instance created by addition 41; reusing a local record slot must not redirect old work to its new occupant.

Reuse existing identifying information where its meaning is sufficient: the agreed design derives instance identity from the creating operation and distinguishes API types without adding another allocator. Service membership, placement and deployment grouping do not supply object lifetime or safe identity-reuse boundaries. Additional pipeline/group identities are deferred until a concrete requirement needs semantics that the existing graph and contracts do not express.

Basis: [Q03](graph_operation_design_questions.md#q03), [Q05](graph_operation_design_questions.md#q05).

## 6. Make each acknowledgement state exactly what it establishes

Authorization, admission, preparation, execution, closure and evidence storage are distinct facts. Transport receipt alone establishes neither application completion nor storage of an operation result. A report-storage ACK must identify the exact evidence and revision recorded; an older ACK cannot retire a newer report.

Delegating storage transfers a specific responsibility. The designated DB's ACK can let a device reclaim a detailed report before O receives it, but does not establish O's knowledge or discharge cleanup obligations. The DB must retain accepted evidence until O acknowledges it and decline new writes when it cannot honor that promise. Persistence across DB failure is a separate, currently unselected guarantee.

Basis: [Q04](graph_operation_design_questions.md#q04), [Q06](graph_operation_design_questions.md#q06), [Q07](graph_operation_design_questions.md#q07).

## 7. Validate evidence within its source, operation and lifecycle

Compare revisions only within the stream they identify. Object evidence and Kc's aggregate operation report have separate operation-scoped streams; their counters do not establish a global order. Arrival order, transport message IDs and larger operation IDs cannot substitute for evidence ordering or prove current remote health.

Identity, issued-work and lifecycle checks still apply to a higher revision. Validate a delayed callback before allowing it to change state and publish evidence. Retransmission preserves the original revision. Identical repeats have no repeated effect; conflicting logical contents under the same identity and revision are rejected and logged without automatically failing the operation or replacing accepted state. This comparison uses retained evidence and does not require an indefinite archive of every revision.

Basis: [Q03](graph_operation_design_questions.md#q03), [Q05](graph_operation_design_questions.md#q05), [Q07](graph_operation_design_questions.md#q07).

## 8. Preserve uncertainty until the relevant evidence resolves it

Missing reports, timeouts, an empty local registry or an absent archive entry do not prove that work never ran or that all resources were released. O retains possible incidence for sent requests with unknown outcomes. Devices may release actually closed local resources while O still lacks the evidence needed to release its corresponding bookkeeping.

Historical outcome and current state also have different meanings. Successful addition does not establish continuing application health, and a successful parent can still have optional work pending. `Unresolved` preserves missing closure obligations and can later be resolved by valid evidence. Results reporting `Succeeded`, `FailedClean` or `Unresolved` must be interpretable without earlier progress reports, including their required history and remaining obligations.

Basis: [Q02](graph_operation_design_questions.md#q02), [Q04](graph_operation_design_questions.md#q04), [Q06](graph_operation_design_questions.md#q06), [Q07](graph_operation_design_questions.md#q07).

## 9. Protect operational state while reclaiming eligible history

Keep the state needed for live ownership, ongoing work, truthful outcomes and unresolved cleanup. Detailed report copies and processed intermediate messages can have shorter lifetimes once their necessary facts are preserved. This supports continued device functionality during management outages without assuming unlimited evidence storage.

Under the agreed device policy, reclaim eligible acknowledged copies first, then the oldest eligible unacknowledged `Succeeded` or `FailedClean` report under pressure. Preserve pending and unresolved state even when its parent's outcome is successful. The DB has its separate retain-until-O-ACK commitment from principle 6; device pressure eviction does not override that promise.

Revision metadata remains while valid updates or retained evidence need it. Reclaim it only when locally established settled work, the retention policy and local access lifetimes permit. An ACK does not reset a continuing counter, and Kc's completion alone does not establish Ki's eligibility to reclaim its state.

Basis: [Q04](graph_operation_design_questions.md#q04), [Q06](graph_operation_design_questions.md#q06), [Q07](graph_operation_design_questions.md#q07).

## 10. Preserve authority and lifetime across retries and queries

Retries retain the original request identity and act against its existing state. Queries observe retained results or current state with their meaning distinguished; they do not authorize work, reset deadlines or restart reclaimed revision streams. The provisionally agreed missing-result response makes the information gap explicit.

When a sent operation's report is unavailable, automatically resending construction can repeat effects after history has been lost. If O intends the objects to be absent, it must explicitly authorize cleanup or reconciliation of the intended scope. Fresh management intent has its own operation identity and preserves outstanding obligations.

Basis: [Q03](graph_operation_design_questions.md#q03), [Q04](graph_operation_design_questions.md#q04), [Q06](graph_operation_design_questions.md#q06), [Q07](graph_operation_design_questions.md#q07).

## 11. Make resource–guarantee tradeoffs explicit and test their consequences

Finite storage and continued operation during outages constrain the guarantees the implementation can offer. State the chosen tradeoff rather than treating a likely benign workload as proof. Q05 explicitly accepts that stale ADD/PREPARE requests can recreate removed or rejected objects after the necessary rejection history is discarded. Non-recycled IDs and short operation deadlines do not eliminate this risk.

Analyze resource consumption, peripheral contention, duplicate or incorrect data and repeated application effects. Possible orchestrator reconciliation is a conditional mitigation, not guaranteed prevention or a way to undo all effects. Live/pending identity checks, admission limits, lifecycle guards, complete cleanup and memory-safe callbacks remain required.

Keep implementation claims aligned with this limitation: stronger reference protocols and formal models do not prove a deliberately weaker implementation safe after history loss. Fault-injection tests should characterize the accepted gap as well as verify retained-state safeguards; a low observed failure rate does not establish the missing guarantee.

Basis: [Q05](graph_operation_design_questions.md#q05), supported by [Q01](graph_operation_design_questions.md#q01) and [Q06](graph_operation_design_questions.md#q06).

## 12. Extend a concrete baseline without assuming future guarantees

Choose mechanisms for requirements that exist and leave explicit extension points where additional semantics may later be useful. The crash-stop baseline, immutable identities, direct channel relationships and designated collector are concrete starting points. They do not implicitly provide reboot recovery, orchestrator takeover, atomic pipeline operations or pooled persistent storage.

Keep provisional choices visible. Revision widths still need numerical qualification; CoAP-style reliable datagrams are the working assumption, while standard CoAP framing versus a derived protocol, codecs and libraries remain unselected. Rebuilding O's observed view, restoring its authority and pooling device storage are future design work. Extensibility should allow those choices later without claiming their guarantees now.

Basis: [Q02](graph_operation_design_questions.md#q02), [Q03](graph_operation_design_questions.md#q03), [Q04](graph_operation_design_questions.md#q04), [Q06](graph_operation_design_questions.md#q06), [Q07](graph_operation_design_questions.md#q07).
