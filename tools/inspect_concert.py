"""Inventaria plists e referências de um pacote MainStage local."""
import argparse
import json
from pathlib import Path
import plistlib
import zlib

LIMIT = 16 * 1024 * 1024
EXTENSIONS = {".exs", ".caf", ".wav", ".aif", ".aiff", ".cst", ".patch"}


def read_plist(path):
    with path.open("rb") as source:
        data = source.read(LIMIT + 1)
    if len(data) > LIMIT:
        raise ValueError("plist excede o limite")
    if path.suffix.lower() == ".plistz":
        codec = zlib.decompressobj()
        data = codec.decompress(data, LIMIT + 1)
        if len(data) > LIMIT or not codec.eof or codec.unused_data:
            raise ValueError("plistZ truncado ou inválido")
    return plistlib.loads(data)


def references(root):
    pending = [root]
    visited = set()
    result = set()
    while pending:
        node = pending.pop()
        if isinstance(node, str) and Path(node).suffix.lower() in EXTENSIONS:
            result.add(node)
        if isinstance(node, (dict, list)) and id(node) not in visited:
            visited.add(id(node))
            pending.extend(node if isinstance(node, list) else [*node, *node.values()])
    return sorted(result)


def inspect(path):
    if path.is_dir():
        if path.suffix.lower() != ".concert":
            raise ValueError("selecione uma pasta .concert")
        files = sorted(f for f in path.rglob("*")
                       if f.is_file() and f.suffix.lower() in {".plist", ".plistz"})
        base = path
    else:
        files = [path]
        base = path.parent
    if not files:
        raise ValueError("nenhum plist encontrado")
    report = []
    for file in files:
        entry = {"file": file.relative_to(base).as_posix()}
        try:
            document = read_plist(file)
            entry["keys"] = sorted(document) if isinstance(document, dict) else []
            entry["references"] = references(document)
        except (OSError, ValueError, OverflowError, zlib.error) as error:
            entry["error"] = str(error)
        report.append(entry)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        report = inspect(args.path)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2, ensure_ascii=True))
    raise SystemExit(any("error" in item for item in report))
