# Hill-climbing glint

Give a new model this prompt: "Read HILLCLIMB.md and make glint faster without breaking it."
One round is one idea, measured before and after, kept only if it wins.

## Rules

- Output stays identical to ripgrep: every row of the bench says `same`.
- One dependency (PCRE2), no daemon. Reach for the standard library or PCRE2 before writing your own.
- `src/glint.cpp` stays under 450 lines, in its style: `using namespace std`, `all(x)`, `pb`, Allman braces, few comments.
- No tricks for one query. A change has to help a class of queries.
- Same machine, same corpus, nothing else running.

## Setup

```bash
make
python3 bench/corpus.py ~/glint-corpus ~/.cache/uv/archive-v0/*      # build once, then reuse
python3 bench/bench.py ~/glint-corpus --runs 5 --skip "grep -r" --json base.json
```

## Loop

1. **Profile.** `./glint --stats PATTERN ~/glint-corpus` splits a query into `plan_ms` (stamps and lookups) and verify (`total_ms - plan_ms`).
   Compare `candidates` with `matched_files`: a large gap means the index lets too much through.
   For hot functions, use `sample glint 5` or Instruments on macOS, and `perf record -g` on Linux.
2. **Pick the biggest measured cost.** Write down the number it should move.
3. **Microbenchmark the idea** in a scratch file before touching glint. Most ideas die here, cheaply.
4. **Change glint.** `make` must stay free of warnings.
5. **Bench against the baseline:**
   ```bash
   python3 bench/bench.py ~/glint-corpus --runs 5 --skip "grep -r" --json new.json --compare base.json
   ```
6. **Keep it only if** every row is `same`, the glint geomean is at most 0.95 of the baseline, no query is more than 1.10 slower, and index time and size grow less than 10%. Otherwise revert.
7. **Commit** one change with its number, e.g. `perf: skip stat for unchanged dirs (0.91x)`. Add a row to the log, and `new.json` becomes `base.json`.

## Where the time goes

Apple M5, 10 cores, 98,023 files, 1.25 GB:

- About 52 ms per query: one `stat` per file to catch edits. `--no-fresh` skips it.
- About 9 ms: reading `files.bin` into memory.
- Verify: about 31 µs per candidate file, mostly `open` and `read`. A full scan takes about 3 s, which is the I/O floor.

## Tried

| Idea | Measured | Verdict |
|---|---|---|
| `getattrlistbulk` instead of `stat` | 283 ms vs 52 ms | rejected |
| `fstatat` on a directory fd | 88 ms vs 54 ms at 10 threads | rejected |
| `string_view` paths instead of `string` | 9.7 ms vs 8.7 ms | no gain |
| `mmap` instead of `read` for candidates | no consistent gain | rejected |
| More threads than cores for I/O | noisy, no consistent gain | rejected |
| tgrep next-character masks | helps 2 of 10 queries; about 2.8x index size | not yet |
| Sparse n-grams (Cursor) | posting-list work is a small share today | not yet |

## Open ideas

- Line-level co-occurrence for `literal.*literal` patterns: `raise ValueError.*dtype` reads 7,836 files to find 266.
- Masks or sparse n-grams once phrase queries show candidates far above matches.
- A cheaper signal than one `stat` per file that still catches edits, without a daemon.
- Start the largest candidate files first to cut tail latency.

## Log

| Date | Model | Change | Geomean vs baseline | Commit |
|---|---|---|---|---|
| 2026-11-01 | Claude Opus 5.5 | baseline: 13.7x faster than ripgrep (26.1x with `--no-fresh`), 98,023 files | 1.00 | 3e06c25 |
