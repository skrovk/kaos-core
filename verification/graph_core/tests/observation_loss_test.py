"""OBS03: corrupt observation copies of a positively successful host run.

This does not inject live network loss or infer execution/closure from silence.
Collector snapshots below are a separate adapter validation case.
"""

import copy
import hashlib
import json
import sys
import tempfile
from pathlib import Path

import cbor2

from sim_env.graph_collector import GraphCollector
from sim_env.graph_evidence import AuthorizationError, storage_ack
from sim_env.graph_protocol import SCHEMA_VERSION, Node
from sim_env.graph_workload_timing import evaluate_timing
from sim_env.graph_workloads import evaluate, run_workload
from sim_env.orchestrator import Orchestrator


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def collector_truncation():
    """Every proper prefix fails; a complete positive control stores exactly."""
    authority = Orchestrator([10])
    request = authority.add_node(
        Node(1, 10, b"module", 8, 0, 4096, b"", 10000000, 1000000)
    )
    authority.dispatch(request.operation)
    collector = GraphCollector(None, authority=90, capacity=1)
    collector.register(request)
    payload = cbor2.dumps(
        [SCHEMA_VERSION, 8, request.operation.value, 10, 1, 1, 39, [1]],
        canonical=True,
    )
    for length in range(len(payload)):
        try:
            collector.store(payload[:length], peer=10)
        except AuthorizationError:
            pass
        else:
            raise AssertionError(f"truncated snapshot accepted at length {length}")
        check(collector.retrieve(request.operation) == (), "partial evidence retained")
    expected_ack = storage_ack(request.operation, 10, 1)
    check(collector.store(payload, peer=10) == expected_ack, "complete snapshot lacks ACK")
    check(collector.retrieve(request.operation) == (payload,), "exact snapshot not retained")
    # A later malformed observation cannot erase the already retained history.
    try:
        collector.store(payload[:-1], peer=10)
    except AuthorizationError:
        pass
    else:
        raise AssertionError("truncated duplicate accepted")
    check(collector.retrieve(request.operation) == (payload,), "valid history changed")
    return {"proper_prefixes_rejected": len(payload), "complete_snapshot_stored": True}


def main(host, guest, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    run_output = output / "healthy"
    result = run_workload(
        Path(host), Path(guest), run_output, workload="feedback", seed=17, duration_s=0.25
    )
    check(result["status"] == "passed", "positive sample control did not pass")
    check(not result["remaining_instances_at_teardown"], "healthy run left instances")
    check(all(item["report"][5] == 1 for item in result["outcomes"]), "operation failed")
    original_bytes = (run_output / "events.jsonl").read_bytes()
    events = [json.loads(line) for line in original_bytes.splitlines()]
    windows = result["windows"]
    devices = (1, 2)
    timing = evaluate_timing(events, devices, "feedback", windows, budget_us=20000)
    # A timing budget may fail under host load; completeness must still hold.
    check(timing["trace_complete"] is True, "positive timing trace is incomplete")
    check(timing["status"] in ("passed", "failed"), "positive timing is inconclusive")
    assessments = []
    for device in devices:
        channel = [i for i, event in enumerate(events) if event["kind"] == "host_stdout"
                   and event["device"] == device and event["line"].startswith("CHANNEL ")]
        summaries = [i for i, event in enumerate(events) if event["kind"] == "host_stdout"
                     and event["device"] == device and event["line"].startswith("CHANNEL_END ")]
        check(len(channel) >= 3 and len(summaries) == 1, "missing positive host observations")
        for fault in ("first_record", "middle_record", "last_record", "all_records",
                      "terminal_summary", "reported_drop", "duplicate_record", "reordered_records"):
            damaged = copy.deepcopy(events)
            if fault in ("first_record", "middle_record", "last_record"):
                index = {"first_record": channel[0], "middle_record": channel[len(channel) // 2],
                         "last_record": channel[-1]}[fault]
                del damaged[index]
            elif fault == "all_records":
                indices = set(channel)
                damaged = [event for i, event in enumerate(damaged) if i not in indices]
            elif fault == "terminal_summary":
                del damaged[summaries[0]]
            elif fault == "reported_drop":
                summary = damaged[summaries[0]]
                total = summary["line"].split()[1]
                summary["line"] = f"CHANNEL_END {total} 1"
            elif fault == "duplicate_record":
                damaged.insert(channel[0], copy.deepcopy(damaged[channel[0]]))
            else:
                first, second = channel[:2]
                damaged[first], damaged[second] = damaged[second], damaged[first]
            samples = evaluate(damaged, devices, "feedback", windows)
            budget = evaluate_timing(damaged, devices, "feedback", windows, budget_us=20000)
            check(samples["status"] == "inconclusive", f"{device}/{fault}: sample verdict")
            check(budget["status"] == "inconclusive" and budget["trace_complete"] is False,
                  f"{device}/{fault}: timing verdict")
            assessments.append({"device": device, "fault": fault,
                                "samples": samples, "timing": budget})
    check((run_output / "events.jsonl").read_bytes() == original_bytes, "original trace modified")
    check(evaluate(events, devices, "feedback", windows)["status"] == "passed",
          "original evidence no longer passes")
    report = {"scope": "post-capture observation corruption; not live fault injection",
              "seed": 17, "healthy_trace_sha256": hashlib.sha256(original_bytes).hexdigest(),
              "healthy_sample_status": result["status"], "healthy_timing_status": timing["status"],
              "graph_outcomes": result["outcomes"], "observation_faults": assessments,
              "collector": collector_truncation(),
              "limitations": ["No kernel loss accounting", "No collector crash or relay overflow",
                              "No delayed injector qualification", "No full execution replay"]}
    (output / "assessment.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"output": str(output),
                      "observation_faults_inconclusive": len(assessments),
                      "collector": report["collector"], "healthy_sample_status": "passed"}))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: observation_loss_test.py HOST FEEDBACK_GUEST OUTPUT")
    main(*sys.argv[1:])
