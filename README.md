<p align="center"><img src="assets/glint.jpg" alt="glint" width="100%"></p>

# glint
glint is a small, daemon-free indexed grep for coding agents. It narrows a regex search with a trigram index, stays correct while you edit, returns results sized for a model’s context window, and can tag each hit with labels you supply

## Build

```bash
brew install pcre2    # or: apt install libpcre2-dev
make
```

## Use

```bash
glint index                                  # index the files git tracks
glint 'def [a-z_]+_cache'                    # path:line:text, like ripgrep
glint -i -m 20 httpconnection                # at most 20 lines, then a count
glint -l --labels nodes.tsv whatif_stats     # tag hits from path<TAB>label lines
glint --no-fresh --stats ThreadPoolExecutor  # skip the freshness check, print timings
```

## How it works

1. `git ls-files` lists the files; every 3-byte slice is indexed, lowercased.
2. The regex is reduced to the literals every match must contain, and their posting lists are intersected.
3. Files whose size or mtime changed since indexing are searched too.
4. PCRE2 verifies the candidates in parallel. The index never decides a match.

Ideas from Russ Cox's codesearch, Cursor's Instant Grep, Microsoft's tgrep, CMU's BigGrep, ripgrep and codedb.

## Benchmark

98,023 files, 1.25 GB of Python, C, C++ and CUDA source, on an Apple M5.
Wall time including process startup, median of 3 runs after a warm-up; every tool's output is identical to ripgrep's.

| Query | ripgrep 14.1.1 | tgrep 1.2.6 | glint | glint --no-fresh |
|---|--:|--:|--:|--:|
| `ThreadPoolExecutor` | 4.32 s | 222 ms | 127 ms | 34 ms |
| `__global__ void` | 2.24 s | 94 ms | 83 ms | 31 ms |
| `def [a-z_]+_cache` | 2.26 s | 2.14 s | 256 ms | 195 ms |
| `return None` | 2.63 s | 2.71 s | 563 ms | 527 ms |
| `[0-9]{4}-[0-9]{2}-[0-9]{2}` (no literal) | 2.15 s | 37.8 s | 1.89 s | 1.92 s |
| Geomean of 10 queries | 1x | 2.9x | 11.1x | 20.2x |

| | Index build | Index size |
|---|--:|--:|
| glint | 4 s | 151 MB |
| tgrep | 11 s | 970 MB |
| codedb 0.2.5862 | 221 s | 1.4 GB |

- tgrep's warm server (1.6 GB resident) reaches 32x, but takes 39.7 s on the regex with no literal.
- codedb returned nothing at this size: one search ran past 60 s, and its server reached 3.8 GB without answering. On 1,852 files it returns its top 50 ranked lines.
- Not timed: BigGrep (n-gram index for binary files, needs Boost, returns file names), igrep (an interactive UI over ripgrep's library) and Cursor's Instant Grep (only inside Cursor).
- ripgrep is the copy bundled with Claude Code; its times vary run to run (2.0-4.3 s here).

Across the 10 queries, `glint -m 20` returns 4,342 tokens, against 5.3 million from ripgrep.

```bash
python3 bench/corpus.py ~/glint-corpus ~/.cache/uv/archive-v0/*
python3 bench/bench.py ~/glint-corpus --skip "grep -r" --tool 'tgrep=tgrep -n --no-heading --with-filename {flags} -e {p} .'
```

To make glint faster, follow [HILLCLIMB.md](HILLCLIMB.md).

## Limits

- Untracked files are not searched. Files added to git are, at one `git ls-files` per query until you run `glint index` again.
- Each query checks every indexed file's size and mtime: about 50 ms at 100k files. `--no-fresh` skips it for copies that are only updated by re-indexing.
- Back-references and `\x` escapes make glint scan every file.
