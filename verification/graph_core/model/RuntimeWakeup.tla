-------------------------- MODULE RuntimeWakeup --------------------------
EXTENDS Naturals, FiniteSets

CONSTANTS Slots, TimedSlots
Commands == {"start", "cancel", "dispose"}
VARIABLES phase, token, issued, handled
vars == <<phase, token, issued, handled>>

(* Exactly one worker waits on each slot. "check" and "release" hold its
 * mailbox mutex; "take" and "acquire" do not. Control publication includes
 * the command update and semaphore give within one mutex critical section.
 * Commands are abstract retained obligations, not guest execution states. *)
Locked(s) == phase[s] \in {"check", "release"}
Pending(s) == issued[s] \ handled[s]

Init == /\ phase = [s \in Slots |-> "check"]
        /\ token = [s \in Slots |-> FALSE]
        /\ issued = [s \in Slots |-> {}]
        /\ handled = [s \in Slots |-> {}]

Publish(s, command) ==
    /\ ~Locked(s)
    /\ command \notin issued[s]
    /\ issued' = [issued EXCEPT ![s] = @ \cup {command}]
    /\ token' = [token EXCEPT ![s] = TRUE]
    /\ UNCHANGED <<phase, handled>>

(* Ordinary unlocks may give a token even without changing a command.
 * Further gives coalesce at capacity one and do not overwrite evidence. *)
OrdinarySignal(s) ==
    /\ ~Locked(s)
    /\ token' = [token EXCEPT ![s] = TRUE]
    /\ UNCHANGED <<phase, issued, handled>>

Handle(s) ==
    /\ phase[s] = "check"
    /\ Pending(s) # {}
    /\ \E command \in Pending(s):
          handled' = [handled EXCEPT ![s] = @ \cup {command}]
    /\ UNCHANGED <<phase, token, issued>>

CheckEmpty(s) ==
    /\ phase[s] = "check"
    /\ Pending(s) = {}
    /\ phase' = [phase EXCEPT ![s] = "release"]
    /\ UNCHANGED <<token, issued, handled>>

Unlock(s) ==
    /\ phase[s] = "release"
    /\ phase' = [phase EXCEPT ![s] = "take"]
    /\ UNCHANGED <<token, issued, handled>>

Take(s) ==
    /\ phase[s] = "take"
    /\ token[s]
    /\ token' = [token EXCEPT ![s] = FALSE]
    /\ phase' = [phase EXCEPT ![s] = "acquire"]
    /\ UNCHANGED <<issued, handled>>

(* A finite wait can expire without consuming a token. One configured slot
 * has no timeout, so progress cannot be rescued by periodic polling. *)
TimedWake(s) ==
    /\ s \in TimedSlots
    /\ phase[s] = "take"
    /\ phase' = [phase EXCEPT ![s] = "acquire"]
    /\ UNCHANGED <<token, issued, handled>>

Acquire(s) ==
    /\ phase[s] = "acquire"
    /\ phase' = [phase EXCEPT ![s] = "check"]
    /\ UNCHANGED <<token, issued, handled>>

Worker(s) == Handle(s) \/ CheckEmpty(s) \/ Unlock(s) \/ Take(s)
             \/ TimedWake(s) \/ Acquire(s)
Next == \E s \in Slots:
            Worker(s) \/ OrdinarySignal(s)
            \/ (\E command \in Commands: Publish(s, command))

(* Weak fairness services an enabled worker. Strong fairness for publication
 * models fair control acquisition of a mutex that is repeatedly available;
 * ordinary scheduling fairness alone does not imply fair mutex acquisition. *)
Spec == Init /\ [][Next]_vars
        /\ (\A s \in Slots: WF_vars(Worker(s)))
        /\ (\A s \in Slots, command \in Commands:
                 SF_vars(Publish(s, command)))

TypeOK == /\ phase \in [Slots -> {"check", "release", "take", "acquire"}]
          /\ token \in [Slots -> BOOLEAN]
          /\ issued \in [Slots -> SUBSET Commands]
          /\ handled \in [Slots -> SUBSET Commands]
HandledOnlyIssued == \A s \in Slots: handled[s] \subseteq issued[s]
NoSleepingPending == \A s \in Slots:
    (phase[s] = "take" /\ Pending(s) # {}) => token[s]
CheckedBeforeUnlock == \A s \in Slots:
    phase[s] = "release" => Pending(s) = {}

PendingProgress == \A s \in Slots, command \in Commands:
    (command \in Pending(s)) ~> (command \in handled[s])
ControlProgress == \A s \in Slots, command \in Commands:
    <> (command \in handled[s])
=============================================================================
