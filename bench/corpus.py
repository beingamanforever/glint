"""Build a benchmark corpus: copy source files into OUT, drop duplicates, commit it.

  python3 bench/corpus.py OUT SRC [SRC ...] [--max-mb 2000]
  e.g. python3 bench/corpus.py ~/glint-corpus ~/.cache/uv/archive-v0/*
"""
import argparse, hashlib, os, subprocess
from pathlib import Path

EXT = {".py", ".pyi", ".c", ".h", ".cc", ".cpp", ".hpp", ".cu", ".cuh", ".java", ".kt", ".scala", ".go", ".rs",
       ".js", ".ts", ".tsx", ".sql", ".sh", ".md", ".rst", ".toml", ".yaml", ".yml"}


def copy(out, sources, limit):
    seen, files, size = set(), 0, 0
    for src in sources:
        for dirpath, dirs, names in os.walk(src):
            dirs[:] = sorted(d for d in dirs if not d.startswith("."))
            for name in sorted(names):
                path = Path(dirpath, name)
                dst = out / path.relative_to(src)
                if name.startswith(".") or path.suffix not in EXT or dst.exists():
                    continue
                try:
                    data = path.read_bytes()
                except OSError:
                    continue
                digest = hashlib.blake2b(data, digest_size=16).digest()
                if not data or len(data) > 1 << 20 or b"\0" in data or digest in seen:
                    continue
                seen.add(digest)
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(data)
                files, size = files + 1, size + len(data)
                if size >= limit:
                    return files, size
    return files, size


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("src", nargs="+")
    ap.add_argument("--max-mb", type=int, default=2000)
    a = ap.parse_args()
    out = Path(a.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    files, size = copy(out, [Path(s).expanduser() for s in a.src], a.max_mb << 20)
    git = ["git", "-C", str(out), "-c", "user.name=glint", "-c", "user.email=glint@localhost"]
    subprocess.run(git + ["init", "-q"], check=True)
    subprocess.run(git + ["add", "-A"], check=True)
    subprocess.run(git + ["commit", "-qm", "corpus"], check=True)
    print(f"{files} files, {size / 1e6:.0f} MB in {out}")


if __name__ == "__main__":
    main()
