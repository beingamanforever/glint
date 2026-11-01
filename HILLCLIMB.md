# Hill-climbing glint

One round is one idea, measured before and after, kept only if it wins.
Paste this into a fresh session at the repo root:

> Read HILLCLIMB.md and run one round on glint.
> Profile first, after a warm-up run, then change one thing, and keep it only if the bench says so.
> Keep the code concise: the fewest clear lines, no new abstractions, no comments that restate the code.
> Prefer deleting code to adding it. Report the profile, the hypothesis, the numbers before and after, and the line count.

## Rules

- Output stays identical to ripgrep: every row of the bench says `same`.
- One dependency (PCRE2), no daemon. Reach for the standard library or PCRE2 before writing your own.
- `src/glint.cpp` stays under 450 lines, in its style: `using namespace std`, `all(x)`, `pb`, Allman braces, few comments.
- Lines are a cost. A change that adds more than 20 lines must be at least 10% faster; one that deletes lines at equal speed is a win.
- No tricks for one query. A change has to help a class of queries.
- Same machine, same corpus, nothing else running.

## Setup

```bash
make
python3 bench/corpus.py ~/glint-corpus ~/.cache/uv/archive-v0/*      # build once, then reuse
python3 bench/bench.py ~/glint-corpus --runs 5 --skip rg --skip "grep -r" --skip "git grep" --skip "glint --no-index" --json base.json
```

## Loop

1. **Profile, warm.** Run each query once to warm the page cache and discard it, then profile the median of the next 5 runs.
   `./glint --stats PATTERN ~/glint-corpus` splits a query into `plan_ms` (stamps and lookups) and verify (`total_ms - plan_ms`).
   Compare `candidates` with `matched_files`: a large gap means the index lets too much through.
   Most queries finish in 20-500 ms, too fast for `sample`. Time phases in a scratch copy, or loop one query under `sample` (macOS) or `perf record -g` (Linux).
2. **Pick the biggest measured cost.** Write down the number it should move.
3. **Microbenchmark the idea** in a scratch file before touching glint. Most ideas die here, cheaply.
4. **Change glint.** `make` must stay free of warnings.
5. **Check, then bench.** Correctness against ripgrep once, then timing without it, since ripgrep's full scans disturb the cache between rows:
   ```bash
   python3 bench/bench.py ~/glint-corpus --runs 1 --skip "grep -r" --skip "git grep"
   python3 bench/bench.py ~/glint-corpus --runs 5 --skip rg --skip "grep -r" --skip "git grep" --skip "glint --no-index" --json new.json --compare base.json
   ```
6. **Keep it only if** every row is `same` (`capped` for `-m 20`), the `glint` row's geomean is at most 0.95 of the baseline (or equal with fewer lines), and index time and size grow less than 10%.
   A query more than 1.10 slower is rerun interleaved (base, new, base, new); reject only if the slowdown reproduces. Otherwise revert.
7. **Commit** one change with its number, e.g. `perf: skip stat for unchanged dirs (0.91x)`. Add a row to the log, and `new.json` becomes `base.json`.

## Where the time goes

Apple M5, 98,023 files, 1.25 GB, measured warm on 2026-11-01. This Mac runs three endpoint-security agents, which is why `open()` is slow; expect different numbers elsewhere.

- Process start: about 11 ms for any process; glint adds about 4 ms at start and exit.
- Freshness: one `stat` per file, about 55 ms per query. It stops scaling past about 4 threads. `--no-fresh` skips it.
- Loading the index (`files.bin` and the maps): about 4.4 ms. Trigram lookups: 0.2-1.9 ms.
- Verify: `open()` costs about 53 µs single-threaded, or 15-17 µs per candidate file at 10 threads; `read` adds about 3%. A full scan alone takes about 1.7 s.

## Tried

| Idea | Measured | Verdict |
|---|---|---|
| `getattrlistbulk` instead of `stat` | 283 ms vs 52 ms | rejected |
| `fstatat` on a directory fd | 88 ms vs 54 ms at 10 threads | rejected |
| `string_view` paths instead of `string` (time to split `files.bin`) | 9.7 ms with views vs 8.7 ms with strings | no gain |
| `mmap` instead of `read` for candidates | no consistent gain | rejected |
| More threads than cores for I/O | noisy, no consistent gain | rejected |
| tgrep next-character masks | helps 2 of 10 queries; about 2.8x index size | not yet |
| Sparse n-grams (Cursor) | posting-list work is a small share today | not yet |
| Hand out files to threads in chunks | `stat` 57 ms and open+read 1.5-1.7 s at any chunk size | rejected |
| Overlap the `stat` pass with verify | 4-11 ms faster below 2,500 candidates, 46 ms slower at 24,500 | rejected |
| `stat` on 4 threads (the performance cores) | 46 ms vs 57 ms alone, no gain inside glint | rejected |
| Paths as pointers into mapped `files.bin` | bench 0.924, `--no-fresh` 0.858, but `return None` 1.10x; interleaved A/B: 3-4 ms off each `--no-fresh` query, about 1.5 ms off `glint` | rejected by the 1.10 rule |

Ideas that die in a microbenchmark go in this table too, with what was timed.

## Open ideas

- Line-level co-occurrence for `literal.*literal` patterns: `raise ValueError.*dtype` reads 7,836 files to find 266.
- Masks or sparse n-grams once phrase queries show candidates far above matches.
- A cheaper signal than one `stat` per file that still catches edits, without a daemon.
- Start the largest candidate files first to cut tail latency.

## Log

| Date | Model | Change | Geomean vs baseline | Commit |
|---|---|---|---|---|
| 2026-11-01 | Claude Opus 5.5 (max) | first test round: zero-copy paths, reverted (3-4 ms, within noise) | 1.00 | a16ffcf |
| 2026-11-01 | Claude Opus 5.5 | baseline: 13.7x faster than ripgrep (26.1x with `--no-fresh`), 98,023 files | 1.00 | 3e06c25 |
