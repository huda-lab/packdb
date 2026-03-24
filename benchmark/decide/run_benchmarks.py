#!/usr/bin/env python3
"""PackDB DECIDE performance benchmark runner.

Runs a set of DECIDE queries at multiple scales, measuring wall-clock time
and peak memory (RSS) via /usr/bin/time -v. Outputs results to both terminal
(formatted table) and JSON (for regression tracking).

Usage:
    python3 benchmark/decide/run_benchmarks.py                  # run all
    python3 benchmark/decide/run_benchmarks.py --queries Q1,Q3  # subset
    python3 benchmark/decide/run_benchmarks.py --iterations 10  # more runs
    python3 benchmark/decide/run_benchmarks.py --compare prev.json  # delta

When PACKDB_BENCH=1 is set, also parses per-stage timers from packdb stderr
(requires Phase B C++ instrumentation).
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
QUERIES_DIR = SCRIPT_DIR / "queries"
RESULTS_DIR = SCRIPT_DIR / "results"

PACKDB_EXE = REPO_ROOT / "build" / "release" / "packdb"
PACKDB_DB = REPO_ROOT / "packdb.db"

# Default scale points per query
DEFAULT_SCALES: dict[str, list[int]] = {
    "Q1": [10, 100, 500],
    "Q2": [10, 50, 200],
    "Q3": [10, 100, 500],
    "Q4": [10, 50, 200],
    "Q5": [500, 2000, 10000],
}

# Query template files
QUERY_TEMPLATES: dict[str, str] = {
    "Q1": "q1_knapsack_baseline.sql.template",
    "Q2": "q2_abs_minmax.sql.template",
    "Q3": "q3_count_avg_when.sql.template",
    "Q4": "q4_nested_per.sql.template",
    "Q5": "q5_stress.sql.template",
}

QUERY_DESCRIPTIONS: dict[str, str] = {
    "Q1": "knapsack_baseline",
    "Q2": "abs_minmax",
    "Q3": "count_avg_when",
    "Q4": "nested_per",
    "Q5": "stress",
}

DEFAULT_ITERATIONS = 5

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def get_git_commit() -> str:
    """Return short git commit hash of HEAD."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, cwd=REPO_ROOT,
        )
        return result.stdout.strip() if result.returncode == 0 else "unknown"
    except FileNotFoundError:
        return "unknown"


def get_system_info() -> dict:
    """Collect basic system info for reproducibility."""
    info: dict = {
        "platform": platform.platform(),
        "cpu": platform.processor() or "unknown",
        "python": platform.python_version(),
    }
    # Try to get total RAM on Linux
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    kb = int(line.split()[1])
                    info["ram_gb"] = round(kb / 1024 / 1024, 1)
                    break
    except (OSError, ValueError):
        pass
    return info


def ensure_database() -> None:
    """Ensure packdb.db exists with TPC-H data, generating if needed."""
    # Check if DB exists and has tables (read-only to avoid locks)
    if PACKDB_DB.exists():
        result = subprocess.run(
            [str(PACKDB_EXE), str(PACKDB_DB), "-readonly", "-c",
             "SELECT COUNT(*) FROM lineitem;"],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode == 0 and "Error" not in result.stderr:
            return
        # DB exists but no lineitem table — needs TPC-H data

    print("Setting up TPC-H data (sf=0.01)...")
    setup_sql = "LOAD tpch; CALL dbgen(sf=0.01);"
    result = subprocess.run(
        [str(PACKDB_EXE), str(PACKDB_DB), "-c", setup_sql],
        capture_output=True, text=True, timeout=120,
    )
    if result.returncode != 0:
        print(f"ERROR: Failed to generate TPC-H data:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    print("TPC-H data loaded successfully.")


def load_template(query_name: str) -> str:
    """Load a query template file."""
    template_file = QUERIES_DIR / QUERY_TEMPLATES[query_name]
    return template_file.read_text()


def render_query(template: str, scale: int) -> str:
    """Replace {SCALE} placeholder in template."""
    return template.replace("{SCALE}", str(scale))


def parse_time_output(stderr: str) -> dict:
    """Parse /usr/bin/time -v output from stderr.

    Returns dict with wall_time_s and peak_rss_kb.
    """
    result: dict = {}

    # Wall clock time - format: h:mm:ss or m:ss.ss
    m = re.search(
        r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*"
        r"(?:(\d+):)?(\d+):(\d+(?:\.\d+)?)",
        stderr,
    )
    if m:
        hours = int(m.group(1) or 0)
        minutes = int(m.group(2))
        seconds = float(m.group(3))
        result["wall_time_s"] = round(hours * 3600 + minutes * 60 + seconds, 4)

    # Peak RSS
    m = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", stderr)
    if m:
        result["peak_rss_kb"] = int(m.group(1))

    return result


def parse_bench_output(stderr: str) -> dict:
    """Parse PACKDB_BENCH stage timer lines from stderr.

    Lines look like: PACKDB_BENCH: key=value
    """
    stages: dict = {}
    for m in re.finditer(r"PACKDB_BENCH:\s*(\w+)=([\d.]+)", stderr):
        key = m.group(1)
        val = m.group(2)
        # Convert to float if it looks numeric
        try:
            stages[key] = float(val) if "." in val else int(val)
        except ValueError:
            stages[key] = val
    return stages


def run_single(query_sql: str, timeout: int = 600) -> dict:
    """Run a single benchmark query, returning timing and memory metrics."""
    # Write query to a temp file (avoids shell escaping issues with stdin)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".sql", delete=False) as f:
        f.write(query_sql)
        sql_path = f.name

    try:
        env = os.environ.copy()
        cmd = [
            "/usr/bin/time", "-v",
            str(PACKDB_EXE), str(PACKDB_DB), "-readonly",
        ]

        with open(sql_path) as sql_file:
            result = subprocess.run(
                cmd,
                stdin=sql_file,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env,
            )

        metrics = {"exit_code": result.returncode}

        # Parse /usr/bin/time output (goes to stderr)
        time_metrics = parse_time_output(result.stderr)
        metrics.update(time_metrics)

        # Parse PACKDB_BENCH stage timers if present
        bench_metrics = parse_bench_output(result.stderr)
        if bench_metrics:
            metrics["stages"] = bench_metrics

        # Capture any errors
        if result.returncode != 0:
            # Filter out /usr/bin/time lines to get actual error
            error_lines = [
                line for line in result.stderr.splitlines()
                if not line.strip().startswith(("Command", "Elapsed", "Maximum",
                    "Average", "Major", "Minor", "Voluntary", "Involuntary",
                    "Swaps", "File", "Socket", "Signals", "Page", "Exit",
                    "Percent", "System", "User"))
                and line.strip()
            ]
            if error_lines:
                metrics["error"] = "\n".join(error_lines[:5])

        return metrics

    except subprocess.TimeoutExpired:
        return {"exit_code": -1, "error": f"Timeout after {timeout}s"}
    finally:
        os.unlink(sql_path)


def compute_stats(runs: list[dict]) -> dict:
    """Compute median and stddev from a list of run results."""
    successful = [r for r in runs if r.get("exit_code") == 0]
    stats: dict = {"total_runs": len(runs), "successful_runs": len(successful)}

    if not successful:
        return stats

    wall_times = [r["wall_time_s"] for r in successful if "wall_time_s" in r]
    if wall_times:
        stats["median_wall_time_s"] = round(statistics.median(wall_times), 4)
        if len(wall_times) > 1:
            stats["stddev_wall_time_s"] = round(statistics.stdev(wall_times), 4)
        else:
            stats["stddev_wall_time_s"] = 0.0

    rss_values = [r["peak_rss_kb"] for r in successful if "peak_rss_kb" in r]
    if rss_values:
        stats["median_peak_rss_kb"] = int(statistics.median(rss_values))

    # Aggregate stage timers if present (median of each stage)
    stage_keys: set[str] = set()
    for r in successful:
        if "stages" in r:
            stage_keys.update(r["stages"].keys())
    if stage_keys:
        stats["stages"] = {}
        for key in sorted(stage_keys):
            values = [r["stages"][key] for r in successful
                      if "stages" in r and key in r["stages"]]
            if values:
                if isinstance(values[0], float):
                    stats["stages"][key] = round(statistics.median(values), 2)
                else:
                    stats["stages"][key] = int(statistics.median(values))

    return stats


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def print_table(results: list[dict], commit: str) -> None:
    """Print a formatted results table to terminal."""
    print()
    print(f"PackDB Benchmark Results (commit: {commit})")
    print("=" * 78)

    header = f"{'Query':<25} {'Scale':>6} {'Median(s)':>10} {'StdDev':>8} {'PeakRSS(MB)':>12}"
    print(header)
    print("-" * 78)

    for entry in results:
        name = f"{entry['query']}_{QUERY_DESCRIPTIONS.get(entry['query'], '')}"
        scale = entry["scale"]
        stats = entry.get("stats", {})

        median = stats.get("median_wall_time_s")
        stddev = stats.get("stddev_wall_time_s")
        rss_kb = stats.get("median_peak_rss_kb")

        median_str = f"{median:.4f}" if median is not None else "FAIL"
        stddev_str = f"{stddev:.4f}" if stddev is not None else "-"
        rss_str = f"{rss_kb / 1024:.1f}" if rss_kb is not None else "-"

        print(f"{name:<25} {scale:>6} {median_str:>10} {stddev_str:>8} {rss_str:>12}")

        # Print stage breakdown if available
        stages = stats.get("stages")
        if stages:
            stage_parts = [f"{k}={v}" for k, v in stages.items()]
            print(f"{'':>25} {'':>6}   stages: {', '.join(stage_parts)}")

    print("=" * 78)
    print()


def print_comparison(current: list[dict], previous: dict) -> None:
    """Print a comparison table between current and previous results."""
    prev_lookup: dict[tuple[str, int], dict] = {}
    for entry in previous.get("queries", []):
        key = (entry["query"], entry["scale"])
        prev_lookup[key] = entry.get("stats", {})

    print()
    print(f"Comparison: {previous.get('commit', '?')} -> current")
    print("=" * 90)
    header = (f"{'Query':<25} {'Scale':>6} {'Prev(s)':>9} {'Curr(s)':>9} "
              f"{'Delta':>8} {'%':>7} {'RSS Delta':>10}")
    print(header)
    print("-" * 90)

    for entry in current:
        name = f"{entry['query']}_{QUERY_DESCRIPTIONS.get(entry['query'], '')}"
        scale = entry["scale"]
        curr_stats = entry.get("stats", {})
        prev_stats = prev_lookup.get((entry["query"], scale), {})

        curr_t = curr_stats.get("median_wall_time_s")
        prev_t = prev_stats.get("median_wall_time_s")
        curr_rss = curr_stats.get("median_peak_rss_kb")
        prev_rss = prev_stats.get("median_peak_rss_kb")

        if curr_t is not None and prev_t is not None:
            delta = curr_t - prev_t
            pct = (delta / prev_t * 100) if prev_t > 0 else 0
            delta_str = f"{delta:+.4f}"
            pct_str = f"{pct:+.1f}%"
        else:
            delta_str = "-"
            pct_str = "-"

        prev_str = f"{prev_t:.4f}" if prev_t is not None else "-"
        curr_str = f"{curr_t:.4f}" if curr_t is not None else "FAIL"

        rss_delta_str = "-"
        if curr_rss is not None and prev_rss is not None:
            rss_delta_mb = (curr_rss - prev_rss) / 1024
            rss_delta_str = f"{rss_delta_mb:+.1f}MB"

        print(f"{name:<25} {scale:>6} {prev_str:>9} {curr_str:>9} "
              f"{delta_str:>8} {pct_str:>7} {rss_delta_str:>10}")

    print("=" * 90)
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(description="PackDB DECIDE benchmark runner")
    parser.add_argument(
        "--queries", type=str, default=None,
        help="Comma-separated list of queries to run (e.g., Q1,Q3). Default: all",
    )
    parser.add_argument(
        "--scales", type=str, default=None,
        help="Comma-separated list of scales to override defaults (e.g., 50,500)",
    )
    parser.add_argument(
        "--iterations", type=int, default=DEFAULT_ITERATIONS,
        help=f"Number of iterations per query/scale (default: {DEFAULT_ITERATIONS})",
    )
    parser.add_argument(
        "--output", type=str, default=None,
        help="Custom JSON output path (default: results/<commit>_<timestamp>.json)",
    )
    parser.add_argument(
        "--compare", type=str, default=None,
        help="Path to previous results JSON for comparison",
    )
    parser.add_argument(
        "--timeout", type=int, default=600,
        help="Timeout per query execution in seconds (default: 600)",
    )
    args = parser.parse_args()

    # Validate prerequisites
    if not PACKDB_EXE.exists():
        print(f"ERROR: packdb executable not found at {PACKDB_EXE}", file=sys.stderr)
        print("Run 'make release' first.", file=sys.stderr)
        sys.exit(1)

    ensure_database()

    # Determine which queries to run
    if args.queries:
        query_names = [q.strip().upper() for q in args.queries.split(",")]
        for q in query_names:
            if q not in QUERY_TEMPLATES:
                print(f"ERROR: Unknown query '{q}'. Available: {', '.join(sorted(QUERY_TEMPLATES))}",
                      file=sys.stderr)
                sys.exit(1)
    else:
        query_names = sorted(QUERY_TEMPLATES.keys())

    # Determine scale overrides
    scale_override = None
    if args.scales:
        scale_override = [int(s.strip()) for s in args.scales.split(",")]

    commit = get_git_commit()
    system_info = get_system_info()

    print(f"PackDB DECIDE Benchmark (commit: {commit})")
    print(f"Queries: {', '.join(query_names)}")
    print(f"Iterations: {args.iterations}")
    if os.environ.get("PACKDB_BENCH"):
        print("Stage timers: ENABLED (PACKDB_BENCH=1)")
    print()

    # Run benchmarks
    all_results: list[dict] = []
    total_combos = sum(
        len(scale_override or DEFAULT_SCALES[q]) for q in query_names
    )
    combo_idx = 0

    for query_name in query_names:
        template = load_template(query_name)
        scales = scale_override or DEFAULT_SCALES[query_name]

        for scale in scales:
            combo_idx += 1
            desc = QUERY_DESCRIPTIONS.get(query_name, query_name)
            print(f"[{combo_idx}/{total_combos}] {query_name}_{desc} (scale={scale})", end="", flush=True)

            sql = render_query(template, scale)
            runs: list[dict] = []

            for i in range(args.iterations):
                metrics = run_single(sql, timeout=args.timeout)
                runs.append(metrics)

                # Progress indicator
                if metrics.get("exit_code") == 0:
                    print(".", end="", flush=True)
                else:
                    print("X", end="", flush=True)

            stats = compute_stats(runs)

            entry = {
                "query": query_name,
                "scale": scale,
                "runs": runs,
                "stats": stats,
            }
            all_results.append(entry)

            # Print inline summary
            median = stats.get("median_wall_time_s")
            if median is not None:
                print(f" {median:.4f}s")
            else:
                print(" FAILED")

    # Build output document
    output = {
        "commit": commit,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "system": system_info,
        "iterations": args.iterations,
        "queries": all_results,
    }

    # Print results table
    print_table(all_results, commit)

    # Print comparison if requested
    if args.compare:
        try:
            with open(args.compare) as f:
                prev = json.load(f)
            print_comparison(all_results, prev)
        except (OSError, json.JSONDecodeError) as e:
            print(f"WARNING: Could not load comparison file: {e}", file=sys.stderr)

    # Save JSON
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    if args.output:
        output_path = Path(args.output)
    else:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = RESULTS_DIR / f"{commit}_{ts}.json"

    with open(output_path, "w") as f:
        json.dump(output, f, indent=2)
    print(f"Results saved to: {output_path}")


if __name__ == "__main__":
    main()
