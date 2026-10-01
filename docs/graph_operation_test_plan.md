# Graph operation test catalogue

This catalogue specifies future acceptance tests for the [implementation plan](graph_operation_implementation_plan.md). It is not a claim that the tests are implemented or passing. It covers the six operations, every V2 F0–F15/C1–C6 case, the existing finite checks and their witnesses/counterexamples, application examples, and additional implementation hazards. Read the [open questions](graph_operation_design_questions.md) before fixing decision-dependent oracles.

Existing feature implementations may be replaced completely. The oracles below concern protocol behavior, resource bounds and workload meaning; they do not require legacy LOAD compatibility, old guest entrypoint names, a particular codec/runtime, or the existing host patch. Translate fixtures to the selected API/ABI and replace tests tied only to discarded internals. Keep test IDs stable for the formal inventory. Tests motivated by old-code hazards still apply where the same lifetime, identity or overload risk exists in the replacement.

Source abbreviations SH, V2, IN, VN, VG, KD, FT and SIM refer to the plan's linked source register. **D** means described in the materials; **F** means related finite-model coverage exists, not that the concrete test is proved; **G** means a documented implementation/evaluation gap; **N** means an additional explicit test identified in this review. A test can be both F and G when an abstract invariant leaves the concrete mechanism unverified. **U** denotes deterministic core/codec tests, **S** real KaOS simulator tests, **W** application tests, and **H** embedded qualification.

## Fixtures and common assertions

Use K0 as live coordinator unless the case explicitly crashes it; K1/K2 own remote endpoints. N is a new instance; A/B/C are complete declared channels, not individual endpoints. Established peers have successfully added runtimes. Keep a running unrelated node U and an unrelated channel Z in every scoped cleanup fixture, and assert their identities, allocations and data service are unchanged by the operation.

The harness must independently pause: admission preflight/reservation, each allocation, preparation completion/publication/acceptance, commit, receiver and sender authorization/activation/report, startup dispatch/execution/STARTED emission/acceptance, cleanup selection/send/owner receipt/quiescence/closure/report acceptance, and cutoff handling. Inject lost/delayed/duplicate/reordered **local callbacks as well as network messages**. Capture owner truth and accepted observations separately; owner truth is a test oracle, not hidden knowledge supplied to Kc.

For every applicable test assert: at most one dispatch/execution per addition; no data access before its role is active; no construction after owner-recorded removal; complete cleanup scope; unchanged unrelated objects; no deadline reset; request/creation/instance/owner correlation; bounded allocations including partial work; correct retained history/outcome. Any event gap required for the oracle makes the test inconclusive/failed instrumentation, not a pass. Cleanup timeouts must identify missing objects rather than assert physical leaks or closure.

Expand each applicable case over outgoing, incoming, co-located and mixed placement, several endpoints on one host, and single- versus multi-service identities. Test both orderings of each two-event race, including same-timestamp events. Use no-fault controls and verify that the fault actually reached the intended boundary. Exercise zero/one/maximum channel counts and `limit−1`, `limit`, `limit+1` for resource/length fields. Exhaustively enumerate small finite predicate inputs and short adversarial schedules; use pairwise parameter coverage and seeded stress at the chosen maximum scale. Record instantiated variants; the row count is not the executed-test count.

## Baseline and environment qualification

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| BASE01 | Build the new shared core through host and selected ESP-IDF ports from the same revision/profile. | Both compile the same protocol implementation without a behavior-changing host patch; source lists and port contracts resolve; record platform differences. | ENV08/09; G; S/H |
| BASE02 | Run retained or translated unit/port/process tests plus new contract tests, then sanitizer checks where supported. | Required behavior has coverage; discarded internal/LOAD expectations are replaced, not counted as required compatibility failures; report test dispositions and skips. | SIM/replacement policy; D/G; S |
| BASE03 | Change a core/port/configuration input after building; attempt to run the old binary. | Provenance guard requires rebuild or explicitly identifies stale evaluated source, whether builds use direct sources or copies. | ENV10; N; S |
| BASE04 | Run host with the selected embedded limits and compare boundary manifests to the unconstrained profile. | Host rejects at the same configured limits; larger host defaults cannot mask target admission failures. | Q01/ENV07; G; S/H |
| BASE05 | Translate smoke, baseline, colocated, independent-loss, burst-loss, outage, node-faults and operations scenarios to the new API/ABI. | Preserve workload/fault intent and honest observations, with new protocol expectations; old command/snapshot formats are not required. Restoration is not autonomous recovery or protocol closure. | SIM scenarios; D; S |

## Successful operation paths

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| HP01 | Add N with empty manifest and P=true on a device with established empty incidence. | N prepares inertly, commits, dispatches once, executes and confirms; Succeeded only after accepted STARTED; empty channelStatus. | No-edge add; F; U/S |
| HP02 | Remove the successful no-edge N. | Disable execution, quiesce, release runtime/registry, accept CLEANED, then Succeeded. No endpoint work. | No-edge removal; F; U/S |
| HP03 | Add one remote edge between established nodes. | Both endpoints Prepared before commit; receiver ACTIVE accepted before sender activation; Succeeded after both ACTIVE; no runtime start/restart. | Edge add; F; U/S |
| HP04 | Add the same shape with co-located endpoints. | Same role ordering and separate endpoint evidence; correct shared-storage ownership; local data bypasses relay. | Edge add/VG; F; U/S |
| HP05 | Remove a remote active edge while both peers keep running. | Both endpoint CLEANED required; node identities, execution counts and unrelated edges preserved. | Edge remove; F; U/S |
| HP06 | Remove co-located endpoint pair with one shared buffer. | Two obligations discharged, storage freed exactly once; no retained alias can access it. | M6/IN; F/G; U/S |
| HP07 | Add N with one required outgoing channel A. | Complete preparation permits commit, complete activation permits one start, matching STARTED permits success. | VN safety/startup one; F; U/S |
| HP08 | Add N with one required incoming channel A; send sample after activation but before N execution. | Local receiver activates first; sample may buffer; N cannot Get until it runs. | V2 §4; F/G; U/S |
| HP09 | Add N with A or B; hold B incomplete. Repeat with B sufficient and A held. | Succeeded through either complete channel without waiting for the other; original optional ownership/deadline retained. | V2 sequence/VN witness; F; U/S |
| HP10 | Add N with `(A or B) and C`, where C is an output and alternatives are inputs. | Both `{A,C}` and `{B,C}` allow start; A or B alone does not. | VN mixed; F; U/S |
| HP11 | Add N with P=true and nonempty optional channels, all initially silent. | Runtime can succeed without channels; this is distinct from HP01; all optional records continue independently. | VN true optional/VG witness; F; U/S |
| HP12 | Add N with `(A and B) or (A and C) or (B and C)` across shared/mixed hosts. | Any complete pair suffices; third channel can continue; no all-channel join. | VN Boolean pairs; F; U/S |
| HP13 | Remove N with incoming A, outgoing B, and unrelated Z. | Scope is N and four endpoint roles; Z and surviving peer runtimes unchanged; success needs all five closure reports. | VG multi removal; F; U/S |
| HP14 | Add N with no edges, await success, then separately add an edge. | New edge accepted only after established-node condition; no second node startup. | VG node then edge; F; U/S |
| HP15 | Two independent node additions share K0. | Their preparation overlaps; both can succeed; one device does not have one global operation phase. | VG two successes/concurrent; F; U/S |
| HP16 | Two edge additions use different services on the same directed node pair/hosts. | Both bindings succeed independently without conflation or duplicate node startup. | VG two edges; F; U/S |

## Request validation and admission

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| REQ01 | Invalid/missing authority, unregistered owner, wrong report sender or unauthorized resource/port. | Reject before managed allocation/dispatch; no mutation of an existing operation. | F0/A1/M1–M2; D/G; U/S |
| REQ02 | Missing fields, wrong types, truncated messages, duplicate fields where representable, unknown version, oversized/deep values and inconsistent lengths in the selected codec. | Bounded decode failure, explicit rejection, no out-of-bounds access or unbounded allocation/recursion; instantiate CBOR-specific cases only if CBOR is selected. | Parser excluded by VN/VG; G/N; U |
| REQ03 | Unknown/duplicate channels, wrong instance/port placement, direction/type mismatch, invalid resource bounds. | Reject whole invalid request before acceptance; no partial graph side effects. | V2 §3/F0; D/G; U/S |
| REQ04 | Same opId and identical request before/after every phase, success, cleanup and unresolved cutoff. | Retained status/result; no duplicate allocation/start/removal or reset deadline. | M1/F1; D/G; U/S |
| REQ05 | Same opId but change target, artifact, P, channel placement, durations or operation kind. | Reject conflicting duplicate; original accepted request and outcome unchanged. | M1/F0; D/G; U/S |
| REQ06 | Same request encoded using every equivalent representation allowed by the selected codec, such as field order if variable. | Equality follows Q08/Q09's immutable-request policy; no accidental second allocation; a strictly canonical codec rejects disallowed representations consistently. | N; U |
| REQ07 | Two requests preflight the same free binding before either reserves it. | One admissible binding; consistent loser rejection/defer; no overlapping ownership. | VG stale binding; F; U/S |
| REQ08 | Two requests compete for one work unit or one result slot. | Atomic check-and-charge accepts at most the capacity; partial reservations roll back safely before acceptance. | VG capacity/records; F; U/S |
| REQ09 | No-edge node addition has an existing, pending or unresolved incident binding. | Reject no-edge specialization; empty original manifest is insufficient evidence of empty incidence. | M1/OP; F; U/S |
| REQ10 | No-edge removal preflights empty incidence, then an edge reservation appears. | Revalidation/fencing includes the edge or rejects this specialization; never remove using stale empty scope. | VG stale scope/empty; F; U/S |
| REQ11 | Request node removal with unknown incident scope or missing owner records. | Reject before selecting removal; request authoritative reconciliation, not best-effort scope truncation. | M1/OP; D/G; U/S |
| REQ12 | Existing application does not support dynamic binding, or endpoint belongs to an unfinished node addition. | Edge addition rejected; no invented universal application READY exchange; original work preserved. | Edge-add assumptions/M3; F/G; U/S |
| REQ13 | Invalid/corrupt/oversized WASM, unsupported import, failed integrity check, unavailable peripheral or placement. | Preflight rejection where knowable; failures discovered after admission follow cleanup with charged partial resources. | KD/M3/F2; D/G; U/S |
| REQ14 | Fail allocation after one endpoint/runtime stage or after potentially allocating remote dispatch. | Retain accepted-operation cleanup obligation; do not return allocation-free REJECTED as the whole result. | F0/M1/IN; D/G; U/S |
| REQ15 | Submit logical self-loop, migration/rebinding request, negation or arbitrary executable P. | Explicit unsupported/invalid response under current scope; no silent interpretation as a supported operation. | V2 limitations; D; U/S |

## Predicate evaluation

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| PRED01 | Enumerate all subsets for A, A or B, `(A or B) and C`, Boolean any-pair, true and the redundant expression in V2 §3. | Match explicit truth tables; redundant expression equals A or B; monotonicity holds. | BooleanContracts/PredicateChecks; F; U |
| PRED02 | Accept only A sender READY and B receiver READY, then repeat for ACTIVE. | Neither A nor B qualifies; partial evidence cannot be combined across channels. | VN walkthrough; F; U/S |
| PRED03 | Duplicate READY/ACTIVE repeatedly; same service on different node pairs. | Each complete channel counts once; no cross-channel evidence. | V2 §2–3; F/G; U/S |
| PRED04 | Failure/removal follows READY/ACTIVE, then deliver superseded readiness. | Qualifying sets exclude failed/removed channel; historical report cannot restore it. | M2/VN; F/G; U/S |
| PRED05 | P=false on all declared channels, undeclared term/group, illegal operator or excessive expression size. | Reject contract; bounded evaluation; no guest execution for P. | V2 §3; D/G; U |
| PRED06 | Before dispatch, fail A under A or B while B remains possible but incomplete; include a run that committed using A and later starts using B. | Clean A, wait for B within original deadlines; the sufficient set is not frozen at commit; fail whole operation if B also becomes terminally impossible. | M7/F3–F9/V2 Step 3; F; U/S |
| PRED07 | During Starting, lose A with B already active; repeat with B merely possible. | First case may continue because P remains true; second selects whole StartIssued cleanup immediately. | M7/F11; F; U/S |
| PRED08 | If shorthand enabled: 50% of 3, 0/100%, k=0/size, invalid k/p, empty groups and maximum arithmetic operands. | Fixed declared totals; 50% of 3 requires 2; reject/handle empty group per Q10; no overflow. If unsupported, reject syntax explicitly. | V2 shorthand not in revised model; G; U |
| PRED09 | Fail group members and duplicate group references/reports after validation. | Contract totals never shrink; duplicate handling follows validated set semantics; same P governs commit/start. | V2 §3; D/G; U/S |
| PRED10 | Maximum supported expression/channels with optional work consuming shared resources. | Evaluation bounded; admission/scheduling protects a sufficient support set under stated P3 assumptions. | P3/IN; G/N; U/S/H |

## Permissions and practical data behavior

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| PERM01 | Call Put/Send/Receive/Get on Absent, Preparing and Prepared endpoints. | Lifecycle gate rejects access; data cannot lazily create a binding. | M3/M4; D/G; U/S |
| PERM02 | Commit but pause all activation. | N inert and data permissions closed; commit alone opens nothing. | S1/M3; F/G; U/S |
| PERM03 | Activate receiver; hold its ACTIVE reply from Kc. | Receive/Get role permission may exist locally, subject to normal prerequisites; sender authorization waits for accepted evidence. | M4/F7; F/G; U/S |
| PERM04 | Activate both roles with empty/full buffers. | Empty Get/full Put return selected ordinary result; no automatic lifecycle failure unless contract specifies it. | V2 §4/KD; D/G; U/S |
| PERM05 | Try Put/Get before N executes; let a running peer use its newly activated endpoint before operation success. | N cannot call guest API while inert; established peer use is allowed and effects may predate recorded edge success. | V2 §4/M7; D/G; U/S |
| PERM06 | Submit stale/wrong-instance handle, forged source identity, undeclared import or capability from a guest. | Access denied and attributed; no authority over another or replacement binding; no uncontrolled allocation. | KD/FT/E5; G; U/S |
| PERM07 | Owner records removal while Put/Get/Send/Receive competes. | Subsequent access denied; in-flight access safely completes/quiesces before release; no silent re-creation. | M6/S4; F/G; U/S |
| PERM08 | Use identical payloads over local and remote bindings, including size/type boundary failures. | Same lifecycle, authority and ordinary buffer rules; transport placement does not bypass checks. | M4/E5; G; U/S |

## Every documented node addition fault

For F00–F15, test required versus tolerated channels where meaningful. Test failures before any allocation, during partial allocation, and after local completion but before acceptance. Prefix history is Abort before commit, CommitNoStart after commit but before dispatch, and StartIssued after dispatch, irrespective of whether execution began.

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| F00 | Invalid admission, conflicting duplicate, or validation failure after work has already begun. | REQ oracles; started work retains cleanup obligations; original request survives conflicting duplicate rejection. | V2 F0; D/G; U/S |
| F01 | Lose/delay/duplicate ADD or ACCEPTED; let Kc continue. | O remains uncertain and queries/retries same opId; one operation; no deadline reset or inferred abort. | V2 F1; D/G; U/S |
| F02 | Runtime preparation fails or stalls while endpoints allocate. | Abort whole scope; clean N and every intended endpoint, including unobserved peer allocations. | V2 F2; F/G; U/S |
| F03 | Local buffer/registry/mapping/interface preparation fails on A. | Clean both A endpoints; continue if P remains possible; otherwise whole cleanup. | V2 F3; F/G; U/S |
| F04 | Peer rejects or stalls PREPARE; lose PREPARE or READY separately. | No evidence invented; terminal failure removes channel from PossibleSet; missing reply alone does not; cleanup includes never-ready peer. | V2 F4; F/G; U/S |
| F05 | Race commit with failure/cancel/expiry; READY sender fails before its report is received. | Locally ordered decision selects correct history; unobserved failure does not retroactively invalidate genuine historical evidence; late READY cannot reopen cleanup. | V2 F5/VN stale-health witness; F; U/S |
| F06 | After commit, fail a prepared endpoint before activation. | N stays inert; alternative allowed while P possible, else CommitNoStart cleanup. | V2 F6; F; U/S |
| F07 | Receiver activation fails, ACTIVATE lost, or ACTIVE reply lost after owner activation. | Sender never authorized without accepted receiver ACTIVE; idempotent retry; cleanup accounts for possibly active receiver. | V2 F7; F/G; U/S |
| F08 | Sender activation fails or its report is lost; receiver active. | Channel not complete in ActiveSet; alternative may permit start; abandoned pair cleaned; no resurrection after local removal. | V2 F8; F/G; U/S |
| F09 | Race startup dispatch with N fault, known dependency loss, cancellation or node deadline. | Re-evaluate current gates, not commit snapshot; before dispatch failure uses CommitNoStart. Temporary insufficiency may wait only while possible/time remains. | V2 F9; F; U/S |
| F10 | Dispatch start, then revoke permission/lose prerequisites/expire before execution. | Execution suppressed; one dispatch retained; StartIssued cleanup even with execution count zero. | V2 F10/VN dispatch-suppressed witness; F/G; U/S |
| F11 | Execute N but hold confirmation; inject runtime trap/stall, loss of P or deadline. | StartIssued cleanup; effects may exist; missing confirmation cannot prove nonexecution. Tolerated loss preserving P does not force whole failure. | V2 F11; F/G; U/S |
| F12 | Deliver STARTED versus failure/deadline in both orders and at exact boundary. | Accepted success first remains terminal; failure first rejects late confirmation. Exact-time result follows Q15; no two outcomes for the same serialized decision. | V2 F12; F/N; U/S |
| F13 | Reach success, then lose/delay RESULT or make O unavailable. | Retain Succeeded/evidence; no extra acknowledgement phase, compensating cleanup or dependence on O availability detection. | V2 F13/E1; D/G; U/S |
| F14 | After N success, pending B fails or reaches its original channel deadline. | Clean B's pair only; preserve N success and ownership of missing closure; later channel status updates independently. | V2 F14/E2; F/G; U/S |
| F15 | After recorded success, N or an active channel fails. | Runtime fault/new authorized operation, not revision of historical addition outcome; removal fences still required. | V2 F15/VN historical witness; F; U/S |

## Every documented cleanup fault

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| C01 | Lose/delay CLEANUP before owner receipt, with preparation/activation still in flight. | Kc selection alone does not stop remote work; retry only within allowance; Unresolved with exact missing owners at cutoff. | V2 C1/selection witness; F; U/S |
| C02 | Owner closes but CLEANED is lost. | Duplicate cleanup returns retained evidence; Kc cannot claim clean or definite physical leak until evidence arrives. | V2 C2; F/G; U/S |
| C03 | Deliver PREPARE/ACTIVATE/STARTED or completion after owner removal; also after detailed record retirement. | Reject obsolete construction; safely reclaim late results; never reopen or issue startup again. | V2 C3/M6/reuse model; F/G; U/S |
| C04 | Hold runtime/initializer/endpoint callback reference forever during removal. | No CLEANED or unsafe free; resource stays charged; independent cleanup progresses; cutoff may report Unresolved. | V2 C4/negative reclamation; F/G; U/S |
| C05 | Race final CLEANED acceptance with removal cutoff in both orders. | Before cutoff gives final clean result; after cutoff yields Unresolved then later FailedClean/Succeeded with the same scope/history. | V2 C5/late-clean witnesses; F; U/S |
| C06 | Restore connectivity after retries stopped, once with prior command in flight and once never delivered/sent. | Late evidence may resolve first case; reconnection alone resolves neither; new retries require separately authorized reconciliation. | V2 C6/P6/VG cleanup delivery; F/G; U/S |

## Removals and host failures

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| REM01 | Remove N in each lifecycle: reserved/Preparing/Instantiated/Running/Failed/Cleaning. | Suppress delayed construction/start, share existing cleanup safely, complete only on closure; no resurrection. | OP/M6; F; U/S |
| REM02 | Remove edge while Absent-but-authorized, Preparing, Prepared, receiver-only Active, fully Active, Failed or partly Closed. | Both roles remain mandatory; never-ready role records cancellation; preserve peer runtimes. | Edge removal; F; U/S |
| REM03 | N's current incidence includes edges added later, failed bindings and optional work pending after success. | Scope includes all current records, not merely N's original manifest or active traffic. | M1/M6/VG pending witness; F; U/S |
| REM04 | Select edge removal while its standalone addition is pending. | Addition separately reaches FailedClean/Unresolved; requested removal reaches Succeeded/Unresolved; opIds and histories not conflated. | Edge removal §5; F/G; U/S |
| REM05 | Node removal overlaps its unfinished node addition. | Fence construction; cleanup full selected incidence; prior pending addition fails with original history; removal has independent result. | VG node-remove race; F/G; U/S |
| REM06 | Retry same removal and issue an independently authorized second observer while cleanup is active. | No repeated release, deadline reset or compensating recreation; each result matches the exact scope and authorization. | M1/M6/Q17; D/G; U/S |
| REM07 | Remove one of several services between the same nodes or one of several bindings of a port. | Only the named concrete binding is detached; sibling bindings and node runtime remain usable. | Identity/scoped closure; D/G; U/S |
| REM08 | Failure during removal after some CLEANED evidence accepted. | Preserve selected Objects and accepted evidence; only outstanding obligations remain; never return FailedClean for incomplete requested removal. | M6/M7; D; U/S |
| REM09 | Remove with queued payloads and already-consumed messages. | Apply selected discard policy; no guaranteed drain or undo of prior effects; physical shutdown need not be simultaneous. | OP/V2 §6; D/G; U/S/W |
| REM10 | Owner crashes after its CLEANED evidence was already accepted. | That discharged obligation remains satisfied; no renewed contact needed to use historical closure. | V2 §6.6; D; U/S |
| HOST01 | Crash Kc before/after commit, dispatch, success or cleanup selection. | All its local services stop; O records uncertainty from missing evidence; peers may retain resources; no invented takeover/closure. | A7/V2 §6.6; F/G; S |
| HOST02 | Crash Ki or permanently partition its path at each preparation/activation/cleanup boundary. | No assumed closure; historical observations persist; live Kc resolves foreground attempt under timer assumptions; missing evidence explicit. | V2 §6.6/P1–P6; F; S |
| HOST03 | Crash a device hosting several node instances/endpoints/coordinator roles. | All actual services on that device stop together; no fictitious independent surviving endpoints. | VG host crashes; F; S |
| HOST04 | O becomes unavailable during accepted addition/removal. | Authorized work continues, results retained within admitted capacity; independent device work does not wait for O. | E1/SH M7; D/G; S |
| HOST05 | Freeze/resume live Ki with buffered packets, then compare with crash-stop. | Freeze retains state; delayed traffic is validated after resume; neither silence nor later reachability proves closure. | SIM/observation model; D/G; S |
| HOST06 | Empty reboot with old commands/data still in transit. | Base crash-stop conformance makes no recovery promise; deployment history preserves boot separation; reuse/rejoin blocked unless a specified extension establishes isolation. | Excluded recovery/Q02; G; S |
| HOST07 | Crash a required owner before receipt versus after closure but before report. | Both may be unresolved to Kc, despite different physical states; no false root-cause inference from identical observations. | A9/C1/C2; D/G; U/S |

## Concurrent operations and scheduling

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| CON01 | Overlap independent node/runtime and endpoint preparation on the same device. | Work interleaves/progresses independently within resources; no full-device operation lock. | VN/VG concurrency witnesses; F; U/S |
| CON02 | Edge admission races with node-removal scope fixation, before and after edge reservation. | Binding either belongs to fixed removal scope or is excluded by fence; no accepted orphan. | VG stale scope/missing fence; F; U/S |
| CON03 | Edge admission targets N while its no-edge addition is pending or unresolved. | Cannot attach before N success; cleanup uncertainty does not make the instance available. | VG premature edge/admission pending; F; U/S |
| CON04 | Edge removal proceeds while unrelated node addition waits for a dead peer. | Removal and unrelated timers complete independently; preserved scope and bounded waits. | VG edge remove independent/scheduler; F; U/S |
| CON05 | One operation holds a remote wait; queue a second operation, local task and both deadline handlers. | No shared lock/worker exhaustion can disable their progress; both foreground attempts resolve if clock/handlers progress. | VG negative device lock; F/G; U/S/H |
| CON06 | Saturate heartbeat/data tasks while one cleanup timer is enabled. | Relevant timer/action receives service; a live device heartbeat alone is not fairness evidence. | VG negative device fairness; F/G; U/S/H |
| CON07 | Freeze logical time while fairly scheduling handlers, then resume time. | No false elapsed-time progress claim while frozen; deadlines become actionable when time advances. | VG negative no clock; F; U |
| CON08 | Fair local scheduling but permanently withhold necessary message delivery. | Safety holds; do not promise successful setup/complete cleanup; live Kc timers can report unresolved. | VN/VG negative delivery; F; U/S |
| CON09 | Optional channel expires after parent success while a node-removal request fixes scope. | Shared cleanup retains correct binding ownership/deadlines; parent success unchanged; no optional obligation dropped. | Combined-model gap; G; U/S |
| CON10 | Three or more concurrent operations share capacity, ports and mixed placements. | Exclusive reservations, complete scopes and per-action progress hold up to admitted limits. | Beyond VG's two-operation bound; G; U/S |
| CON11 | Two different Kc devices request conflicting edges/removals through O concurrently. | Distributed reservation algorithm enforces authority/fences across hosts without deadlock; no assumption of one K0. | VG limitation/Q04; G; U/S |
| CON12 | O has authorized/reserved an incident edge but its command has not reached Kc/Ki when node removal is requested. | Authorized edge included or safely revoked by admission protocol before removal accepts; local snapshot alone insufficient. | M1 realization; N; U/S |
| CON13 | Cancel after only some initial PREPARE requests have been sent. | Every intended object remains in cleanup scope; no false liveness premise that all cleanup commands were sent before cutoff. | VG initial fan-out abstraction; G/N; U/S |
| CON14 | Rich Boolean P across shared hosts; race loss of an alternative with commit/start and later graph operation. | Current P and phase determine consequences; complete-channel matching and full removal scope preserved. | Separate VN/VG composition gap; G; U/S |
| CON15 | Shared cleanup satisfies a failed addition and a requested removal while one result consumer loses its report. | Accepted evidence at one consumer does not update another invisibly; independent correlated results can differ in knowledge. | V2 §6.4/Q17; N; U/S |

## Identity and evidence ordering

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| ID01 | Same service across disjoint node pairs; several services between one pair; opposite directions. | Full directed tuple and roles isolate readiness, data and cleanup. | V2 §2; D/G; U/S |
| ID02 | Wrong opId/instance/port/creating authorization/owner or report for work never issued. | Reject evidence with no qualifying-set or outcome change. | M2; D/G; U/S |
| ID03 | READY then failure then delayed duplicate READY/ACTIVE with older revision. | No readiness revival; stream ordering, not receive order alone, determines admissibility. | M2/IN; F/G; U/S |
| ID04 | Delayed genuine report from an owner that has since failed, with no newer failure observation yet. | Accept only if otherwise matching/current under M2; do not imply current health; preserve historic evidence. | VN stale-health witness; F; U/S |
| ID05 | Close both endpoint roles, retire detailed result, then deliver old PREPARE. | Obsolete construction still rejected; no allocated resurrection. | VG negative forget rejection; F/G; U/S |
| ID06 | Reuse channel after complete closure/isolation with fresh creation identity; inject old ACTIVATE/CLEANUP/callback. | New binding unaffected; old commands cannot target it through channelId or reused pointer/slot. | VG channel-only command; F/G; U/S |
| ID07 | Inject old buffered/in-flight data into a newly active reused channel, over local queues and remote transport. | Old payload not delivered to replacement binding; control opId checks alone insufficient. | VG channel-only data; F/G; U/S |
| ID08 | Attempt reuse after timeout with one CLEANED missing. | Reuse rejected/deferred even if observer believes peer is dead. | VG reuse on timeout; F; U/S |
| ID09 | Reclaim/reuse an internal operation record, then deliver its old timer/completion handle. | New operation unchanged; lifetime/identity validation prevents aliasing regardless of the replacement's record representation. | Record reuse risk; N; U/S |
| ID10 | Deliver valid earlier operation after a larger ID; test retirement with holes in outstanding IDs. | Earlier authorized work remains admissible; highest-seen ID alone cannot retire it. | IN retired-prefix warning; D/G; U/S |
| ID11 | Approach identity/revision counter exhaustion, rollover, session reset or restart. | Selected Q03/Q05/Q07 policy either safely isolates old traffic or stops admission; never silently alias records. | Concrete rollover unspecified; N; U/S |
| ID12 | Reorder/duplicate results at O and status updates after success. | Historical outcome never regresses; newer channel evidence does not rewrite startup; revisions compared only within intended streams. | E1/M2/M7; D/G; U/S |

## Reporting and deadlines

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| MGMT01 | Query after lost ACCEPTED or RESULT using original opId. | Return retained outcome/evidence without allocating, restarting, resetting deadlines or requiring Kc to detect O availability. | SH M7; D/G; U/S |
| MGMT02 | Query Pending in Preparing, Committed, Starting and failure cleanup. | Correct common label plus phase/history, matching evidence, channel statuses and exact missing obligations. | SH M7/V2 §7; D/G; U/S |
| MGMT03 | Keep O unavailable until all admitted result slots are occupied. | Already admitted operations resolve; further admission rejected/deferred; no premature retirement of promised evidence. | SH bounded evidence/VG record bound; F/G; U/S |
| MGMT04 | Query a result after selected, authorized detailed-record retirement. | Defined retired-result response under Q06; no fabricated historical result from current state; obsolete commands still rejected. | Q06/E1/M6; G; U/S |
| MGMT05 | Retry each command, commit, start, and query near node/channel deadlines; submit a channel allowance shorter than its parent node allowance. | Original monotonic deadlines unchanged; invalid allowance relationship rejected at validation; a standalone edge uses its channel deadline without a node deadline. | M7; D/G; U/S |
| MGMT06 | At node deadline−tick, deadline, deadline+tick accept STARTED; repeat for final channel ACTIVE. | Before deadline can succeed; boundary follows Q15; after deadline cannot succeed even if timer callback is delayed. | M7/P5; N; U/S |
| MGMT07 | Delay timer handler under load and step clock through numeric wrap/boundary. | No false success after expiry; bounded handler delay measured; arithmetic safe under selected clock representation. | P5/Q15; G/N; U/H |
| MGMT08 | Optional channel still pending after node success, then completes or cleans after its original deadline. | Node result remains Succeeded; channel uses its own original allowance/cleanup evidence; no result-slot leak from forgotten owner. | E2/M7; F/G; U/S |
| MGMT09 | Select whole cleanup while a channel cleanup is active or its retry cutoff exhausted. | Reuse evidence/work without extending the exhausted retry window; preserve original missing obligations. | V2 §4.3/M6; D/G; U/S |
| MGMT10 | Separately authorize reconciliation after Unresolved; lose/repeat authorization. | New bounded attempt only under defined authorization; preserve history and original result lineage; no spontaneous retries on link restoration. | P6/Q17; D/G; U/S |

## Lifetime and embedded resource tests

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| LIFE01 | Fail the nth allocation in every runtime/endpoint/artifact preparation path. | Every partial allocation has an owner; no false READY; complete closure only after reclaim/cancel; ledger returns to expected retained baseline. | IN/E6; G; U/S |
| LIFE02 | Endpoint initialization holds N's registry while node cleanup runs. | Registry not freed until access safe; no premature CLEANED(N). | VG early-registry counterexample; F/G; U/S |
| LIFE03 | Delay timer, network-send, receive, guest-call and local-event callbacks across removal. | No use-after-free/recreation; callbacks carry matching lifetime/creation reference and release it once. | M6/IN; G; U/S |
| LIFE04 | Cleanup a never-created object, then release its delayed allocating task. | Terminal cancellation precedes CLEANED; task cannot recreate it and disposes any late private allocation. | VN never-ready witness; F/G; U/S |
| LIFE05 | Multiple co-located endpoint roles share buffer/registry storage; remove in every order. | No double free, leak or premature release; remaining authorized users remain safe. | E6/M6; G; U/S |
| LIFE06 | Guest initialization traps/stalls/attempts data access or emits liveness before authorized execution; endpoint preparation runs concurrently. | Q12's inert-preparation contract enforced; mappings preserved; no false STARTED or indefinitely held decision lock. If preparation runs no guest code, verify this exclusion and inject equivalent platform preparation faults. | Q12/Q23; N; U/S |
| LIFE07 | Missing declared execution entrypoint, failed runtime execution-context creation, trap at entry, immediate normal return. | STARTED reflects actual intended entry through the selected engine hook; preparation/enqueue/state flags cannot substitute; subsequent exit is separately reported. | M5/Q12/Q23; G/N; U/S |
| LIFE08 | Duplicate dispatch/STARTED, plus failure between execution and emission/acceptance. | One execution maximum, correct StartIssued history, no late restoration of failure. | S2/VN; F/G; U/S |
| LIFE09 | Release an endpoint buffer with outstanding Get/Put access or cancel a timer from its callback; include blocked users if the selected API supports them. | New port cancellation/lifetime contract makes release safe; unsupported cases cannot be reported as CLEANED; tests do not require the old queue/timer implementation. | Lifetime/port contract; G; U/S/H |
| LIFE10 | Repeated add/remove/fail/reconcile/reuse cycles, including sanitizer and bounded-soak runs. | No increasing live allocation/reference count beyond intentional bounded retained records; no sanitizer findings; independently account unresolved resources. | E6/IN; G; S/H |
| RES01 | Each configured bound at limit−1/limit/limit+1: instances, endpoint roles, channels, P size, events, results, payloads, timers and artifacts. | Correct acceptance/rejection with no integer overflow or partial untracked side effects; host/target profile parity. | A8/Q01; G; U/S/H |
| RES02 | Steady-state resources fit, but overlapping runtime preparation/cleanup/staging exceeds peak budget. | Admission considers peak overlap; no accepted guarantee based only on final footprint. | E6; G/N; U/S/H |
| RES03 | Exhaust the selected data/event capacity while cleanup and deadlines require completion events. | Reserved control capacity or recoverable notification maintains correctness; dropped required evidence never becomes success; test the new profile rather than inheriting queue size 10. | P2/control capacity; N; U/S/H |
| RES04 | Many optional channels compete with one sufficient required subset. | Under admitted P3 assumptions, optional work cannot indefinitely exhaust the support set's resources; otherwise admission rejects. | P3; D/G; U/S/H |
| RES05 | Finish work but retain all full result slots; separately hold unresolved buffers. | Transient work capacity, retained-result capacity and unresolved resource charges remain independent. | VG negative ignore records; F/G; U/S |
| RES06 | Exhaust compact rejection-record capacity after many closed operations. | Safe admission backpressure or proven compaction; no unsafe eviction that permits delayed creation. | IN/M6; G; U/S |
| RES07 | Maximum fan-in/out, full queue payloads and result/channel-status serialization together. | Bounded peak memory and CPU; no unbudgeted report buffer; response paging/truncation, if chosen, preserves explicit completeness. | Q01/Q06/V2 notes; G/N; U/S/H |
| RES08 | Invalid lengths and arithmetic extremes in payload/frame/array/percentage sizes. | Validate before multiplication/addition/copy/allocation; safely reject truncated frame or oversized declared payload. | ENV04/Q08; N; U |
| RES09 | Malicious/faulty guest floods reports, requests, Put, timers or imports. | Enforced per-instance bounds preserve management/cleanup service; guest reports cannot authorize remote changes. | FT trust boundary; D/G; U/S/H |
| RES10 | Measure stack high-water, allocator fragmentation, RAM/flash footprint, CPU and handler latency on selected board. | Meet Q01/Q14/Q15 budgets with recorded headroom; no host RSS or latency substituted for target measurements. | FT evaluation/P5; G; H |
| RES11 | If persistence/reboot extension selected: cut power during each identity/result journal update and compaction. | Explicit recovery specification holds, including torn writes/old traffic and flash budget; otherwise mark out of scope. | Q02 extension; G; H |
| RES12 | No clock progress, no scheduling, no resources, no delivery and owner loss in separate runs. | Identify exactly which liveness premise is removed; preserve safety, avoid unconditional startup/closure claims, retain useful negative traces. | VN/VG assumption removals; F/G; U/S |

## Observer and simulator support

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| OBS01 | Inject loss/duplication/delay/reordering separately into O↔Kc commands/results, Kc↔Ki exchanges, data and local callbacks. | Each class actually impaired; reliable direct TCP path cannot silently bypass requested fault. | ENV06; G; S |
| OBS02 | Schedule injection at a named preparation/start/cleanup boundary. | Barrier reached and fault recorded before release; tests do not rely only on warmup-relative wall-clock offsets. | ENV12; G; U/S |
| OBS03 | Lose telemetry, truncate snapshot, overflow relay or delay injector beyond tolerance. | Mark affected oracle/run compromised; no event absence treated as proof of nonexecution or closure. | SIM evidence; D; S |
| OBS04 | Stale/empty/wrong address mappings; local channels versus remote transport. | No semantic rerouting by relay; failure/uncertainty visible; local path obeys permissions/isolation. | SIM mapping policy; D; S |
| OBS05 | Replay same event log; rerun same seed with different thread scheduling. | Replay reproduces observation results; report that full execution is not deterministic; retain actual schedule/trace. | SIM replay limits; D; S |
| OBS06 | Observe requested graph, actual owner state, accepted evidence and O's view at different times. | Four views remain distinct; no synthetic simultaneous snapshot or hidden evidence propagation. | A9/E1/M2; D/G; U/S |
| OBS07 | End measurement/run with pending operations or missing closure; then stop processes for harness teardown. | Pending/unresolved operation evidence preserved; teardown/exit does not convert it into protocol CLEANED or success. | SIM teardown/M6; N; S |
| OBS08 | Run directed independent/burst loss, asymmetrical partition, jitter reordering, freeze and host crash on shared services. | Correct physical-pair impairment and common-host failure; report distributions over independent runs, not correlated packets. | SIM/FT; D; S |
| OBS09 | Apply skew/drift to per-device data clocks while keeping local monotonic operation timers independent. | Deadline ownership preserved; freshness judged using Q19 provenance/uncertainty, not shared host time. | KD clock gap; G; U/S/W |
| OBS10 | Deliberately invalidate one guard in a test-only reference and compare with conforming core. | Regression detects the known bad trace; positive witnesses also reach success so invariants are not vacuous. | VN/VG methodology; F/G; U/S |

## Application examples and service evaluation

Application cases retain the protocol oracles above and add workload observations. Until Q18–Q21 are answered, service thresholds are explicit parameters and the affected acceptance criterion is **blocked**, not guessed or marked passed. Values used for mechanical test fixtures must be labeled synthetic. A received transport acknowledgement never proves that an actuator executed a command.

| ID | Setup and stimulus | Expected observations | Basis and layer |
|---|---|---|---|
| APP01 | Compile sensor→transform→sink fixtures for the new ABI; deploy on three devices, then co-located; feed known sequence/value/padding. | `2*value+1` and padding correct; scheduled misses counted; configured synthetic deadline/freshness evaluated separately from protocol success. Existing fixture binary compatibility is unnecessary. | SIM pipeline; D; S/W |
| APP02 | Reimplement or recompile the two-node feedback example for the new ABI; run local and remote under loss and delayed preparation. | Correlated responses and intended loop behavior through new explicit operations; each operation obeys its graph gates. Old `echo.c` imports and LOAD framing are not requirements. | SIM operations example; D; S/W |
| APP03 | V2 two outgoing channels A/B to peer(s), P=A or B; complete only A, then reverse. | N starts with either support; B may join or clean later without changing success; app tolerates unavailable binding. | V2 sequence and walkthrough; F; S/W |
| APP04 | Mixed input alternatives and actuator output: `(inputA or inputB) and actuatorOutput`. | Startup waits for complete active output plus one input; later application service assessment remains separate. | V2 §3/VN mixed; F; S/W |
| APP05 | Ventilation sensing→validation/aggregation→policy→device-local controller/actuator; healthy deterministic sensor traces. | Approved criteria define nominal service; hardware dependencies enforced; local control remains correctly placed; useful output not inferred from STARTED alone. | FT selected application; D/G; S/W/H |
| APP06 | Inject malformed measurement, implausible CO2/occupancy, drift and wrong-but-plausible data independently. | Application rejects/reports only according to declared checks; transport can remain Active/healthy; undetectable semantic faults reported as coverage limits. | FT execution/hardware scope; D/G; W |
| APP07 | Remove/lose optional sensor then required sensing set under running ventilation policy. | Classify nominal/degraded/unavailable from Q20; authorized alternatives do not recreate edges; no automatic equation of channel failure and total service failure. | FT/V2 §8; D/G; W |
| APP08 | Partition supervisory setpoint path; retain local feedback and actuator. | Selected fallback or hold policy and maximum duration enforced; record degradation and escalation; no assumed safe default. | FT workload questions; D/G; W/H |
| APP09 | Deliver stale/duplicate/reordered/versioned setpoints and let validity expire. | Apply chosen version/expiry/outcome semantics; old command cannot control replacement binding; missing application response remains unknown. | KD command profile proposal; D/G; W |
| APP10 | Command accepted by endpoint, then application rejects/traps or actuator fails; separately lose return-edge outcome. | Distinguish Receive/Get/acceptance/execution; report application/physical outcome or uncertainty; do not infer exactly-once effects. | KD completion/FT actuator faults; D/G; W/H |
| APP11 | Stop sensor production or consumer draining while transport remains available. | Producer/consumer obligations and buffer/expiry policy expose the observed violation; no unjustified diagnosis of network failure. | KD contracts/FT; D/G; W |
| APP12 | Add/remove a validation or policy node with incoming/outgoing channels while other control work runs. | Partial activation and service interruption are measured; no atomic graph-switch/migration claim; peer applications tolerate defined binding changes. | OTHER insertion example constrained by V2; D/G; S/W |
| APP13 | Crash hardware sensor/actuator device with spare compute elsewhere. | Placement/physical-capacity constraints prevent pretending spare CPU restores sensing/actuation; defined unavailable/escalation behavior. | FT hardware distinction; D/G; W/H |
| APP14 | Workload bug deterministically produces wrong output; reload same binary in a separate extension experiment. | No claim that redeployment repairs deterministic bug; application checks/observer identify the actual supported detection boundary. | FT fault scope; D/G; W |
| APP15 | Future migration/replacement or persistent recovery with stateful filter/controller. | Mark outside current six-operation conformance; require state-transfer/reset, service-disruption and old-instance exclusion specification before pass criteria. | V2 exclusions/FT migration caveat; G; W/H |

## Formal configuration traceability

The [CSV inventory](graph_operation_verification_inventory.csv) contains all **90 configurations** from the two suites, including their module, specification, exact expected TLC result, invariants/properties, required trace actions where declared, retained state counts, audit status and target test IDs. No configuration is silently omitted, including the deliberate negative experiments.

The graph suite has 29 positive configurations, 16 negatives and 10 witnesses; the node suite has 19 positives, 7 negatives and 9 witnesses. Positive finite safety is preserved without fairness; conditional progress requires the particular live hosts, scheduling, delivery, resources and time assumptions. The main graph model's coordinators share K0, its examples have one or two operations and at most three channels, and its focused admission/scheduler/reuse models are not a machine-checked composition with C or with the richer node model.

For each witness, turn the retained trace into a deterministic integration schedule and assert the desired behavior while checking conforming invariants. For guard-removal counterexamples, make the corresponding bad state unreachable in core. For liveness-assumption removals, assert the supported safety and uncertainty outcome, not an impossible unconditional completion guarantee. Model mutations stay in verification fixtures and must not ship as disabled production checks.

## Execution and acceptance record

Each instantiated test records ID/variant, setup/profile, applicable requirement, exact boundary/fault, expected result/evidence, trace, actual injected fault, actual result, residual resource obligations, timing/memory measurements, hashes and status: pass, fail, blocked by design, unsupported harness, or inconclusive instrumentation. Do not combine these statuses into one pass count.

Run deterministic codec/core/predicate/lifetime tests on each relevant change; then affected real-process tests with controlled schedules. Run all six operations with no faults before extending to loss/race matrices. Use allocation/sanitizer/churn suites for ownership changes; use selected target-board tests for memory/timing/port claims. Rerun affected TLC configurations when changing a protocol guard or a modeled service algorithm, preserving exact positive/negative/witness expectations. A full sweep is a release gate after those focused checks, not a replacement for them.

Release acceptance requires all mandatory protocol cases and embedded bounds to pass in the selected scope. Optional shorthand/persistence/migration tests receive explicit Q10/Q02/Q22 dispositions. Ventilation service acceptance remains separate and needs approved Q20 criteria plus justified Q21 model fidelity. A complete simulator observation with zero useful samples is an experiment result, not successful application service.
