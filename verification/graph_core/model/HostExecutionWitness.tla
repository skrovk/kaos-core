----------------------- MODULE HostExecutionWitness -----------------------
EXTENDS HostExecution

(* Expected invariant violations establish reachable cases; they are not
 * safety failures. Full checks use HostExecution's unmodified Next/Spec. *)
NoStartupWitness == ~s.success
NoBlockedNativeWitness ==
    ~(s.phase = "native" /\ s.closing /\ s.removalCutoff /\
      s.background = "clean" /\ ~s.nodeClean)
NoQueuePressureWitness ==
    ~(s.phase = "native" /\ s.endpointAccess # {} /\ s.packet.kind = "noise" /\
      \E c \in Channels : s.queue[c] = 1)
NoStaleTrafficWitness ==
    ~(s.generation = 1 /\ s.wire.kind # "empty" /\
      s.wire.generation # s.generation /\ s.packet.kind = "report" /\
      s.packet.generation # s.generation /\ s.ack.kind = "ack" /\
      s.ack.generation # s.generation)
NoOptionalOverlapWitness ==
    ~(s.success /\ s.removal /\
      (s.channelCutoff \ (Accepted(s, "active"))) # {})
KeepStartupPossible == s.success \/ (~s.closing /\ ~s.nodeDue)

(* This reachability-only search delays authorized full removal until an
 * optional, still incomplete branch has already hit its cleanup cutoff.
 * It restricts the witness search, never the exhaustive safety runs. *)
OptionalWitnessSpec ==
    Init /\ [][Next /\
        ((~s.removal /\ s'.removal) =>
          s.success /\ (s.channelCutoff \ Accepted(s, "active")) # {})]_vars
=============================================================================
