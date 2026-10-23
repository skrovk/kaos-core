--------------------------- MODULE HostExecution ---------------------------
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Channels, Supports, LocalRoles, InitialMode, InitialActive,
          NativeGranted, CanReturnNative, RemoteLive, CheckDelivery,
          AllowRemoval, AllowReuse, EnableData, EnableArchive, RuntimeInterference

Roles == Channels \X {"send", "recv"}
AllLocal == Roles
SenderLocal == {<<c, "send">> : c \in Channels}
RemoteRoles == Roles \ LocalRoles
Empty == [kind |-> "empty"]
NoEvent == "none"
Facts == {"ready", "active", "cleanup", "clean"}
Phases == {"preparing", "prepared", "configure", "main", "native",
           "exited", "disposed", "free"}
EndpointPhases == {"absent", "reserved", "ready", "active", "closing", "clean"}
MaxRevision == 64

VARIABLE s
vars == <<s>>

HasSupport(set) == \E support \in Supports : support \subseteq set
Pair(c) == {<<c, "send">>, <<c, "recv">>}
Accepted(state, fact) ==
    {c \in Channels : \A r \in Pair(c) : fact \in state.seen[r]}
LocalUsable(state, c) ==
    \A r \in Pair(c) \cap LocalRoles :
       state.endpoint[r].phase = "active"
Startup(state) ==
    /\ state.commit /\ state.dispatch /\ ~state.closing /\ ~state.nodeDue
    /\ HasSupport({c \in Accepted(state, "active") : LocalUsable(state, c)})
AllClosed(state) ==
    /\ state.phase = "free" /\ state.bytes = 0 /\ ~state.worker
    /\ ~state.access /\ state.endpointAccess = {}
    /\ \A r \in Roles : state.endpoint[r].phase = "clean"
AllKnownClosed(state) ==
    /\ state.nodeClean
    /\ \A r \in Roles : "clean" \in state.seen[r]

(* Only aggregate evidence changes create a new report revision. Cardinality
 * and exact wire increment counts are abstracted; each visible mutation below
 * is one revision. Copies, queue activity and ordinary ACKs create no facts. *)
View(state) ==
    <<state.ready, state.commit, state.dispatch, state.success, state.closing,
      state.nodeClean, state.exitSeen, state.cutoff, state.channelClosing, state.channelCutoff,
      state.seen, state.removal, state.removalCutoff>>
Change(next) ==
    s' = IF View(next) # View(s)
         THEN [next EXCEPT !.revision = @ + 1, !.retained = TRUE]
         ELSE next

InitialEndpoint(r, active) ==
    IF InitialMode = "closed"
    THEN [phase |-> "clean", facts |-> Facts]
    ELSE IF r[1] \in active
    THEN [phase |-> "active", facts |-> {"ready", "active"}]
    ELSE IF InitialMode = "fresh"
         THEN [phase |-> IF r \in LocalRoles THEN "reserved" ELSE "absent",
               facts |-> {}]
         ELSE [phase |-> "ready", facts |-> {"ready"}]

InitialState ==
    [generation |-> 0, record |-> TRUE,
     phase |-> IF InitialMode = "closed" THEN "free"
               ELSE IF InitialMode = "fresh" THEN "preparing"
               ELSE IF InitialMode = "prepared" THEN "prepared" ELSE "main",
     worker |-> InitialMode # "closed",
     bytes |-> IF InitialMode = "closed" THEN 0 ELSE 2, access |-> FALSE,
     grant |-> NativeGranted, ready |-> InitialMode # "fresh",
     commit |-> InitialMode # "fresh",
     dispatch |-> InitialMode \in {"running", "closed"},
     init |-> FALSE, success |-> InitialMode \in {"running", "closed"},
     closing |-> InitialMode = "closed", nodeClean |-> InitialMode = "closed",
     exitSeen |-> InitialMode = "closed",
     nodeDue |-> FALSE, channelDue |-> {},
     channelClosing |-> IF InitialMode = "closed" THEN Channels ELSE {},
     cleanupDue |-> FALSE, cutoff |-> FALSE, channelCleanupDue |-> {},
     channelCutoff |-> {}, removal |-> FALSE, removalDue |-> FALSE,
     removalCutoff |-> FALSE,
     endpoint |-> [r \in Roles |-> InitialEndpoint(r, InitialActive)],
     seen |-> [r \in Roles |-> InitialEndpoint(r, InitialActive).facts],
     prepared |-> IF InitialMode = "fresh" THEN LocalRoles ELSE Roles,
     activated |-> {r \in Roles : r[1] \in InitialActive},
     cleanupIssued |-> IF InitialMode = "closed" THEN Roles ELSE {},
     endpointAccess |-> {},
     queue |-> [c \in Channels |-> 0],
     notification |-> [c \in Channels |-> NoEvent], errors |-> 0,
     packet |-> IF InitialMode = "closed" /\ RemoteRoles # {}
                THEN [kind |-> "report", generation |-> 0,
                      role |-> CHOOSE r \in RemoteRoles : TRUE,
                      facts |-> {"ready", "active"}]
                ELSE Empty,
     exchange |-> Empty, attempts |-> 0,
     wire |-> IF InitialMode = "closed" /\ RemoteRoles # {}
              THEN [kind |-> "activate", generation |-> 0,
                    role |-> CHOOSE r \in RemoteRoles : TRUE]
              ELSE Empty,
     revision |-> 1, acknowledged |-> 0, retained |-> TRUE,
     snapshot |-> Empty, ack |-> Empty,
     background |-> IF RuntimeInterference THEN "running" ELSE "clean",
     backgroundDue |-> ~RuntimeInterference,
     backgroundMailbox |-> FALSE]
Init == s = InitialState

(* Runtime workers publish to reserved mailboxes. They cannot consume the
 * ordinary packet slot, and native calls keep the runtime worker/arena pinned. *)
PrepareComplete ==
    /\ s.phase = "preparing" /\ ~s.closing
    /\ Change([s EXCEPT !.phase = "prepared"])
AcceptPrepared ==
    /\ s.phase = "prepared" /\ ~s.ready /\ ~s.closing /\ ~s.nodeDue
    /\ Change([s EXCEPT !.ready = TRUE])
Commit ==
    /\ s.ready /\ ~s.commit /\ ~s.closing /\ ~s.nodeDue
    /\ HasSupport(Accepted(s, "ready") \ s.channelClosing)
    /\ Change([s EXCEPT !.commit = TRUE])
Dispatch ==
    /\ s.commit /\ ~s.dispatch /\ ~s.closing /\ ~s.nodeDue
    /\ HasSupport({c \in Accepted(s, "active") : LocalUsable(s, c)}
                   \ s.channelClosing)
    /\ Change([s EXCEPT !.dispatch = TRUE])
BeginConfigure ==
    /\ s.phase = "prepared" /\ Startup(s)
    /\ Change([s EXCEPT !.phase = "configure"])
EnterMain ==
    /\ s.phase = "configure" /\ Startup(s)
    /\ Change([s EXCEPT !.phase = "main"])
EmitInit ==
    /\ s.phase = "main" /\ ~s.init /\ ~s.success /\ ~s.closing
    /\ Change([s EXCEPT !.init = TRUE])
AcceptInit ==
    /\ s.init /\ s.phase \in {"main", "native"} /\ Startup(s)
    /\ Change([s EXCEPT !.init = FALSE, !.success = TRUE])
NativeEnter ==
    /\ RuntimeInterference /\ s.phase = "main" /\ s.grant /\ ~s.closing
    /\ Change([s EXCEPT !.phase = "native"])
NativeReturn ==
    /\ CanReturnNative /\ s.phase = "native"
    /\ Change([s EXCEPT !.phase = "main"])
Exit ==
    /\ s.phase \in {"preparing", "prepared", "configure", "main"}
    /\ Change([s EXCEPT !.phase = "exited"])
HoldAccess ==
    /\ RuntimeInterference
    /\ s.phase \in {"configure", "main"} /\ ~s.closing /\ ~s.access
    /\ Change([s EXCEPT !.access = TRUE])
ReturnAccess ==
    /\ s.access /\ Change([s EXCEPT !.access = FALSE])
Stop ==
    /\ s.closing /\ s.phase \in {"preparing", "prepared", "configure", "main"}
    /\ Change([s EXCEPT !.phase = "exited", !.init = FALSE])
Dispose ==
    /\ s.closing /\ s.phase = "exited" /\ ~s.access
    /\ Change([s EXCEPT !.phase = "disposed"])
AcceptDisposed ==
    /\ s.phase = "disposed" /\ s.worker
    /\ Change([s EXCEPT !.worker = FALSE, !.bytes = 0, !.phase = "free"])
AcceptNodeClean ==
    /\ s.closing /\ s.phase = "free" /\ ~s.worker /\ s.bytes = 0
    /\ ~s.nodeClean /\ (~s.cleanupDue \/ s.cutoff)
    /\ (~s.removalDue \/ s.removalCutoff)
    /\ \A r \in LocalRoles : s.endpoint[r].phase = "clean"
    /\ Change([s EXCEPT !.nodeClean = TRUE])

CloseNode ==
    /\ ~s.closing
    /\ (s.phase = "exited" \/ (~s.success /\ s.nodeDue)
        \/ (~s.success /\ ~HasSupport(Channels \ s.channelClosing)))
    /\ Change([s EXCEPT !.closing = TRUE, !.init = FALSE,
        !.exitSeen = s.phase = "exited",
        !.channelClosing = IF s.success
                           THEN @ \cup (Channels \ Accepted(s, "active"))
                           ELSE Channels])
RemoveNode ==
    /\ AllowRemoval /\ ~s.removal /\ s.record
    /\ Change([s EXCEPT !.removal = TRUE, !.closing = TRUE, !.init = FALSE,
                         !.channelClosing = Channels])
RemoveEdge(c) ==
    /\ AllowRemoval /\ c \notin s.channelClosing
    /\ Change([s EXCEPT !.channelClosing = @ \cup {c}])

(* Clock comparisons, independently enabled from servicing a timer or packet.
 * Node/channel original deadlines never reset. A channel's allowance is at
 * least the node allowance. Each cleanup clock starts at its first close. *)
NodeTime ==
    /\ ~s.nodeDue /\ Change([s EXCEPT !.nodeDue = TRUE])
ChannelTime(c) ==
    /\ s.nodeDue /\ c \notin s.channelDue
    /\ Change([s EXCEPT !.channelDue = @ \cup {c}])
CleanupTime ==
    /\ s.closing /\ ~s.cleanupDue /\ ~AllKnownClosed(s) /\ ~s.success
    /\ Change([s EXCEPT !.cleanupDue = TRUE])
ChannelCleanupTime(c) ==
    /\ c \in s.channelClosing /\ c \notin s.channelCleanupDue
    /\ ~(\A r \in Pair(c) : "clean" \in s.seen[r])
    /\ Change([s EXCEPT !.channelCleanupDue = @ \cup {c}])
RemovalTime ==
    /\ s.removal /\ ~s.removalDue /\ ~AllKnownClosed(s)
    /\ Change([s EXCEPT !.removalDue = TRUE])
ExpireChannel(c) ==
    /\ c \in s.channelDue /\ c \notin s.channelClosing
    /\ c \notin Accepted(s, "active")
    /\ Change([s EXCEPT !.channelClosing = @ \cup {c}])
Cutoff ==
    /\ s.cleanupDue /\ ~s.cutoff /\ ~AllKnownClosed(s)
    /\ Change([s EXCEPT !.cutoff = TRUE])
ChannelCutoff(c) ==
    /\ c \in s.channelCleanupDue /\ c \notin s.channelCutoff
    /\ Change([s EXCEPT !.channelCutoff = @ \cup {c}])
RemovalCutoff ==
    /\ s.removalDue /\ ~s.removalCutoff /\ ~AllKnownClosed(s)
    /\ Change([s EXCEPT !.removalCutoff = TRUE])
Urgent ==
    \/ (~s.success /\ s.nodeDue /\ ~s.closing)
    \/ (s.cleanupDue /\ ~s.cutoff /\ ~AllKnownClosed(s))
    \/ (s.removalDue /\ ~s.removalCutoff /\ ~AllKnownClosed(s))
    \/ \E c \in Channels :
         (c \in s.channelDue /\ c \notin s.channelClosing
          /\ c \notin Accepted(s, "active"))
         \/ (c \in s.channelCleanupDue /\ c \notin s.channelCutoff)

NewEndpoint(phase, old) ==
    [phase |-> phase,
     facts |-> old.facts \cup
         CASE phase = "ready" -> {"ready"}
           [] phase = "active" -> {"active"}
           [] phase = "closing" -> {"cleanup"}
           [] phase = "clean" -> {"cleanup", "clean"}
           [] OTHER -> {}]
CanActivate(r) ==
    /\ s.commit /\ r[1] \notin s.channelClosing
    /\ r[1] \notin s.channelDue /\ ~s.closing
    /\ r[1] \in Accepted(s, "ready")
    /\ (r[2] = "recv" \/ "active" \in s.seen[<<r[1], "recv">>])

(* Local endpoint work is a bounded control pass, not an invented worker task.
 * Local facts can be accepted in that same registry critical section. *)
LocalPrepare(r) ==
    /\ EnableData /\ r \in LocalRoles /\ s.endpoint[r].phase = "reserved"
    /\ r[1] \notin s.channelClosing /\ ~s.closing
    /\ LET e == NewEndpoint("ready", s.endpoint[r]) IN
       Change([s EXCEPT !.endpoint[r] = e, !.seen[r] = e.facts])
LocalPrepareAll ==
    /\ ~EnableData /\ ~s.closing
    /\ LET targets == {r \in LocalRoles :
             s.endpoint[r].phase = "reserved" /\ r[1] \notin s.channelClosing}
       IN /\ targets # {}
          /\ Change([s EXCEPT
              !.endpoint = [r \in Roles |->
                  IF r \in targets THEN NewEndpoint("ready", s.endpoint[r]) ELSE s.endpoint[r]],
              !.seen = [r \in Roles |->
                  IF r \in targets THEN s.seen[r] \cup {"ready"} ELSE s.seen[r]]])
LocalActivate(r) ==
    /\ r \in LocalRoles /\ s.endpoint[r].phase = "ready" /\ CanActivate(r)
    /\ LET e == NewEndpoint("active", s.endpoint[r]) IN
       Change([s EXCEPT !.endpoint[r] = e, !.seen[r] = e.facts,
              !.activated = @ \cup {r}, !.notification[r[1]] = "enabled"])
LocalClose(r) ==
    /\ EnableData /\ r \in LocalRoles /\ (r[1] \in s.channelClosing \/ s.closing)
    /\ s.endpoint[r].phase \notin {"closing", "clean"}
    /\ LET e == NewEndpoint("closing", s.endpoint[r]) IN
       Change([s EXCEPT !.endpoint[r] = e, !.seen[r] = e.facts,
                         !.notification[r[1]] = "disabled"])
LocalCloseFree ==
    /\ ~EnableData /\ ~Urgent
    /\ LET targets == {r \in LocalRoles :
             (r[1] \in s.channelClosing \/ s.closing) /\
             s.endpoint[r].phase # "clean"}
           channels == {r[1] : r \in targets}
       IN /\ targets # {}
          /\ Change([s EXCEPT
              !.endpoint = [r \in Roles |->
                  IF r \in targets THEN NewEndpoint("clean", s.endpoint[r]) ELSE s.endpoint[r]],
              !.seen = [r \in Roles |->
                  IF r \in targets THEN s.endpoint[r].facts \cup {"cleanup", "clean"}
                                   ELSE s.seen[r]],
              !.queue = [c \in Channels |-> IF c \in channels THEN 0 ELSE s.queue[c]],
              !.notification = [c \in Channels |->
                  IF c \in channels THEN "disabled" ELSE s.notification[c]]])
EndpointFree(r) ==
    /\ s.endpoint[r].phase = "closing" /\ r \notin s.endpointAccess
    /\ (EnableData \/ r \in RemoteRoles)
    /\ (r \in LocalRoles \/ RemoteLive)
    /\ LET e == NewEndpoint("clean", s.endpoint[r]) IN
       Change([s EXCEPT !.endpoint[r] = e, !.queue[r[1]] = 0])
AcceptLocalClean(r) ==
    /\ r \in LocalRoles /\ s.endpoint[r].phase = "clean"
    /\ "clean" \notin s.seen[r] /\ ~Urgent
    /\ Change([s EXCEPT !.seen[r] = s.endpoint[r].facts])
HoldEndpoint(r) ==
    /\ EnableData /\ s.endpoint[r].phase = "active" /\ r \notin s.endpointAccess
    /\ r[1] \notin s.channelClosing
    /\ Change([s EXCEPT !.endpointAccess = @ \cup {r}])
ReturnEndpoint(r) ==
    /\ r \in s.endpointAccess
    /\ Change([s EXCEPT !.endpointAccess = @ \ {r}])

Command(kind, r) == [kind |-> kind, role |-> r, generation |-> s.generation]
IssuePrepare(r) ==
    /\ r \in RemoteRoles /\ r \notin s.prepared
    /\ s.exchange.kind = "empty" /\ r[1] \notin s.channelDue
    /\ r[1] \notin s.channelClosing /\ ~s.closing
    /\ Change([s EXCEPT !.prepared = @ \cup {r},
                         !.exchange = Command("prepare", r), !.attempts = 0])
IssueActivate(r) ==
    /\ r \in RemoteRoles /\ r \notin s.activated /\ CanActivate(r)
    /\ s.exchange.kind = "empty"
    /\ Change([s EXCEPT !.activated = @ \cup {r},
                         !.exchange = Command("activate", r), !.attempts = 0])
IssueCleanup(r) ==
    /\ r \in RemoteRoles /\ r \notin s.cleanupIssued
    /\ r[1] \in s.channelClosing /\ r[1] \notin s.channelCleanupDue
    /\ s.exchange.kind = "empty"
    /\ Change([s EXCEPT !.cleanupIssued = @ \cup {r},
                         !.exchange = Command("cleanup", r), !.attempts = 0])
Transmit ==
    /\ s.exchange.kind # "empty" /\ s.wire.kind = "empty" /\ s.attempts < 2
    /\ Change([s EXCEPT !.wire = s.exchange, !.attempts = @ + 1])
LoseCommand ==
    /\ s.wire.kind # "empty" /\ Change([s EXCEPT !.wire = Empty])
ExchangeExpired ==
    /\ s.exchange.kind # "empty"
    /\ LET c == s.exchange.role[1] IN
       (IF s.exchange.kind = "cleanup"
       THEN c \in s.channelCleanupDue
       ELSE c \in s.channelDue \/ c \in s.channelClosing)
    /\ Change([s EXCEPT !.exchange = Empty])
DeliverCommand ==
    /\ RemoteLive /\ s.wire.kind # "empty"
    /\ LET m == s.wire
           r == m.role
           current == m.generation = s.generation /\ s.record
           phase == s.endpoint[r].phase
           nextPhase ==
             IF ~current THEN phase
             ELSE CASE m.kind = "cleanup" /\ phase # "clean" -> "closing"
                    [] m.kind = "prepare" /\ phase = "absent" -> "ready"
                    [] m.kind = "activate" /\ phase = "ready" -> "active"
                    [] OTHER -> phase
           e == NewEndpoint(nextPhase, s.endpoint[r])
       IN Change([s EXCEPT !.wire = Empty,
             !.exchange = IF s.exchange.kind = m.kind /\
                              s.exchange.generation = m.generation /\
                              s.exchange.role = m.role THEN Empty ELSE @,
             !.endpoint[r] = e])

(* Owners retain cumulative facts independently of a bounded ordinary packet.
 * Noise and reports may fill that slot; INIT/completion publication never
 * depends on its vacancy. A delayed report retains its original generation. *)
Noise ==
    /\ (RuntimeInterference \/ EnableData)
    /\ s.packet.kind = "empty" /\ Change([s EXCEPT !.packet = [kind |-> "noise"]])
ReportArrival(r) ==
    /\ RemoteLive /\ r \in RemoteRoles /\ s.packet.kind = "empty"
    /\ s.endpoint[r].facts # s.seen[r]
    /\ Change([s EXCEPT !.packet =
        [kind |-> "report", generation |-> s.generation,
         role |-> r, facts |-> s.endpoint[r].facts]])
ServicePacket ==
    /\ s.packet.kind # "empty" /\ ~Urgent
    /\ IF s.packet.kind = "noise"
       THEN Change([s EXCEPT !.packet = Empty])
       ELSE LET m == s.packet
                r == m.role
                valid ==
                  /\ m.generation = s.generation /\ s.record
                  /\ (r \in s.prepared \/ r \in s.cleanupIssued)
                  /\ ("active" \in m.facts => r \in s.activated)
            IN Change([s EXCEPT !.packet = Empty,
                 !.seen[r] = IF valid THEN @ \cup m.facts ELSE @])

Put(c) ==
    /\ EnableData /\ s.phase \in {"configure", "main", "native"}
    /\ ~s.closing /\ s.endpoint[<<c, "send">>].phase = "active"
    /\ s.queue[c] = 0
    /\ Change([s EXCEPT !.queue[c] = 1])
GetOrSend(c) ==
    /\ EnableData /\ s.queue[c] = 1
    /\ Change([s EXCEPT !.queue[c] = 0])
GuestError ==
    /\ RuntimeInterference
    /\ s.phase \in {"configure", "main", "native"} /\ ~s.closing /\ s.errors = 0
    /\ Change([s EXCEPT !.errors = 1])
DrainError ==
    /\ s.errors = 1 /\ Change([s EXCEPT !.errors = 0])
ConsumeNotification(c) ==
    /\ s.notification[c] # NoEvent
    /\ Change([s EXCEPT !.notification[c] = NoEvent])

(* The second runtime abstracts unrelated admitted work. Its retained
 * completion and deadline must progress even while the focal native call
 * cannot return. It shares the same serialized control task and packet load. *)
BackgroundTime ==
    /\ ~s.backgroundDue /\ Change([s EXCEPT !.backgroundDue = TRUE])
BackgroundCancel ==
    /\ s.backgroundDue /\ s.background = "running"
    /\ Change([s EXCEPT !.background = "stopping"])
BackgroundDispose ==
    /\ s.background = "stopping"
    /\ Change([s EXCEPT !.background = "disposed", !.backgroundMailbox = TRUE])
BackgroundAccept ==
    /\ s.backgroundMailbox
    /\ Change([s EXCEPT !.background = "clean", !.backgroundMailbox = FALSE])

Query ==
    /\ EnableArchive /\ s.record /\ s.retained /\ s.snapshot.kind = "empty"
    /\ (~AllowReuse \/ s.generation = 0)
    /\ Change([s EXCEPT !.snapshot =
        [kind |-> "snapshot", generation |-> s.generation,
         revision |-> s.revision, facts |-> View(s)]])
QueueAck ==
    /\ s.snapshot.kind # "empty" /\ s.ack.kind = "empty"
    /\ Change([s EXCEPT !.ack =
        [kind |-> "ack", generation |-> s.snapshot.generation,
         revision |-> s.snapshot.revision]])
DeliverAck ==
    /\ s.ack.kind # "empty"
    /\ LET exact == s.record /\ s.ack.generation = s.generation
                          /\ s.ack.revision = s.revision
       IN Change([s EXCEPT !.ack = Empty,
            !.retained = IF exact THEN FALSE ELSE @,
            !.acknowledged = IF exact THEN s.revision ELSE @])
CollectorAck ==
    /\ s.snapshot.kind # "empty" /\ s.ack.kind # "empty"
    /\ Change([s EXCEPT !.snapshot = Empty])
Retire ==
    /\ AllowReuse /\ s.generation = 0 /\ s.record
    /\ AllClosed(s) /\ AllKnownClosed(s) /\ ~s.retained
    /\ Change([s EXCEPT !.record = FALSE])
FreshInstance ==
    /\ AllowReuse /\ s.generation = 0 /\ ~s.record
    /\ LET fresh == [InitialState EXCEPT
          !.generation = 1, !.phase = "preparing", !.ready = FALSE,
          !.worker = TRUE, !.bytes = 2, !.closing = FALSE, !.nodeClean = FALSE,
          !.exitSeen = FALSE, !.channelClosing = {}, !.cleanupIssued = {},
          !.commit = FALSE, !.dispatch = FALSE, !.success = FALSE,
          !.grant = FALSE, !.record = TRUE,
          !.endpoint = [r \in Roles |->
              [phase |-> IF r \in LocalRoles THEN "reserved" ELSE "absent", facts |-> {}]],
          !.seen = [r \in Roles |-> {}], !.prepared = LocalRoles,
          !.activated = {}, !.packet = s.packet, !.wire = s.wire,
          !.snapshot = s.snapshot, !.ack = s.ack]
       IN s' = fresh

Next ==
    PrepareComplete \/ AcceptPrepared \/ Commit \/ Dispatch \/ BeginConfigure
    \/ EnterMain \/ EmitInit \/ AcceptInit \/ NativeEnter \/ NativeReturn \/ Exit
    \/ HoldAccess \/ ReturnAccess \/ Stop \/ Dispose \/ AcceptDisposed
    \/ AcceptNodeClean \/ CloseNode \/ RemoveNode
    \/ NodeTime \/ CleanupTime \/ Cutoff \/ RemovalTime \/ RemovalCutoff
    \/ Transmit \/ LoseCommand \/ ExchangeExpired \/ DeliverCommand
    \/ Noise \/ ServicePacket \/ GuestError \/ DrainError
    \/ BackgroundTime \/ BackgroundCancel \/ BackgroundDispose \/ BackgroundAccept
    \/ Query \/ QueueAck \/ DeliverAck \/ CollectorAck \/ Retire \/ FreshInstance
    \/ LocalPrepareAll \/ LocalCloseFree
    \/ (\E c \in Channels : RemoveEdge(c) \/ ChannelTime(c) \/ ExpireChannel(c)
          \/ ChannelCleanupTime(c) \/ ChannelCutoff(c)
          \/ Put(c) \/ GetOrSend(c) \/ ConsumeNotification(c))
    \/ (\E r \in Roles : LocalPrepare(r) \/ LocalActivate(r) \/ LocalClose(r)
          \/ EndpointFree(r) \/ AcceptLocalClean(r) \/ HoldEndpoint(r)
          \/ ReturnEndpoint(r) \/ IssuePrepare(r) \/ IssueActivate(r)
          \/ IssueCleanup(r) \/ ReportArrival(r))

(* Each enabled bounded control/worker obligation is serviced. This abstracts
 * the deterministic bounded scans around each packet in core_host_step.
 * Guest INIT, removal authorization, successful preparation before deadlines,
 * and native entry are deliberately not fairness assumptions. *)
FairService ==
    WF_vars(PrepareComplete) /\ WF_vars(AcceptPrepared)
    /\ WF_vars(Commit) /\ WF_vars(Dispatch) /\ WF_vars(BeginConfigure)
    /\ WF_vars(EnterMain) /\ WF_vars(AcceptInit)
    /\ WF_vars(NativeReturn) /\ WF_vars(ReturnAccess) /\ WF_vars(Stop)
    /\ WF_vars(Dispose) /\ WF_vars(AcceptDisposed) /\ WF_vars(AcceptNodeClean)
    /\ WF_vars(CloseNode) /\ WF_vars(NodeTime) /\ WF_vars(CleanupTime)
    /\ WF_vars(Cutoff) /\ WF_vars(RemovalTime) /\ WF_vars(RemovalCutoff)
    /\ WF_vars(ExchangeExpired) /\ WF_vars(ServicePacket) /\ WF_vars(DrainError)
    /\ WF_vars(BackgroundTime) /\ WF_vars(BackgroundCancel)
    /\ WF_vars(BackgroundDispose) /\ WF_vars(BackgroundAccept)
    /\ WF_vars(LocalPrepareAll) /\ WF_vars(LocalCloseFree)
    /\ (\A c \in Channels : WF_vars(ChannelTime(c)) /\ WF_vars(ExpireChannel(c))
          /\ WF_vars(ChannelCleanupTime(c)) /\ WF_vars(ChannelCutoff(c)))
    /\ (\A r \in Roles : WF_vars(LocalPrepare(r)) /\ WF_vars(LocalActivate(r))
          /\ WF_vars(LocalClose(r)) /\ WF_vars(EndpointFree(r))
          /\ WF_vars(AcceptLocalClean(r)) /\ WF_vars(ReturnEndpoint(r))
          /\ WF_vars(IssuePrepare(r)) /\ WF_vars(IssueActivate(r))
          /\ WF_vars(IssueCleanup(r)))
DeliveryFair ==
    IF CheckDelivery
    THEN WF_vars(Transmit) /\ SF_vars(DeliverCommand)
         /\ (\A r \in RemoteRoles : SF_vars(ReportArrival(r)))
    ELSE TRUE
Spec == Init /\ [][Next]_vars /\ FairService /\ DeliveryFair

TypeOK ==
    /\ s.generation \in 0..1 /\ s.phase \in Phases /\ s.bytes \in 0..2
    /\ s.revision \in 1..MaxRevision /\ s.acknowledged \in 0..MaxRevision
    /\ s.endpointAccess \subseteq Roles /\ s.channelClosing \subseteq Channels
    /\ s.channelDue \subseteq Channels /\ s.channelCleanupDue \subseteq Channels
    /\ s.channelCutoff \subseteq Channels
    /\ s.errors \in 0..1 /\ s.attempts \in 0..2
    /\ s.queue \in [Channels -> 0..1]
    /\ s.notification \in [Channels -> {NoEvent, "enabled", "disabled"}]
    /\ s.seen \in [Roles -> SUBSET Facts]
    /\ s.endpoint \in [Roles -> [phase : EndpointPhases, facts : SUBSET Facts]]
WorkerPinned ==
    s.phase \notin {"disposed", "free"} => (s.worker /\ s.bytes = 2 /\ ~s.nodeClean)
NativeAuthorized == s.phase = "native" => s.grant
SafeReclamation ==
    s.nodeClean =>
      /\ s.phase = "free" /\ ~s.worker /\ s.bytes = 0 /\ ~s.access
      /\ \A r \in LocalRoles : s.endpoint[r].phase = "clean"
EndpointReclaimed ==
    \A r \in Roles : s.endpoint[r].phase = "clean" => r \notin s.endpointAccess
ReservedCompletion ==
    s.init => (s.worker /\ ~s.nodeClean)
FullBeforeRetirement ==
    ~s.record => AllClosed(s) /\ AllKnownClosed(s)
ExactVisibility ==
    ~s.retained => s.acknowledged = s.revision
NoNewGrantOnReuse ==
    s.generation = 1 => ~s.grant
NoInventedFacts ==
    \A r \in Roles : s.seen[r] \subseteq s.endpoint[r].facts
ActivationAuthorized ==
    (\E r \in Roles : s.endpoint[r].phase = "active") => s.commit
StartupAcceptance ==
    [][(s.generation = s'.generation /\ ~s.success /\ s'.success) =>
       (s.init /\ s.phase \in {"main", "native"} /\ Startup(s))]_vars
HistoricalSuccess ==
    [][(s.generation = s'.generation /\ s.success) => s'.success]_vars
DisposalAfterCalls ==
    [][(s.phase # "disposed" /\ s'.phase = "disposed") =>
       (s.phase = "exited" /\ ~s.access /\ s.closing)]_vars
ReceiverFirst ==
    [][\A r \in Roles :
       (r \notin s.activated /\ r \in s'.activated /\ r[2] = "send") =>
          ("active" \in s.seen[<<r[1], "recv">>])]_vars
DeadlineMonotonic ==
    [][s.generation = s'.generation =>
       ((s.nodeDue => s'.nodeDue) /\ s.channelDue \subseteq s'.channelDue
        /\ s.channelCleanupDue \subseteq s'.channelCleanupDue
        /\ (s.removalDue => s'.removalDue))]_vars
ExactAckAcceptance ==
    [][(s.generation = s'.generation /\ s.retained /\ ~s'.retained) =>
       (s.ack.kind = "ack" /\ s.ack.generation = s.generation /\
        s.ack.revision = s.revision)]_vars
StaleCommandIgnored ==
    [][(s.wire.kind # "empty" /\ s.wire.generation # s.generation /\
        s'.wire.kind = "empty") => s'.endpoint = s.endpoint]_vars
StaleReportIgnored ==
    [][(s.packet.kind = "report" /\ s.packet.generation # s.generation /\
        s'.packet.kind = "empty") => s'.seen = s.seen]_vars
CutoffBeforeLateClean ==
    [][\A r \in Roles :
       ("clean" \notin s.seen[r] /\ "clean" \in s'.seen[r]) =>
          (r[1] \notin s.channelCleanupDue \/ r[1] \in s.channelCutoff)]_vars
BackgroundProgress == <> (s.background = "clean")
OutcomeProgress == <> (s.success \/ AllKnownClosed(s) \/ s.cutoff)
RemovalOutcome == s.removal ~> (AllKnownClosed(s) \/ s.removalCutoff)
LocalCleanupProgress == s.closing ~> s.nodeClean
=============================================================================
