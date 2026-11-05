#!/usr/bin/env python3
"""Read an Ableton Live 11 .als or collected-project ZIP and create PlayStage maps.

This converter does not execute Live devices or third-party installers. By default
it exports only small JSON sample maps; the audio stays with its owner. Supply
--extract-samples for a ZIP when the imported audio is not already on disk.
"""

from __future__ import annotations

import argparse
import gzip
import json
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath
from xml.etree import ElementTree as ET


ASTORIA_NAME = re.compile(
    r"^ASTORIA GRAND XL-([A-G](?:#|b)?)(-?\d+)-V(\d+)-[^/]+\.aiff?$",
    re.IGNORECASE,
)
PITCH_CLASSES = {"C": 0, "C#": 1, "DB": 1, "D": 2, "D#": 3,
                 "EB": 3, "E": 4, "F": 5, "F#": 6, "GB": 6,
                 "G": 7, "G#": 8, "AB": 8, "A": 9, "A#": 10,
                 "BB": 10, "B": 11}
SAMPLE_SUFFIXES = {".aif", ".aiff", ".wav", ".flac"}


def xml_value(node, path, fallback=None):
    item = node.find(path)
    return fallback if item is None else item.get("Value", fallback)


def int_value(node, path, fallback):
    return int(xml_value(node, path, fallback))


def safe_sample_path(value):
    """Reject absolute/traversal paths from imported project metadata."""
    p = PurePosixPath(value.replace("\\", "/"))
    if p.is_absolute() or ".." in p.parts or not p.parts or ":" in p.parts[0]:
        raise ValueError("Caminho de sample inseguro no arquivo .als")
    return p.as_posix()


def entry_for_part(part, *, drum_note=None):
    relative = xml_value(part, ".//SampleRef/FileRef/RelativePath")
    if not relative:
        return None
    result = {
        "file": safe_sample_path(relative),
        "root": int_value(part, "RootKey", 60),
        "lo": int_value(part, "KeyRange/Min", 0),
        "hi": int_value(part, "KeyRange/Max", 127),
        "velLo": int_value(part, "VelocityRange/Min", 1),
        "velHi": int_value(part, "VelocityRange/Max", 127),
    }
    if drum_note is not None:
        # Live Drum Rack maps ReceivingNote to SendingNote (usually C3/MIDI 60).
        # The native player does not remap incoming notes, so make one key zone
        # with its real receiving note as normal pitch.
        result.update(root=drum_note, lo=drum_note, hi=drum_note)
    return result


def map_obj(name, samples, source):
    return {
        "schema": "playstage.multisample/1",
        "name": name,
        "source": source,
        "samples": samples,
    }


def parse_project(xml_bytes):
    root = ET.fromstring(xml_bytes)
    creator = root.get("Creator", "Ableton Live")
    if "Ableton Live 11" not in creator:
        raise ValueError("Este importador foi validado para projetos do Live 11")
    maps = {}
    multisamplers = root.findall(".//MultiSampler")
    for idx, sampler in enumerate(multisamplers):
        username = xml_value(sampler, "UserName", "") or ""
        first_part = sampler.find(".//MultiSamplePart")
        first_name = xml_value(first_part, "Name", "") if first_part is not None else ""
        if "OBERHEIM" in (username + first_name).upper():
            label, filename = "OBERHEIM SWELL", "Oberheim_Swell.psmap"
        elif "SHIMER" in (username + first_name).upper():
            label, filename = "Shimer + Big Sky", "Shimer_Big_Sky.psmap"
        else:
            label, filename = username or f"Live Sampler {idx + 1}", f"Sampler_{idx + 1}.psmap"
        samples = [entry_for_part(p) for p in sampler.findall(".//MultiSamplePart")]
        maps[filename] = map_obj(label, [s for s in samples if s], creator)

    # For OriginalSimpler in Drum Racks, the trigger key is in the DrumBranch,
    # not in the sample's own 0..127 KeyRange.
    parents = {child: par for par in root.iter() for child in par}
    drums = []
    for simpler in root.findall(".//OriginalSimpler"):
        branch = simpler
        while branch is not None and branch.tag != "DrumBranch":
            branch = parents.get(branch)
        if branch is None:
            continue
        key = xml_value(branch, "BranchInfo/ReceivingNote")
        if key is None:
            continue
        for part in simpler.findall(".//MultiSamplePart"):
            sample = entry_for_part(part, drum_note=int(key))
            if sample:
                drums.append(sample)
    if drums:
        maps["Drum_Pad.psmap"] = map_obj("DRUM PAD", drums, creator)
    return maps, creator


def astoria_map(sample_names, creator):
    pitches = {}
    for path in sample_names:
        name = PurePosixPath(path).name
        match = ASTORIA_NAME.fullmatch(name)
        if not match:
            continue
        pitch_class, octave, velocity = match.groups()
        # Confirmed against the actual Live MultiSampler RootKey tags:
        # C2 = MIDI 48; C4 = MIDI 72, hence C0 = MIDI 24.
        midi_note = 24 + (int(octave) * 12) + PITCH_CLASSES[pitch_class.upper()]
        if 0 <= midi_note <= 127:
            pitches[(midi_note, int(velocity))] = safe_sample_path(path)

    if not pitches:
        return None
    note_roots = sorted({n for n, _ in pitches})
    velocities = sorted({v for _, v in pitches})
    if len(note_roots) < 2 or len(velocities) < 1:
        raise ValueError("O piano Astoria esta incompleto: faltam notas/velocidades")
    entries = []
    for i, root in enumerate(note_roots):
        low = 0 if i == 0 else (note_roots[i - 1] + root) // 2 + 1
        high = 127 if i == len(note_roots) - 1 else (root + note_roots[i + 1]) // 2
        for j, vel in enumerate(velocities):
            ref = pitches.get((root, vel))
            if ref is None:
                continue
            vel_low = 1 if j == 0 else (velocities[j - 1] + vel) // 2 + 1
            vel_high = 127 if j == len(velocities) - 1 else (vel + velocities[j + 1]) // 2
            entries.append({"file": ref, "root": root, "lo": low, "hi": high,
                            "velLo": vel_low, "velHi": vel_high})
    return map_obj("ASTORIA GRAND XL - 5 velocidades", entries,
                   f"Samples coletados de {creator}; o piano nao esta ativo no .als")


def read_zip(path):
    with zipfile.ZipFile(path) as archive:
        members = [n for n in archive.namelist()
                   if not n.startswith("__MACOSX/") and not PurePosixPath(n).name.startswith("._")]
        sets = [n for n in members if n.lower().endswith(".als")]
        if not sets:
            raise ValueError("O ZIP nao contem projeto .als")
        main_set = next((n for n in sets if "backup" not in n.lower()), sets[0])
        xml_data = gzip.decompress(archive.read(main_set))
        folder = PurePosixPath(main_set).parent
        samples = {}
        for member in members:
            p = PurePosixPath(member)
            if p.suffix.lower() not in SAMPLE_SUFFIXES:
                continue
            if p.parts[:len(folder.parts)] != folder.parts:
                continue
            relative = p.relative_to(folder).as_posix()
            samples[safe_sample_path(relative)] = member
        return xml_data, samples


def read_input(path):
    if path.suffix.lower() == ".zip":
        return read_zip(path)
    if path.suffix.lower() == ".als":
        project_folder = path.parent
        audio = {p.relative_to(project_folder).as_posix(): str(p)
                 for p in (project_folder / "Samples").rglob("*") if p.is_file()
                 and p.suffix.lower() in SAMPLE_SUFFIXES} if (project_folder / "Samples").is_dir() else {}
        return gzip.decompress(path.read_bytes()), audio
    raise ValueError("Escolha um arquivo .als ou ZIP coletado pelo Ableton")


def write_maps(source_path, output, extract_samples=False, dry_run=False):
    xml_data, available = read_input(source_path)
    maps, creator = parse_project(xml_data)
    astoria = astoria_map(available.keys(), creator)
    if astoria:
        maps["Astoria_Grand_XL.psmap"] = astoria

    expected = {item["file"] for item in maps.values() for item in item["samples"]}
    missing = sorted(expected - set(available))
    result = {
        "live": creator,
        "mapas": {k: len(v["samples"]) for k, v in maps.items()},
        "samples_encontrados": len(expected - set(missing)),
        "samples_ausentes": missing,
        "observacoes": [
            "Sem emulacao de efeitos ou instrumentos nativos exclusivos do Ableton.",
            "O .als nao disponibiliza os presets externos .adg/.adv ausentes do ZIP.",
            "Os mapas apontam para samples locais; audio nao deve ser publicado no GitHub.",
            "A reproduçao inicial nao inclui warp, loops, crossfades ou automacoes do Live.",
        ],
    }

    if dry_run:
        return result
    output.mkdir(parents=True, exist_ok=True)
    for filename, data in maps.items():
        (output / filename).write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (output / "relatorio-importacao.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    if extract_samples:
        if source_path.suffix.lower() != ".zip":
            raise ValueError("--extract-samples exige um ZIP como entrada")
        with zipfile.ZipFile(source_path) as archive:
            for sample_path in sorted(expected - set(missing)):
                destination = output.joinpath(*PurePosixPath(sample_path).parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(available[sample_path]) as src, destination.open("wb") as dst:
                    import shutil
                    shutil.copyfileobj(src, dst)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("projeto", type=Path, help="arquivo .als ou .zip do Live 11")
    parser.add_argument("--out", type=Path, required=True, help="pasta de saida (ao lado de Samples/)")
    parser.add_argument("--extract-samples", action="store_true",
                        help="copia as amostras do ZIP somente para a pasta local --out")
    parser.add_argument("--dry-run", action="store_true", help="somente relata; nao grava arquivos")
    args = parser.parse_args(argv)
    try:
        result = write_maps(args.projeto, args.out, args.extract_samples, args.dry_run)
    except (OSError, ValueError, ET.ParseError, zipfile.BadZipFile) as exc:
        parser.exit(2, f"Falha: {exc}\n")
    print(json.dumps(result, indent=2, ensure_ascii=False))
    if result["samples_ausentes"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
