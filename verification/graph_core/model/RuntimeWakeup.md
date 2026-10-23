# ESP32 retained wakeup model

`RuntimeWakeup.tla` checks the ESP-IDF binary-semaphore wakeup path in
[`kaos_wamr.c`](../../../graph_core/runtime/kaos_wamr.c). It complements
`NodeRuntime.tla`; it does not replace the lifecycle model or target tests.
The configuration has two independent slots, one with an indefinite wait and
one with a finite wait, each with exactly one worker and a capacity-one token.

| Model action/state | Concrete boundary |
| --- | --- |
| `Publish` | Control holds the mailbox mutex, sets a retained command, calls `signal_worker`, then unlocks. Start, cancel and dispose stand for pending work, not for completed execution. |
| `Handle` | A worker or cancellable import inspects the retained predicate while holding the mailbox mutex and services that obligation. The model abstracts the duration and result of the service. |
| `CheckEmpty`, `release` | The worker checks its predicate while holding the mutex, before entering `wait_worker`. Control cannot publish between this check and mutex release. |
| `Unlock`, `take` | `wait_worker` releases the mutex before `xSemaphoreTake`. Control can publish in this interval, including before the actual take begins. |
| `Take`, `acquire` | A retained token is consumed, after which `wait_worker` reacquires the mutex. Further publication during reacquisition remains visible in the mailbox. |
| `TimedWake` | A finite wait expires and returns to mutex acquisition without consuming a token. Timeout itself cannot create or complete a command. |
| `OrdinarySignal` | An ordinary runtime `unlock` may signal even without a command change. Repeated gives coalesce to one token. |

The token is a scheduling hint, **not evidence**. `issued` and `handled` retain
the abstract obligations independently of token consumption. The safety checks
require type correctness, no handling of unissued work, an empty predicate
through the locked check/release interval, and a token whenever a worker is at
the semaphore wait with pending work. Liveness checks require every published
command to be serviced and, under fair control publication, every configured
command eventually to be published and serviced. The indefinite-wait slot
ensures periodic timeouts cannot conceal a lost wakeup.

Weak fairness is assumed for each worker's enabled transitions. Strong fairness
for each control publication assumes fair mutex acquisition when the mutex is
repeatedly available. This is stronger than merely scheduling the control task:
the model does not prove FreeRTOS mutex fairness or a numeric latency bound.
The signal/update/unlock critical section is abstracted as one atomic action;
it must finish in bounded time in the implementation.

The finite model publishes each of three command kinds at most once per slot.
It does not model slot reuse, identities, guest bytecode, WAMR interruption,
memory reclamation, deadline arithmetic, tick rounding, semaphore allocation,
shutdown or worker priorities. Actual processing and cancellation are assumed
to return to the modeled predicate loop. In particular, progress here cannot
prove that a noncooperative guest terminates. Runtime tests, `NodeRuntime` and
physical scheduling/resource measurements cover separate obligations.

Run from the repository root:

```sh
java -XX:+UseParallelGC -Xmx1g -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC \
  -workers 1 -seed 42 -metadir /tmp/kaos-p3-session/wakeup-tlc \
  -config verification/graph_core/model/RuntimeWakeup.cfg \
  verification/graph_core/model/RuntimeWakeup.tla
```

The initial sandbox launch parsed the model but could not bind TLC's local RMI
socket. The checked run uses the same command outside the sandbox; its output
is retained at `/tmp/kaos-p3-session/wakeup-tlc.log`.

On 9 October 2026, TLC 2.19 completed this configuration with no errors:
116548 generated states, 25281 distinct states, depth 28, four invariants and
two temporal properties (12 instantiated temporal branches), in 106 seconds.
The model SHA-256 was
`bd3c401c83b56db707f7503e435e4b5ccd643342838260409ee8738bf461b5dc`;
the configuration SHA-256 was
`432b6bf319e9e67c1261db86d25c55312babcdf99864177bf357f3cd14dac2ea`.
