---------------------------- MODULE NodeRuntime ----------------------------
EXTENDS Naturals, FiniteSets

CONSTANTS Nodes, MaxAccesses, OrdinaryCapacity
VARIABLES node, ordinary
vars == <<node, ordinary>>

Empty == [admitted |-> FALSE, retained |-> FALSE, phase |-> "idle",
          ready |-> FALSE, start |-> FALSE, closing |-> FALSE,
          initSent |-> FALSE, initPending |-> FALSE, success |-> FALSE,
          additionDue |-> FALSE, cleanupDue |-> FALSE, cutoff |-> FALSE,
          remove |-> FALSE, dispose |-> FALSE, held |-> FALSE,
          charges |-> 0, accesses |-> 0, cleaned |-> FALSE]
Init == /\ node = [n \in Nodes |-> Empty] /\ ordinary = 0

(* Each n denotes one immutable instance/creator identity in a fixed slot.
 * Retained history and mailbox capacity are reserved at admission. No reuse
 * or history retirement is modeled; stale identities cannot name a new n. *)
Admit(n) == /\ ~node[n].admitted
            /\ node' = [node EXCEPT ![n].admitted = TRUE, ![n].retained = TRUE]
            /\ UNCHANGED ordinary

(* Abstract validated artifact handoff, two charges (artifact and arena),
 * and one action token. The token lasts until disposed is accepted, not
 * merely until preparation completes. Worker allocation is outside control. *)
Dispatch(n) == /\ node[n].admitted /\ ~node[n].closing
               /\ ~node[n].additionDue /\ node[n].phase = "idle"
               /\ node' = [node EXCEPT ![n].phase = "queued",
                                        ![n].held = TRUE, ![n].charges = 2]
               /\ UNCHANGED ordinary
BeginPrepare(n) == /\ node[n].phase = "queued" /\ ~node[n].closing
                   /\ node' = [node EXCEPT ![n].phase = "preparing"]
                   /\ UNCHANGED ordinary
Prepared(n) == /\ node[n].phase = "preparing"
               /\ node' = [node EXCEPT ![n].phase = "prepared"]
               /\ UNCHANGED ordinary

(* No-edge predicate is true. READY, commit and dispatch are one bounded
 * control decision; the distinct worker BeginRun still races cancellation. *)
AuthorizeStart(n) == /\ node[n].phase = "prepared" /\ ~node[n].closing
                     /\ ~node[n].additionDue /\ ~node[n].start
                     /\ node' = [node EXCEPT ![n].ready = TRUE, ![n].start = TRUE]
                     /\ UNCHANGED ordinary
BeginRun(n) == /\ node[n].phase = "prepared" /\ node[n].start
               /\ ~node[n].closing /\ ~node[n].additionDue
               /\ node' = [node EXCEPT ![n].phase = "configuring"]
               /\ UNCHANGED ordinary
EnterMain(n) == /\ node[n].phase = "configuring" /\ ~node[n].closing
                /\ node' = [node EXCEPT ![n].phase = "main"]
                /\ UNCHANGED ordinary
EmitInit(n) == /\ node[n].phase = "main" /\ ~node[n].closing
               /\ ~node[n].initSent
               /\ node' = [node EXCEPT ![n].initSent = TRUE, ![n].initPending = TRUE]
               /\ UNCHANGED ordinary

(* Exit and acceptance inspect/update the same locked mailbox. A marker
 * emitted earlier cannot succeed after Exit makes main inactive. *)
AcceptInit(n) == /\ node[n].initPending /\ ~node[n].success
                 /\ node[n].phase = "main" /\ node[n].start /\ node[n].ready
                 /\ ~node[n].closing /\ ~node[n].additionDue
                 /\ node' = [node EXCEPT ![n].initPending = FALSE,
                                          ![n].success = TRUE]
                 /\ UNCHANGED ordinary
Exit(n) == /\ node[n].phase \in {"preparing", "configuring", "main"}
           /\ node' = [node EXCEPT ![n].phase = "exited"]
           /\ UNCHANGED ordinary

(* A fresh O-authorized exact-instance removal may arrive in any phase.
 * It is distinct from runtime exit; historical addition success stays true. *)
Remove(n) == /\ node[n].admitted /\ ~node[n].remove
             /\ node' = [node EXCEPT ![n].remove = TRUE, ![n].closing = TRUE,
                                      ![n].initPending = FALSE]
             /\ UNCHANGED ordinary

(* due is the clock comparison, independent of timer-event service.
 * This abstraction records before/at-or-after each immutable deadline. *)
AdditionTime(n) == /\ node[n].admitted /\ ~node[n].additionDue
                   /\ ~node[n].success /\ ~node[n].closing
                   /\ node' = [node EXCEPT ![n].additionDue = TRUE]
                   /\ UNCHANGED ordinary
Close(n) == /\ node[n].admitted /\ ~node[n].closing
            /\ (node[n].remove \/ node[n].phase = "exited"
                \/ (node[n].additionDue /\ ~node[n].success))
            /\ node' = [node EXCEPT ![n].closing = TRUE, ![n].initPending = FALSE]
            /\ UNCHANGED ordinary
CleanupTime(n) == /\ node[n].closing /\ ~node[n].cleanupDue /\ ~node[n].cleaned
                  /\ (~node[n].success \/ node[n].remove)
                  /\ node' = [node EXCEPT ![n].cleanupDue = TRUE]
                  /\ UNCHANGED ordinary
Cutoff(n) == /\ node[n].cleanupDue /\ ~node[n].cutoff /\ ~node[n].cleaned
             /\ node' = [node EXCEPT ![n].cutoff = TRUE]
             /\ UNCHANGED ordinary

(* Short access admission/closure checks share serialization. Returned
 * accesses retain this exact instance; they cannot transfer to another. *)
Access(n) == /\ node[n].phase = "main" /\ ~node[n].closing
             /\ node[n].accesses < MaxAccesses
             /\ node' = [node EXCEPT ![n].accesses = @ + 1]
             /\ UNCHANGED ordinary
Drain(n) == /\ node[n].accesses > 0
            /\ node' = [node EXCEPT ![n].accesses = @ - 1]
            /\ UNCHANGED ordinary
Stop(n) == /\ node[n].closing
           /\ node[n].phase \in {"queued", "preparing", "prepared", "configuring", "main"}
           /\ node' = [node EXCEPT ![n].phase = "exited"]
           /\ UNCHANGED ordinary
RequestDispose(n) == /\ node[n].closing /\ node[n].held /\ ~node[n].dispose
                     /\ node[n].accesses = 0
                     /\ node' = [node EXCEPT ![n].dispose = TRUE]
                     /\ UNCHANGED ordinary
BeginDispose(n) == /\ node[n].dispose /\ node[n].phase = "exited"
                   /\ node[n].accesses = 0
                   /\ node' = [node EXCEPT ![n].phase = "disposing"]
                   /\ UNCHANGED ordinary
Disposed(n) == /\ node[n].phase = "disposing"
               /\ node' = [node EXCEPT ![n].phase = "disposed"]
               /\ UNCHANGED ordinary
AcceptDisposed(n) == /\ node[n].phase = "disposed" /\ node[n].held
                     /\ node' = [node EXCEPT ![n].held = FALSE]
                     /\ UNCHANGED ordinary
Uncharge(n) == /\ node[n].phase = "disposed" /\ ~node[n].held
               /\ node[n].accesses = 0 /\ node[n].charges > 0
               /\ node' = [node EXCEPT ![n].charges = 0]
               /\ UNCHANGED ordinary
Release(n) == /\ node[n].closing /\ node[n].retained
              /\ node[n].phase \in {"idle", "disposed"}
              /\ ~node[n].held /\ node[n].charges = 0 /\ node[n].accesses = 0
              (* Timely/late evidence ordering at the control boundary. *)
              /\ (~node[n].cleanupDue \/ node[n].cutoff)
              /\ node' = [node EXCEPT ![n].retained = FALSE, ![n].cleaned = TRUE]
              /\ UNCHANGED ordinary

(* Ordinary traffic uses separate bounded storage. It can never fill the
 * per-node INIT/completion mailboxes or disable control/cleanup decisions. *)
OrdinaryArrival == /\ ordinary < OrdinaryCapacity /\ ordinary' = ordinary + 1
                   /\ UNCHANGED node
OrdinaryService == /\ ordinary > 0 /\ ordinary' = ordinary - 1 /\ UNCHANGED node

Next == OrdinaryArrival \/ OrdinaryService \/
        \E n \in Nodes : Admit(n) \/ Dispatch(n) \/ BeginPrepare(n) \/ Prepared(n)
          \/ AuthorizeStart(n) \/ BeginRun(n) \/ EnterMain(n) \/ EmitInit(n)
          \/ AcceptInit(n) \/ Exit(n) \/ Remove(n) \/ AdditionTime(n) \/ Close(n)
          \/ CleanupTime(n) \/ Cutoff(n) \/ Access(n) \/ Drain(n) \/ Stop(n)
          \/ RequestDispose(n) \/ BeginDispose(n) \/ Disposed(n)
          \/ AcceptDisposed(n) \/ Uncharge(n) \/ Release(n)

TypeOK == /\ ordinary \in 0..OrdinaryCapacity
          /\ node \in [Nodes -> [admitted : BOOLEAN, retained : BOOLEAN,
               phase : {"idle", "queued", "preparing", "prepared", "configuring",
                        "main", "exited", "disposing", "disposed"},
               ready : BOOLEAN, start : BOOLEAN, closing : BOOLEAN,
               initSent : BOOLEAN, initPending : BOOLEAN, success : BOOLEAN,
               additionDue : BOOLEAN, cleanupDue : BOOLEAN, cutoff : BOOLEAN,
               remove : BOOLEAN, dispose : BOOLEAN, held : BOOLEAN,
               charges : 0..2, accesses : 0..MaxAccesses, cleaned : BOOLEAN]]
BoundedReservations == Cardinality({n \in Nodes : node[n].retained}) <= Cardinality(Nodes)
RetainedLifetime == \A n \in Nodes :
    (node[n].held \/ node[n].charges > 0 \/ node[n].accesses > 0) => node[n].retained
WorkerPinned == \A n \in Nodes :
    node[n].phase \in {"queued", "preparing", "prepared", "configuring", "main",
                       "exited", "disposing"} => node[n].held
SafeReclamation == \A n \in Nodes : node[n].cleaned =>
    (node[n].closing /\ node[n].phase \in {"idle", "disposed"}
     /\ ~node[n].held /\ node[n].charges = 0 /\ node[n].accesses = 0)
SafeDisposal == \A n \in Nodes : node[n].dispose =>
    (node[n].closing /\ node[n].accesses = 0)
AuthorizedExecution == \A n \in Nodes :
    node[n].phase \in {"configuring", "main"} => (node[n].ready /\ node[n].start)
ReservedEvidence == \A n \in Nodes : node[n].initPending =>
    (node[n].admitted /\ node[n].initSent /\ node[n].held)

AcceptedWhileUsable == [][\A n \in Nodes :
    (~node[n].success /\ node'[n].success) =>
    (node[n].phase = "main" /\ node[n].start /\ node[n].ready
     /\ ~node[n].closing /\ ~node[n].additionDue /\ node[n].initPending)]_vars
HistoricalSuccess == [][\A n \in Nodes : node[n].success => node'[n].success]_vars
NoEntryAfterClose == [][\A n \in Nodes :
    (node[n].closing /\ node[n].phase # "main") => node'[n].phase # "main"]_vars
ChargesUntilDisposed == [][\A n \in Nodes : node'[n].charges < node[n].charges =>
    (node[n].phase = "disposed" /\ ~node[n].held /\ node[n].accesses = 0)]_vars
CutoffBeforeLateClosure == \A n \in Nodes :
    (node[n].cleaned /\ node[n].cleanupDue) => node[n].cutoff

(* Conditional liveness: live host, fair timer/control service and each
 * worker's eventual completion/termination/destruction; tracked accesses
 * eventually return. None of these assumptions establishes a latency bound.
 * Success, guest cooperation, removal authorization and delivery are NOT
 * fairness assumptions. A main loop without INIT times out and is stopped. *)
Spec == Init /\ [][Next]_vars /\ WF_vars(OrdinaryService)
        /\ \A n \in Nodes :
             WF_vars(Dispatch(n)) /\ WF_vars(BeginPrepare(n)) /\ WF_vars(Prepared(n))
          /\ WF_vars(AuthorizeStart(n)) /\ WF_vars(BeginRun(n)) /\ WF_vars(EnterMain(n))
          /\ WF_vars(AcceptInit(n)) /\ WF_vars(AdditionTime(n)) /\ WF_vars(Close(n))
          /\ WF_vars(CleanupTime(n)) /\ WF_vars(Cutoff(n)) /\ WF_vars(Drain(n))
          /\ WF_vars(Stop(n)) /\ WF_vars(RequestDispose(n)) /\ WF_vars(BeginDispose(n))
          /\ WF_vars(Disposed(n)) /\ WF_vars(AcceptDisposed(n))
          /\ WF_vars(Uncharge(n)) /\ WF_vars(Release(n))
CleanupProgress == \A n \in Nodes : node[n].closing ~> node[n].cleaned
OutcomeProgress == \A n \in Nodes : node[n].admitted ~> (node[n].success \/ node[n].cleaned)
=============================================================================
