<p align="center"><img src="assets/glint.png" alt="glint" width="100%"></p>

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
Wall time including process startup, median of 3 runs after a warm-up.
Every result is identical to ripgrep 14.1.1 (the copy bundled with Claude Code).

| Query | ripgrep | glint | glint --no-fresh |
|---|--:|--:|--:|
| `ThreadPoolExecutor` | 2.28 s | 85 ms | 34 ms |
| `__global__ void` | 2.95 s | 87 ms | 24 ms |
| `def [a-z_]+_cache` | 3.57 s | 290 ms | 221 ms |
| `return None` | 3.58 s | 570 ms | 463 ms |
| `[0-9]{4}-[0-9]{2}-[0-9]{2}` (no literal) | 3.48 s | 3.17 s | 3.04 s |
| Geomean of 10 queries | 1x | 13.7x faster | 26.1x faster |

The index builds in 3.7 s and takes 151 MB.
Across the 10 queries, `glint -m 20` returns 4,342 tokens, against 5.3 million from ripgrep.

```bash
python3 bench/corpus.py ~/glint-corpus ~/.cache/uv/archive-v0/*
python3 bench/bench.py ~/glint-corpus --skip "grep -r"
```

To make glint faster, follow [HILLCLIMB.md](HILLCLIMB.md).

## Limits

- Untracked files are not searched. Files added to git are, at one `git ls-files` per query until you run `glint index` again.
- Each query checks every indexed file's size and mtime: about 50 ms at 100k files. `--no-fresh` skips it for copies that are only updated by re-indexing.
- Back-references and `\x` escapes make glint scan every file.
