#!/usr/bin/env python3
import json
import os
import re
import signal
import subprocess
import threading
import time
import uuid
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


WEBUI_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = WEBUI_DIR.parent
BUILD_DIR = WEBUI_DIR / "build"
RUN_DIR = WEBUI_DIR / "runs"
SIM_BINARY = PROJECT_ROOT / "bin" / "trace_pv"
RUN_DIR.mkdir(exist_ok=True)

DEFAULT_SIMULATION_INPUT = {
    "topology": "3l2s",
    "inputMode": "mission",
    "missionCsv": "",
    "environmentalCsv": "simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv",
    "operatingCsv": "simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv",
    "rounds": 1,
    # Run the one-year electrical profile once, then let the simulator reuse
    # those cached electrical results for lifetime degradation iterations until
    # the first component reaches its failure threshold.
    "maxIterations": 0,
    "postProcessingMode": "reference",
    "thermalStep": 0,
    "modulation": "svm",
    "ngpus": "all",
    "model": "simulator_inputs/simulation_model/example_simulation_model.json",
}

JOBS = {}
LOCK = threading.Lock()


def _float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _last_float(pattern, text):
    matches = re.findall(pattern, text, flags=re.MULTILINE)
    if not matches:
        return None
    match = matches[-1]
    if isinstance(match, tuple):
        match = match[0]
    return _float(match)


def _last_range(pattern, text):
    matches = re.findall(pattern, text, flags=re.MULTILINE)
    if not matches:
        return None
    low, high = matches[-1]
    low_value = _float(low)
    high_value = _float(high)
    if low_value is None or high_value is None:
        return None
    return [low_value, high_value]


DEGRADATION_PATTERNS = {
    "fan_electrical_external": r"Electrical \(External\):\s*([-+0-9.eE]+)",
    "fan_electrical_internal": r"Electrical \(Internal\):\s*([-+0-9.eE]+)",
    "fan_mechanical_external": r"Mechanical \(External\):\s*([-+0-9.eE]+)",
    "fan_mechanical_internal": r"Mechanical \(Internal\):\s*([-+0-9.eE]+)",
    "capacitor": r"^\s*Capacitor:\s*([-+0-9.eE]+)",
    "igbt_delta_t": r"^\s*IGBT \(DeltaT model\):\s*([-+0-9.eE]+)",
    "igbt_arrhenius": r"^\s*IGBT \(Arrhenius model\):\s*([-+0-9.eE]+)",
    "pcb": r"^\s*PCB:\s*([-+0-9.eE]+)",
}


def parse_degradation_block(block):
    values = {}
    for key, pattern in DEGRADATION_PATTERNS.items():
        values[key] = _last_float(pattern, block)
    return values


def _zero_degradation():
    return {key: 0.0 for key in DEGRADATION_PATTERNS}


def _numeric_degradation(values):
    return {key: float(values.get(key) or 0.0) for key in DEGRADATION_PATTERNS}


def _add_degradation(left, right):
    return {key: float(left.get(key) or 0.0) + float(right.get(key) or 0.0) for key in DEGRADATION_PATTERNS}


def _time_years(iteration, round_index=None, round_total=None):
    iteration = max(int(iteration or 1), 1)
    if round_index is not None and round_total:
        return (iteration - 1) + (int(round_index) / float(round_total))
    return float(iteration)


def parse_degradation_history(text):
    history = []
    marker = "Cumulative Degradation Progress (This Iteration):"
    for match in re.finditer(re.escape(marker), text):
        start = match.end()
        next_candidates = [
            pos for pos in (
                text.find("Accumulated Degradation Progress (Round", start),
                text.find("Cumulative Degradation Progress (This Iteration):", start),
                text.find("Global Cumulative Degradation Progress (All Iterations):", start),
                text.find("Mission Profile Iteration #", start),
            )
            if pos != -1
        ]
        end = min(next_candidates) if next_candidates else len(text)
        block = text[start:end]
        before = text[:match.start()]
        iteration_matches = re.findall(r"Mission Profile Iteration #([0-9]+)", before)
        round_matches = re.findall(r"Round ([0-9]+)/([0-9]+) finished", before)
        iteration = int(iteration_matches[-1]) if iteration_matches else len(history) + 1
        round_index = int(round_matches[-1][0]) if round_matches else len(history) + 1
        round_total = int(round_matches[-1][1]) if round_matches else None
        history.append({
            "iteration": iteration,
            "round": round_index,
            "round_total": round_total,
            "iteration_degradation": _numeric_degradation(parse_degradation_block(block)),
        })
    if history:
        completed_before_iteration = _zero_degradation()
        current_iteration = None
        latest_iteration_degradation = _zero_degradation()
        for point in history:
            if current_iteration is None:
                current_iteration = point["iteration"]
            elif point["iteration"] != current_iteration:
                completed_before_iteration = _add_degradation(completed_before_iteration, latest_iteration_degradation)
                current_iteration = point["iteration"]
                latest_iteration_degradation = _zero_degradation()

            latest_iteration_degradation = point["iteration_degradation"]
            point["degradation"] = _add_degradation(completed_before_iteration, point["iteration_degradation"])
            point["time_years"] = _time_years(point["iteration"], point["round"], point["round_total"])
            point["label"] = f"{point['time_years']:.2f} yr"
        return history

    marker = "Global Cumulative Degradation Progress (All Iterations):"
    for match in re.finditer(re.escape(marker), text):
        start = match.end()
        next_marker = text.find(marker, start)
        end = next_marker if next_marker != -1 else len(text)
        block = text[start:end]
        before = text[:match.start()]
        iteration_matches = re.findall(r"Mission Profile Iteration #([0-9]+)", before)
        iteration = int(iteration_matches[-1]) if iteration_matches else len(history) + 1
        history.append({
            "iteration": iteration,
            "round": None,
            "round_total": None,
            "label": f"{iteration} yr",
            "time_years": _time_years(iteration),
            "degradation": _numeric_degradation(parse_degradation_block(block)),
        })
    return history


def parse_global_degradation_history(text):
    history = []
    marker = "Global Cumulative Degradation Progress (All Iterations):"
    for match in re.finditer(re.escape(marker), text):
        start = match.end()
        next_candidates = [
            pos for pos in (
                text.find(marker, start),
                text.find("Mission Profile Iteration #", start),
                text.find("DEBUG: Checking degradation thresholds:", start),
                text.find("FAILURE DETECTED:", start),
                text.find("Simulation Complete", start),
            )
            if pos != -1
        ]
        end = min(next_candidates) if next_candidates else len(text)
        block = text[start:end]
        before = text[:match.start()]
        iteration_matches = re.findall(r"Mission Profile Iteration #([0-9]+)", before)
        round_matches = re.findall(r"Round ([0-9]+)/([0-9]+) finished", before)
        iteration = int(iteration_matches[-1]) if iteration_matches else len(history) + 1
        round_index = int(round_matches[-1][0]) if round_matches else None
        round_total = int(round_matches[-1][1]) if round_matches else None
        history.append({
            "iteration": iteration,
            "round": round_index,
            "round_total": round_total,
            "label": f"{iteration} yr",
            "time_years": _time_years(iteration, round_index, round_total),
            "degradation": _numeric_degradation(parse_degradation_block(block)),
        })
    return history


def parse_round_complete_snapshots(text):
    """Parse atomic, accepted round snapshots emitted by the simulator."""
    history = []
    last_end = None
    required = {
        "iteration", "round", "pass", "thermal_action",
        *DEGRADATION_PATTERNS.keys(),
    }
    for match in re.finditer(r"^TRACEPV_ROUND_COMPLETE\s+(.+)$", text, flags=re.MULTILINE):
        fields = {}
        for token in match.group(1).split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        if not required.issubset(fields):
            continue
        try:
            round_index_text, round_total_text = fields["round"].split("/", 1)
            iteration = int(fields["iteration"])
            round_index = int(round_index_text)
            round_total = int(round_total_text)
            round_pass = int(fields["pass"])
            degradation = {
                key: float(fields[key]) for key in DEGRADATION_PATTERNS
            }
        except (TypeError, ValueError):
            continue
        time_years = _time_years(iteration, round_index, round_total)
        history.append({
            "iteration": iteration,
            "round": round_index,
            "round_total": round_total,
            "round_pass": round_pass,
            "thermal_action": fields["thermal_action"],
            "label": f"{time_years:.2f} yr",
            "time_years": time_years,
            "degradation": degradation,
        })
        last_end = match.end()
    return history, last_end


def parse_execution_progress(text):
    """Parse mission-case execution progress independently of degradation."""
    marker_pattern = (
        r"TRACEPV_PROGRESS\s+iteration=([0-9]+)\s+"
        r"round=([0-9]+)/([0-9]+)\s+pass=([0-9]+)\s+"
        r"processed=([0-9]+)\s+total=([0-9]+)"
    )
    markers = re.findall(marker_pattern, text)
    if markers:
        iteration, round_index, round_total, round_pass, processed, total = (
            int(value) for value in markers[-1]
        )
        processed = min(max(processed, 0), max(total, 0))
        round_fraction = (processed / total) if total > 0 else 0.0
        iteration_fraction = ((round_index - 1) + round_fraction) / max(round_total, 1)
        return {
            "execution_progress_percent": round(min(max(iteration_fraction, 0.0), 1.0) * 100.0, 2),
            "current_case": processed,
            "total_cases": total,
            "round_pass": round_pass,
        }

    # Compatibility for logs created before batch markers were added.
    total_cases = _int(_last_float(r"Total Cases per Iteration:\s*([0-9]+)", text))
    if total_cases is None:
        total_cases = _int(_last_float(r"Mission Profile Loaded:\s*([0-9]+) cases", text))
    completed = "Simulation Complete" in text
    if completed and total_cases:
        return {
            "execution_progress_percent": 100.0,
            "current_case": total_cases,
            "total_cases": total_cases,
            "round_pass": None,
        }

    return {
        "execution_progress_percent": 0.0,
        "current_case": 0,
        "total_cases": total_cases,
        "round_pass": None,
    }


def parse_thermal_progress(text):
    """Report initial thermal solves, cached reuse, and critical reruns."""
    marker_pattern = (
        r"TRACEPV_THERMAL\s+action=(initial|updated|rerun|reuse)\s+"
        r"iteration=([0-9]+)\s+round=([0-9]+)/([0-9]+)\s+"
        r"pass=([0-9]+)\s+critical_step_percent=([0-9]+)"
    )
    markers = re.findall(marker_pattern, text)
    if markers:
        action, iteration, round_index, round_total, round_pass, critical_step = markers[-1]
        return {
            "thermal_action": action,
            "thermal_runs": sum(action_name in {"initial", "updated", "rerun"} for action_name, *_ in markers),
            "thermal_reruns": sum(action_name == "rerun" for action_name, *_ in markers),
            "thermal_cache_reuses": sum(action_name == "reuse" for action_name, *_ in markers),
            "thermal_iteration": int(iteration),
            "thermal_round": int(round_index),
            "thermal_round_total": int(round_total),
            "thermal_pass": int(round_pass),
            "critical_degradation_step_percent": int(critical_step),
            "critical_degradation_events": len(re.findall(r"TRACEPV_CRITICAL_DEGRADATION", text)),
        }

    # Compatibility for completed logs produced before explicit thermal markers.
    reruns = len(re.findall(r"is recomputing from the degradation bucket crossed", text))
    thermal_outputs = len(re.findall(r"(?:Capacitor thermal|Thermal validation) data saved to:", text))
    return {
        "thermal_action": "rerun" if reruns else ("initial" if thermal_outputs else None),
        "thermal_runs": thermal_outputs,
        "thermal_reruns": reruns,
        "thermal_cache_reuses": len(re.findall(r"Reusing cached loss/thermal/reliability results", text)),
        "thermal_iteration": None,
        "thermal_round": None,
        "thermal_round_total": None,
        "thermal_pass": None,
        "critical_degradation_step_percent": 10,
        "critical_degradation_events": len(re.findall(r"Degradation bucket crossed in round", text)),
    }


def parse_log(text, max_iterations):
    round_snapshots, committed_end = parse_round_complete_snapshots(text)
    committed_text = text[:committed_end] if committed_end is not None else text
    degradation_history = round_snapshots or parse_degradation_history(text)
    global_degradation_history = [] if round_snapshots else parse_global_degradation_history(text)
    if global_degradation_history:
        existing = {
            (point.get("iteration"), point.get("round"), point.get("time_years"))
            for point in degradation_history
        }
        for point in global_degradation_history:
            key = (point.get("iteration"), point.get("round"), point.get("time_years"))
            if key not in existing:
                degradation_history.append(point)
        degradation_history.sort(key=lambda point: (
            float(point.get("time_years") or 0.0),
            int(point.get("iteration") or 0),
            int(point.get("round") or 0),
        ))

    if degradation_history:
        latest_degradation = degradation_history[-1]["degradation"]
    else:
        latest_degradation = {
        key: _last_float(pattern, text) for key, pattern in DEGRADATION_PATTERNS.items()
        }
    iteration_headers = list(re.finditer(
        r"^Mission Profile Iteration #([0-9]+)\s*$", text, flags=re.MULTILINE
    ))
    current_iteration = int(iteration_headers[-1].group(1)) if iteration_headers else 0
    current_iteration_text = text[iteration_headers[-1].end():] if iteration_headers else text
    completed_iteration_matches = re.findall(r"Mission Profile Iteration #([0-9]+) Complete", text)
    completed_iterations = int(completed_iteration_matches[-1]) if completed_iteration_matches else 0
    # Round counters are scoped to the active iteration. Without this scope,
    # the last completed round from year N was incorrectly shown as completed
    # while year N+1 had only just started.
    started_round_matches = re.findall(r"Starting Round ([0-9]+)/([0-9]+)", current_iteration_text)
    finished_round_matches = re.findall(r"Round ([0-9]+)/([0-9]+) finished", current_iteration_text)
    current_round = 0
    round_total = None
    if started_round_matches:
        current_round = int(started_round_matches[-1][0])
        round_total = int(started_round_matches[-1][1])
    elif finished_round_matches:
        current_round = int(finished_round_matches[-1][0])
        round_total = int(finished_round_matches[-1][1])
    current_iteration_snapshots = [
        point for point in round_snapshots if point["iteration"] == current_iteration
    ]
    if current_iteration_snapshots:
        completed_round = int(current_iteration_snapshots[-1]["round"])
        round_total = int(current_iteration_snapshots[-1]["round_total"])
    else:
        completed_round = int(finished_round_matches[-1][0]) if finished_round_matches else 0
    failed = re.findall(r"Failed Component:\s*(.+)", text)
    failed_component = failed[-1].strip() if failed else None
    has_failure = bool(failed_component and not failed_component.lower().startswith("none"))

    if (current_iteration > 0 or degradation_history) and (
        not degradation_history or float(degradation_history[0].get("time_years") or 0.0) > 0.0
    ):
        degradation_history.insert(0, {
            "iteration": 0,
            "round": 0,
            "round_total": None,
            "label": "0 yr",
            "time_years": 0.0,
            "degradation": _zero_degradation(),
        })

    execution = parse_execution_progress(text)
    thermal = parse_thermal_progress(committed_text)
    result = {
        "internal_temp_range": _last_range(r"Internal temp range:\s*\[([-+0-9.eE]+),\s*([-+0-9.eE]+)\]\s*C", committed_text),
        "internal_rh_range": _last_range(r"Internal RH range:\s*\[([-+0-9.eE]+),\s*([-+0-9.eE]+)\]\s*%", committed_text),
        "capacitor_hotspot_range": None,
        "capacitor_surface_range": None,
        "igbt_junction_range": None,
        "average_inverter_loss": _last_float(r"Avg inverter loss:\s*([-+0-9.eE]+)\s*W", committed_text),
        "failed_component": failed_component,
        "first_failed_component": failed_component if has_failure else None,
        "iterations": _int(_last_float(r"Total Mission Profile Iterations:\s*([0-9]+)", text)),
        "current_iteration": current_iteration,
        "completed_iterations": completed_iterations,
        "simulated_years": completed_iterations,
        "current_year": current_iteration,
        "current_round": current_round,
        "completed_round": completed_round,
        "round_total": round_total,
        "duration_seconds": _last_float(r"Execution Time \(Wall Clock\):\s*([-+0-9.eE]+)\s*s", text),
        "degradation": latest_degradation,
        "degradation_history": degradation_history,
        "lifetime_iterations": None,
        "lifetime_label": None,
        **execution,
        **thermal,
    }

    hotspot_values = [_float(v) for v in re.findall(r"Hotspot temp:\s*([-+0-9.eE]+)\s*C", committed_text)]
    hotspot_values = [v for v in hotspot_values if v is not None]
    if hotspot_values:
        result["capacitor_hotspot_range"] = [min(hotspot_values), max(hotspot_values)]

    surface_values = [_float(v) for v in re.findall(r"Surface temp:\s*([-+0-9.eE]+)\s*C", committed_text)]
    surface_values = [v for v in surface_values if v is not None]
    if surface_values:
        result["capacitor_surface_range"] = [min(surface_values), max(surface_values)]

    junction_values = re.findall(r"Junction temp(?: range)?:\s*(?:\[)?([-+0-9.eE]+)(?:,\s*([-+0-9.eE]+)\])?\s*C", committed_text)
    flattened = []
    for item in junction_values:
        if isinstance(item, tuple):
            flattened.extend(_float(v) for v in item if v)
        else:
            flattened.append(item)
    flattened = [v for v in flattened if v is not None]
    if flattened:
        result["igbt_junction_range"] = [min(flattened), max(flattened)]

    if has_failure and result["iterations"]:
        result["lifetime_iterations"] = result["iterations"]
        result["lifetime_label"] = f"{result['iterations']} mission profile iterations"

    if result["iterations"] is not None:
        result["completed_iterations"] = result["iterations"]
        result["simulated_years"] = result["iterations"]
        result["current_iteration"] = result["iterations"]
        result["current_year"] = result["iterations"]
        if result["round_total"] is not None:
            result["current_round"] = result["round_total"]
            result["completed_round"] = result["round_total"]

    iteration = current_iteration or result["iterations"] or 0
    progress = result["execution_progress_percent"]

    return result, iteration, progress


def build_args(payload):
    payload = {**DEFAULT_SIMULATION_INPUT, **(payload or {})}

    topology = payload["topology"]
    if topology not in {"2l2s", "2l1s", "3l2s", "3l1s"}:
        raise ValueError("Invalid topology")

    input_mode = payload["inputMode"]
    if input_mode not in {"static", "mission"}:
        raise ValueError("Invalid input mode")

    args = [str(SIM_BINARY), "--topology", topology, "--input-mode", input_mode]
    # Dashboard temperature/loss cards still read detailed diagnostics from the
    # run log; opt in explicitly now that standalone CLI output is concise.
    args.append("--verbose")
    if input_mode == "static":
        args.extend([
            "--static-temp", str(payload.get("staticTemp", 25)),
            "--static-rh", str(payload.get("staticRh", 85)),
            "--static-voltage", str(payload.get("staticVoltage", 500)),
            "--static-power", str(payload.get("staticPower", 300)),
            "--static-irradiance", str(payload.get("staticIrradiance", 1000)),
            "--static-cases", str(payload.get("staticCases", 105120)),
        ])
    else:
        mission_csv = str(payload.get("missionCsv", "")).strip()
        if mission_csv:
            args.extend(["--mission-csv", mission_csv])
        else:
            args.extend([
                "--environmental-csv", str(payload["environmentalCsv"]),
                "--operating-csv", str(payload["operatingCsv"]),
            ])

    rounds = int(payload["rounds"])
    max_iterations = int(payload["maxIterations"])
    if rounds <= 0:
        raise ValueError("Rounds must be greater than 0")
    if max_iterations < 0:
        raise ValueError("Max iterations must be greater than or equal to 0")

    post_processing_mode = str(payload.get("postProcessingMode", "reference"))
    if post_processing_mode not in {"fast", "reference", "hybrid"}:
        raise ValueError("Post-processing mode must be fast, reference, or hybrid")

    args.extend(["--rounds", str(rounds), "--max-iterations", str(max_iterations)])
    args.extend(["--post-processing-mode", post_processing_mode])
    args.extend(["--thermal-step", str(payload.get("thermalStep", 0))])
    args.extend(["--modulation", str(payload.get("modulation", "svm"))])
    args.extend(["--ngpus", str(payload.get("ngpus", "all"))])
    args.extend(["--model", str(payload.get("model", "simulator_inputs/simulation_model/example_simulation_model.json"))])
    return args, max_iterations


def job_snapshot(job_id):
    with LOCK:
        job = JOBS.get(job_id)
        if not job:
            return None
        process = job.get("process")
        if process and job.get("status") == "running":
            exit_code = process.poll()
            if exit_code is not None:
                job["status"] = "completed" if exit_code == 0 else "failed"
                job["exit_code"] = exit_code
                job["ended_at"] = job.get("ended_at") or time.time()
        log_path = job["log_path"]
        text = log_path.read_text(errors="replace") if log_path.exists() else ""
        result, iteration, progress = parse_log(text, job["max_iterations"])
        status = job.get("status")
        if status == "completed":
            progress = 100.0
            result["execution_progress_percent"] = 100.0
            result["execution_stage"] = "Completed"
        elif status == "running" and progress >= 100.0:
            progress = 99.5
            result["execution_progress_percent"] = progress
            result["execution_stage"] = "Finalizing current round"
        elif status == "running":
            if result.get("thermal_action") == "rerun":
                result["execution_stage"] = "Re-running thermal simulation after critical degradation"
            elif result.get("thermal_action") == "reuse":
                result["execution_stage"] = "Lifetime cycle using cached electrical and thermal results"
            elif result.get("thermal_action") == "updated":
                result["execution_stage"] = "Processing next batch with updated thermal parameters"
            elif result.get("thermal_action") == "initial":
                result["execution_stage"] = "One-year electrical and thermal simulation"
            else:
                result["execution_stage"] = "Preparing electrical and thermal simulation"
        elif status == "queued":
            result["execution_stage"] = "Queued"
        else:
            result["execution_stage"] = str(status or "idle").capitalize()
        started_at = job.get("started_at")
        ended_at = job.get("ended_at")
        if started_at:
            result["duration_seconds"] = max(0.0, (ended_at or time.time()) - started_at)
        job.update({"result": result, "iteration": iteration, "progress": progress})
        return {k: v for k, v in job.items() if k not in {"process", "log_path"}}


def run_job(job_id, args):
    process = None
    with LOCK:
        job = JOBS[job_id]
        log_path = job["log_path"]
        job["status"] = "running"
        job["started_at"] = time.time()

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = "/usr/local/cuda/lib64:" + env.get("LD_LIBRARY_PATH", "")
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write("$ " + " ".join(args) + "\n\n")
        log.flush()
        try:
            process = subprocess.Popen(
                args,
                cwd=str(PROJECT_ROOT),
                stdout=log,
                stderr=subprocess.STDOUT,
                env=env,
                start_new_session=True,
            )
            with LOCK:
                JOBS[job_id]["process"] = process
            exit_code = process.wait()
            with LOCK:
                job = JOBS[job_id]
                if job["status"] != "stopped":
                    job["status"] = "completed" if exit_code == 0 else "failed"
                job["exit_code"] = exit_code
                job["ended_at"] = time.time()
        except Exception as exc:
            log.write(f"\nSERVER ERROR: {exc}\n")
            log.flush()
            if process and process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                except Exception:
                    process.kill()
            with LOCK:
                JOBS[job_id]["status"] = "failed"
                JOBS[job_id]["exit_code"] = -1
                JOBS[job_id]["ended_at"] = time.time()


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(BUILD_DIR), **kwargs)

    def _send_json(self, data, status=HTTPStatus.OK):
        encoded = json.dumps(data, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def _read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return {}
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def do_OPTIONS(self):
        self.send_response(HTTPStatus.NO_CONTENT)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/api/simulations":
            if not SIM_BINARY.exists():
                self._send_json({"error": f"Simulator binary not found: {SIM_BINARY}"}, HTTPStatus.BAD_REQUEST)
                return
            try:
                payload = self._read_json()
                args, max_iterations = build_args(payload)
                job_id = uuid.uuid4().hex[:12]
                log_path = RUN_DIR / f"{job_id}.log"
                with LOCK:
                    JOBS[job_id] = {
                        "id": job_id,
                        "status": "queued",
                        "progress": 0,
                        "iteration": 0,
                        "result": {},
                        "exit_code": None,
                        "created_at": time.time(),
                        "started_at": None,
                        "ended_at": None,
                        "args": args,
                        "toolbox_inputs": payload.get("toolboxInputs", {}),
                        "max_iterations": max_iterations,
                        "log_path": log_path,
                        "process": None,
                    }
                thread = threading.Thread(target=run_job, args=(job_id, args), daemon=True)
                thread.start()
                self._send_json(job_snapshot(job_id), HTTPStatus.CREATED)
            except Exception as exc:
                self._send_json({"error": str(exc)}, HTTPStatus.BAD_REQUEST)
            return

        match = re.match(r"^/api/simulations/([^/]+)/stop$", path)
        if match:
            job_id = match.group(1)
            with LOCK:
                job = JOBS.get(job_id)
                if not job:
                    self._send_json({"error": "Job not found"}, HTTPStatus.NOT_FOUND)
                    return
                process = job.get("process")
                job["status"] = "stopped"
                job["ended_at"] = time.time()
            if process and process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                except Exception:
                    process.kill()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
                with LOCK:
                    job = JOBS.get(job_id)
                    if job:
                        job["exit_code"] = process.poll()
                        job["ended_at"] = time.time()
            self._send_json(job_snapshot(job_id))
            return

        self._send_json({"error": "Not found"}, HTTPStatus.NOT_FOUND)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/health":
            self._send_json({
                "ok": True,
                "project_root": str(PROJECT_ROOT),
                "simulator": str(SIM_BINARY),
                "simulator_exists": SIM_BINARY.exists(),
            })
            return

        if path == "/api/defaults":
            self._send_json(DEFAULT_SIMULATION_INPUT)
            return

        match = re.match(r"^/api/simulations/([^/]+)$", path)
        if match:
            snapshot = job_snapshot(match.group(1))
            if not snapshot:
                self._send_json({"error": "Job not found"}, HTTPStatus.NOT_FOUND)
                return
            self._send_json(snapshot)
            return

        match = re.match(r"^/api/simulations/([^/]+)/log$", path)
        if match:
            with LOCK:
                job = JOBS.get(match.group(1))
                if not job:
                    self.send_error(HTTPStatus.NOT_FOUND, "Job not found")
                    return
                log_path = job["log_path"]
            text = log_path.read_text(errors="replace") if log_path.exists() else ""
            encoded = text[-250000:].encode("utf-8", errors="replace")
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)
            return

        if BUILD_DIR.exists():
            requested = BUILD_DIR / path.lstrip("/")
            if path != "/" and requested.exists():
                return super().do_GET()
            self.path = "/index.html"
            return super().do_GET()

        self._send_json({"error": "Build directory not found. Run npm run build in webui."}, HTTPStatus.NOT_FOUND)


def main():
    host = os.environ.get("TRACEPV_WEBUI_HOST", "127.0.0.1")
    port = int(os.environ.get("TRACEPV_WEBUI_PORT", "8080"))
    server = ThreadingHTTPServer((host, port), Handler)
    print(f"TRACE-PV WebUI serving on http://{host}:{port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
