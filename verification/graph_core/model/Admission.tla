----------------------------- MODULE Admission -----------------------------
EXTENDS Naturals, FiniteSets

CONSTANTS Creators, NodeLimit, RecordLimit
VARIABLES authorized, sent, possible, retained, nodes, closed, work, staged,
          cleaned, observed, attempts
vars == <<authorized, sent, possible, retained, nodes, closed, work, staged,
          cleaned, observed, attempts>>

Init == /\ authorized = {} /\ sent = {} /\ possible = {}
        /\ retained = {} /\ nodes = {} /\ closed = {} /\ work = {}
        /\ staged = {} /\ cleaned = {} /\ observed = {}
        /\ attempts = [i \in Creators |-> 0]

(* O records immutable authorizations before making them available to send.
 * Consumed IDs never return to the allocator. *)
Authorize(i) == /\ i \notin authorized
                /\ authorized' = authorized \cup {i}
                /\ possible' = possible \cup {i}
                /\ UNCHANGED <<sent, retained, nodes, closed, work, staged,
                               cleaned, observed, attempts>>
Send(i) == /\ i \in possible \ sent
           /\ sent' = sent \cup {i}
           /\ UNCHANGED <<authorized, possible, retained, nodes, closed, work,
                          staged, cleaned, observed, attempts>>
Withdraw(i) == /\ i \in possible \ sent
               /\ possible' = possible \ {i}
               /\ UNCHANGED <<authorized, sent, retained, nodes, closed, work,
                              staged, cleaned, observed, attempts>>

(* One atomic check/reserve/accept decision; failed checks leave K unchanged.
 * Duplicate datagrams cannot execute this transition while ID is retained. *)
Admit(i) == /\ i \in sent /\ i \notin retained /\ attempts[i] = 0
            /\ Cardinality(nodes) < NodeLimit
            /\ Cardinality(retained) < RecordLimit
            /\ retained' = retained \cup {i} /\ nodes' = nodes \cup {i}
            /\ attempts' = [attempts EXCEPT ![i] = @ + 1]
            /\ UNCHANGED <<authorized, sent, possible, closed, work, staged,
                           cleaned, observed>>

Stage(i) == /\ i \in nodes \ closed /\ i \notin staged
            /\ staged' = staged \cup {i} /\ work' = work \cup {i}
            /\ UNCHANGED <<authorized, sent, possible, retained, nodes, closed,
                           cleaned, observed, attempts>>
Finish(i) == /\ i \in work /\ work' = work \ {i}
             /\ UNCHANGED <<authorized, sent, possible, retained, nodes, closed,
                            staged, cleaned, observed, attempts>>
Cancel(i) == /\ i \in nodes \ closed /\ closed' = closed \cup {i}
             /\ UNCHANGED <<authorized, sent, possible, retained, nodes, work,
                            staged, cleaned, observed, attempts>>
Release(i) == /\ i \in nodes \cap closed /\ i \notin work
              /\ staged' = staged \ {i} /\ nodes' = nodes \ {i}
              /\ cleaned' = cleaned \cup {i}
              /\ UNCHANGED <<authorized, sent, possible, retained, closed, work,
                             observed, attempts>>
Observe(i) == /\ i \in cleaned \ observed
              /\ observed' = observed \cup {i} /\ possible' = possible \ {i}
              /\ UNCHANGED <<authorized, sent, retained, nodes, closed, work,
                             staged, cleaned, attempts>>
Retire(i) == /\ i \in retained \cap observed /\ i \notin nodes
             /\ retained' = retained \ {i}
             /\ UNCHANGED <<authorized, sent, possible, nodes, closed, work,
                            staged, cleaned, observed, attempts>>

(* Exhausted finite O fixture may rest after all work/uncertainty is gone. *)
Idle == /\ authorized = Creators /\ nodes = {} /\ possible = {}
        /\ UNCHANGED vars
Next == Idle \/ \E i \in Creators : Authorize(i) \/ Send(i) \/ Withdraw(i) \/ Admit(i)
        \/ Stage(i) \/ Finish(i) \/ Cancel(i) \/ Release(i) \/ Observe(i) \/ Retire(i)

TypeOK == /\ authorized \subseteq Creators /\ sent \subseteq authorized
          /\ possible \subseteq authorized /\ retained \subseteq sent
          /\ nodes \subseteq retained /\ closed \subseteq sent
          /\ work \subseteq nodes /\ staged \subseteq nodes
          /\ cleaned \subseteq sent /\ observed \subseteq cleaned
          /\ attempts \in [Creators -> 0..1]
Capacity == /\ Cardinality(nodes) <= NodeLimit
            /\ Cardinality(retained) <= RecordLimit
SafeClosure == cleaned \cap (nodes \cup work \cup staged) = {}
UncertaintyRetained == sent \ observed \subseteq possible
NoWorkAfterClose == [][\A i \in closed : i \notin work => i \notin work']_vars
Spec == Init /\ [][Next]_vars
        /\ \A i \in Creators : WF_vars(Finish(i)) /\ WF_vars(Release(i))
CleanupProgress == \A i \in Creators : (i \in closed) ~> (i \in cleaned)
=============================================================================
