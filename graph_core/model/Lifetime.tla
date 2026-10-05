---------------------------- MODULE Lifetime ----------------------------
EXTENDS Naturals, FiniteSets

CONSTANTS Slots, MaxTicket, MaxBytes
VARIABLES closing, bytes, held, lastTicket, phase, token, result
vars == <<closing, bytes, held, lastTicket, phase, token, result>>

Init == /\ closing = FALSE
        /\ bytes = 0
        /\ held = {}
        /\ lastTicket = 0
        /\ phase = [s \in Slots |-> "empty"]
        /\ token = [s \in Slots |-> 0]
        /\ result = [s \in Slots |-> "none"]

Charge == /\ ~closing /\ bytes < MaxBytes
          /\ bytes' = bytes + 1
          /\ UNCHANGED <<closing, held, lastTicket, phase, token, result>>

Issue(s) == /\ ~closing /\ phase[s] = "empty"
            /\ Cardinality(held) < Cardinality(Slots)
            /\ lastTicket < MaxTicket
            /\ lastTicket' = lastTicket + 1
            /\ held' = held \cup {lastTicket + 1}
            /\ token' = [token EXCEPT ![s] = lastTicket + 1]
            /\ phase' = [phase EXCEPT ![s] = "issued"]
            /\ result' = [result EXCEPT ![s] = "none"]
            /\ UNCHANGED <<closing, bytes>>

Begin(s) == /\ phase[s] = "issued"
            /\ phase' = [phase EXCEPT ![s] = IF closing THEN "completed"
                                                          ELSE "executing"]
            /\ result' = [result EXCEPT ![s] = IF closing THEN "cancelled"
                                                           ELSE "none"]
            /\ UNCHANGED <<closing, bytes, held, lastTicket, token>>

Complete(s) == /\ phase[s] = "executing"
               /\ phase' = [phase EXCEPT ![s] = "completed"]
               /\ result' = [result EXCEPT ![s] = "done"]
               /\ UNCHANGED <<closing, bytes, held, lastTicket, token>>

Accept(s, t) == /\ phase[s] = "completed" /\ token[s] = t /\ t \in held
                /\ held' = held \ {t}
                /\ phase' = [phase EXCEPT ![s] = "empty"]
                /\ token' = [token EXCEPT ![s] = 0]
                /\ result' = [result EXCEPT ![s] = "none"]
                /\ UNCHANGED <<closing, bytes, lastTicket>>

Close == /\ ~closing /\ closing' = TRUE
         /\ UNCHANGED <<bytes, held, lastTicket, phase, token, result>>

Uncharge == /\ closing /\ held = {} /\ bytes > 0
            /\ bytes' = bytes - 1
            /\ UNCHANGED <<closing, held, lastTicket, phase, token, result>>

Reopen == /\ closing /\ held = {} /\ bytes = 0 /\ lastTicket < MaxTicket
          /\ closing' = FALSE
          /\ UNCHANGED <<bytes, held, lastTicket, phase, token, result>>

Next == Charge \/ Close \/ Uncharge \/ Reopen
        \/ \E s \in Slots : Issue(s) \/ Begin(s) \/ Complete(s)
            \/ \E t \in 1..MaxTicket : Accept(s, t)

TypeOK == /\ closing \in BOOLEAN /\ bytes \in 0..MaxBytes
          /\ lastTicket \in 0..MaxTicket /\ held \subseteq 1..lastTicket
          /\ phase \in [Slots -> {"empty", "issued", "executing", "completed"}]
          /\ token \in [Slots -> 0..MaxTicket]
          /\ result \in [Slots -> {"none", "done", "cancelled"}]

Bounded == Cardinality(held) <= Cardinality(Slots)
PinnedThroughAcceptance ==
    held = {token[s] : s \in {a \in Slots : phase[a] # "empty"}}
UniqueTickets == \A a, b \in Slots :
    (a # b /\ phase[a] # "empty" /\ phase[b] # "empty") => token[a] # token[b]
Quiescence == held = {} => \A s \in Slots : phase[s] = "empty"

NoExecutionAfterClose == [][\A s \in Slots :
    (closing /\ phase[s] = "issued") => phase'[s] # "executing"]_vars
SafeRelease == [][bytes' < bytes => (closing /\ held = {})]_vars

(* Fair worker completion means executing work eventually returns. This is an
 * assumption for this foundation model, not a WAMR cancellation guarantee.
 * Issued work must be serviced even after close to publish cancellation. *)
Spec == Init /\ [][Next]_vars
        /\ \A s \in Slots : WF_vars(Begin(s)) /\ WF_vars(Complete(s))
            /\ \A t \in 1..MaxTicket : WF_vars(Accept(s, t))
        /\ WF_vars(Uncharge)

CleanupProgress == closing ~> (held = {} /\ bytes = 0)
=============================================================================
