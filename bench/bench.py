"""Benchmark greps on one corpus: speed, correctness against ripgrep, output size.

  python3 bench/bench.py CORPUS [--runs 5] [--tool 'name=cmd {flags} {p} .'] [--skip NAME]
                                [--json out.json] [--compare base.json]
"""
import argparse, json, math, re, shlex, shutil, statistics, subprocess, time
from pathlib import Path

GLINT = str(Path(__file__).resolve().parent.parent / "glint")
TOOLS = {
    "rg": ["rg", "-n", "--no-heading", "--with-filename", "--color=never", "{flags}", "-e", "{p}", "."],
    "glint": [GLINT, "{flags}", "{p}", "."],
    "glint --no-fresh": [GLINT, "--no-fresh", "{flags}", "{p}", "."],
    "glint --no-index": [GLINT, "--no-index", "{flags}", "{p}", "."],
    "glint -m 20": [GLINT, "-m", "20", "{flags}", "{p}", "."],
    "git grep": ["git", "grep", "-n", "-I", "-E", "{flags}", "-e", "{p}"],
    "grep -r": ["grep", "-rnI", "-E", "--exclude-dir=.git", "{flags}", "-e", "{p}", "."],
    "ugrep": ["ugrep", "-rnI", "{flags}", "-e", "{p}", "."],
}
try:
    import tiktoken
    count_tokens = lambda s: len(tiktoken.get_encoding("o200k_base").encode(s, disallowed_special=()))
except ImportError:
    count_tokens = lambda s: len(s.encode()) // 4


def timed(argv, cwd):
    t = time.perf_counter()
    out = subprocess.run(argv, cwd=cwd, capture_output=True).stdout.decode(errors="replace")
    return time.perf_counter() - t, out


def matches(out):
    return {(m[1].removeprefix("./"), int(m[2])) for m in re.finditer(r"^\s*(.+?):(\d+)(?=[:\s])", out, re.M)}


def geomean(xs):
    return math.exp(statistics.mean(math.log(x) for x in xs))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("corpus")
    ap.add_argument("--queries", default=str(Path(__file__).with_name("queries.txt")))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--tool", action="append", default=[])
    ap.add_argument("--skip", action="append", default=[])
    ap.add_argument("--json")
    ap.add_argument("--compare")
    a = ap.parse_args()
    tools = {k: v for k, v in TOOLS.items() if shutil.which(v[0]) and k not in a.skip}
    tools.update({s.split("=", 1)[0]: shlex.split(s.split("=", 1)[1]) for s in a.tool})
    queries = [q.strip() for q in Path(a.queries).read_text().splitlines() if q.strip() and not q.startswith("#")]

    floor = statistics.median(timed(["true"], a.corpus)[0] for _ in range(a.runs)) * 1000
    print(f"process startup: {floor:.1f} ms")
    if "rg" in tools:
        print(f"rg --version: {statistics.median(timed(['rg', '--version'], a.corpus)[0] for _ in range(a.runs)) * 1000:.1f} ms")
    index = {}
    if any(v[0] == GLINT for v in tools.values()):
        seconds, out = timed([GLINT, "index", "."], a.corpus)
        index = {"wall_ms": seconds * 1000, "report": out.strip()}
        print(f"glint index: {out.strip()}")

    results = []
    for q in queries:
        flags, p = (["-i"], q[3:]) if q.startswith("-i ") else ([], q)
        print(f"\n`{q}`\n| tool | median ms | p95 ms | lines | vs first | tokens |\n|---|--:|--:|--:|---|--:|")
        oracle = None
        for name, template in tools.items():
            argv = [x for t in template for x in (flags if t == "{flags}" else [p if t == "{p}" else t])]
            timed(argv, a.corpus)
            runs = [timed(argv, a.corpus) for _ in range(a.runs)]
            times, out = sorted(r[0] * 1000 for r in runs), runs[-1][1]
            got = matches(out)
            oracle = got if oracle is None else oracle
            verdict = ("same" if got == oracle else f"capped {len(got)}/{len(oracle)}" if got < oracle and "-m" in argv
                       else f"{len(oracle - got)} missing, {len(got - oracle)} extra")
            row = {"query": q, "tool": name, "median_ms": statistics.median(times), "p95_ms": times[math.ceil(0.95 * len(times)) - 1],
                   "lines": len(got), "verdict": verdict, "tokens": count_tokens(out)}
            results.append(row)
            print(f"| {name} | {row['median_ms']:.1f} | {row['p95_ms']:.1f} | {len(got)} | {verdict} | {row['tokens']:,} |")

    print("\n| tool | geomean ms | x first | tokens |\n|---|--:|--:|--:|")
    by = lambda rows, tool: {r["query"]: r["median_ms"] for r in rows if r["tool"] == tool}
    first = by(results, next(iter(tools)))
    for name in tools:
        mine = by(results, name)
        print(f"| {name} | {geomean(mine.values()):.1f} | {geomean(mine[q] / first[q] for q in mine):.2f} | "
              f"{sum(r['tokens'] for r in results if r['tool'] == name):,} |")
    if a.compare:
        base = json.loads(Path(a.compare).read_text())["results"]
        print("\nvs baseline (below 1 is faster):")
        for name in tools:
            new, old = by(results, name), by(base, name)
            if shared := [q for q in new if q in old]:
                print(f"  {name}: {geomean(new[q] / old[q] for q in shared):.3f}")
    if a.json:
        Path(a.json).write_text(json.dumps({"floor_ms": floor, "index": index, "results": results}, indent=1))


if __name__ == "__main__":
    main()
