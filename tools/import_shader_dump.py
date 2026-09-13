"""Merge SDK guest shader listings and microcode without overwriting conflicts.

Game-derived inputs and outputs must stay local and outside the source tree.
Translated D3D12 blobs are deliberately excluded. No game process is launched.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile


SHADER_NAME = re.compile(r"shader_([0-9A-Fa-f]{16})\.ucode(\.bin)?\.(vert|frag)\Z")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").digest()


def merge_dump(source, destination, *, dry_run=False):
    """Preflight every collision, then atomically publish only absent files.

    An interrupted import may leave a subset of new files; rerunning completes it.
    Each published file is complete, and an existing file is never replaced.
    """
    source, destination = Path(source).resolve(), Path(destination).resolve()
    if not source.is_dir():
        raise ValueError(f"Source is not a directory: {source}")
    if destination.exists() and not destination.is_dir():
        raise ValueError(f"Destination is not a directory: {destination}")
    pending, names = [], set()
    report = {"source": str(source), "destination": str(destination),
              "dry_run": dry_run, "identical_files": 0, "ignored_files": 0,
              "added_listings": 0, "added_binaries": 0, "new_stages": []}
    for path in sorted(source.iterdir()):
        match = SHADER_NAME.fullmatch(path.name)
        if not match or not path.is_file() or path.is_symlink():
            report["ignored_files"] += 1
            continue
        shader_hash, binary, stage = match.groups()
        name = f"shader_{shader_hash.upper()}.ucode{binary or ''}.{stage}"
        if name in names:
            raise ValueError(f"Duplicate normalized shader filename: {name}")
        names.add(name)
        target = destination / name
        expected = digest(path)
        if target.exists() or target.is_symlink():
            if target.is_symlink() or not target.is_file() or digest(target) != expected:
                raise ValueError(f"Shader collision; destination left unchanged: {target}")
            report["identical_files"] += 1
        else:
            pending.append((path, target, expected))
            report["added_binaries" if binary else "added_listings"] += 1
            if not binary:
                report["new_stages"].append(f"{'vs' if stage == 'vert' else 'ps'}_{shader_hash.upper()}")
    if not names:
        raise ValueError(f"No SDK guest shader files found in {source}")
    if dry_run:
        return report
    destination.mkdir(parents=True, exist_ok=True)
    for path, target, expected in pending:
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=destination, prefix=".shader-import-",
                                             delete=False) as stream:
                temporary = Path(stream.name)
                with path.open("rb") as original:
                    while chunk := original.read(1024 * 1024):
                        stream.write(chunk)
                stream.flush()
                os.fsync(stream.fileno())
            if digest(temporary) != expected:
                raise ValueError(f"Source changed during import: {path}")
            # Same-directory hard link publishes a complete file without replacing
            # a destination created after preflight (os.replace would overwrite it).
            try:
                os.link(temporary, target)
            except FileExistsError:
                if target.is_symlink() or not target.is_file() or digest(target) != expected:
                    raise ValueError(f"Destination changed during import: {target}")
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--report", type=Path, help="Optional local JSON report")
    args = parser.parse_args()
    try:
        result = merge_dump(args.source, args.destination, dry_run=args.dry_run)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Import failed: {error}\n")
    output = json.dumps(result, indent=2) + "\n"
    if args.report:
        args.report.write_text(output, encoding="utf-8")
    print(output, end="")


if __name__ == "__main__":
    main()
