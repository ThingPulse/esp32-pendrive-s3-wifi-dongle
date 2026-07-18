#!/usr/bin/env python3
"""Generate and validate ESP App Market release assets."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path

EXPECTED = {
    "bootloader": ("bootloader/bootloader.bin", 0x0),
    "partition-table": ("partition_table/partition-table.bin", 0x8000),
    "otadata": ("ota_data_initial.bin", 0xD000),
    "application": ("usb_dongle.bin", 0x10000),
}
SEMVER = re.compile(r"^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def normalize_version(value: str) -> str:
    value = value.strip()
    match = SEMVER.fullmatch(value)
    if not match:
        raise ValueError(f"version is not SemVer: {value!r}")
    return value[1:] if value.startswith("v") else value


def is_prerelease(value: str) -> bool:
    normalized = normalize_version(value)
    return "-" in normalized.split("+", 1)[0]


def version_from_git(root: Path) -> str:
    result = subprocess.run(
        ["git", "describe", "--tags", "--exact-match", "--match", "v*"],
        cwd=root, check=True, text=True, capture_output=True,
    )
    return normalize_version(result.stdout)


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def discover(build_dir: Path) -> list[dict]:
    args_path = build_dir / "flasher_args.json"
    if not args_path.is_file():
        raise FileNotFoundError(f"missing ESP-IDF flash metadata: {args_path}")
    flash_files = json.loads(args_path.read_text(encoding="utf-8")).get("flash_files", {})
    by_path = {Path(value).as_posix(): int(offset, 0) for offset, value in flash_files.items()}
    partitions = []
    for name, (relative, expected_offset) in EXPECTED.items():
        if relative not in by_path:
            raise FileNotFoundError(f"expected artifact is absent from flasher_args.json: {relative}")
        offset = by_path[relative]
        if offset != expected_offset:
            raise ValueError(f"unexpected offset for {relative}: {offset:#x}, expected {expected_offset:#x}")
        source = build_dir / relative
        if not source.is_file():
            raise FileNotFoundError(f"expected artifact is absent: {source}")
        partitions.append({"name": name, "source": source, "asset": source.name, "offset": offset, "size": source.stat().st_size})
    return sorted(partitions, key=lambda item: item["offset"])


def check_ranges(partitions: list[dict]) -> None:
    previous = None
    for item in sorted(partitions, key=lambda value: value["offset"]):
        if item["offset"] < 0 or item["size"] <= 0:
            raise ValueError(f"invalid offset or size for {item['name']}")
        if previous and item["offset"] < previous["offset"] + previous["size"]:
            raise ValueError(f"flash ranges overlap: {previous['name']} and {item['name']}")
        previous = item


def load_app_metadata(template_path: Path) -> dict:
    template = json.loads(template_path.read_text(encoding="utf-8"))
    app = template.get("app")
    if not isinstance(app, dict):
        raise ValueError(f"missing application metadata in {template_path}")
    required = ("id", "name", "description", "supportedDevices", "tags", "icon")
    if any(field not in app for field in required):
        raise ValueError(f"incomplete application metadata in {template_path}")
    icon = app.get("icon")
    if not isinstance(icon, dict) or Path(icon.get("asset", "")).name != icon.get("asset", ""):
        raise ValueError("app.icon.asset must be a release asset filename")
    return app


def make_manifest(version: str, partitions: list[dict], app: dict, icon_source: Path) -> dict:
    check_ranges(partitions)
    return {
        "schemaVersion": 1,
        "app": {**app, "icon": {"asset": app["icon"]["asset"], "sha256": digest(icon_source)}},
        "release": {
            "version": normalize_version(version),
            "partitions": [
                {"name": item["name"], "asset": item["asset"], "offset": hex(item["offset"]), "sha256": digest(item["source"])}
                for item in partitions
            ],
        },
    }


def validate(manifest_path: Path) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    app = manifest.get("app", {})
    required = ("id", "name", "description", "supportedDevices", "tags", "icon")
    if manifest.get("schemaVersion") != 1 or not isinstance(app, dict) or any(field not in app for field in required):
        raise ValueError("invalid schemaVersion or incomplete application metadata")
    icon = app["icon"]
    icon_path = manifest_path.parent / icon["asset"]
    if Path(icon["asset"]).name != icon["asset"] or not icon_path.is_file():
        raise FileNotFoundError(f"missing release icon: {icon_path}")
    if not SHA256.fullmatch(icon.get("sha256", "")) or digest(icon_path) != icon["sha256"]:
        raise ValueError(f"invalid checksum for {icon['asset']}")
    normalize_version(manifest.get("release", {}).get("version", ""))
    partitions = manifest["release"].get("partitions", [])
    if len(partitions) != len(EXPECTED):
        raise ValueError("manifest has an unexpected partition count")
    ranges = []
    for item in partitions:
        asset = item.get("asset", "")
        if Path(asset).name != asset or not asset:
            raise ValueError(f"asset must be a filename: {asset!r}")
        path = manifest_path.parent / asset
        if not path.is_file():
            raise FileNotFoundError(f"missing release asset: {path}")
        checksum = item.get("sha256", "")
        if not SHA256.fullmatch(checksum) or digest(path) != checksum:
            raise ValueError(f"invalid checksum for {asset}")
        try:
            offset = int(item.get("offset", ""), 0)
        except (TypeError, ValueError) as error:
            raise ValueError(f"invalid offset for {asset}") from error
        ranges.append({"name": item.get("name", asset), "offset": offset, "size": path.stat().st_size})
    check_ranges(ranges)
    return manifest


def generate(root: Path, build_dir: Path, output_dir: Path, version: str | None) -> Path:
    app = load_app_metadata(root / "app-market.json")
    icon_source = root / "_static" / app["icon"]["asset"]
    if not icon_source.is_file():
        raise FileNotFoundError(f"missing application icon: {icon_source}")
    partitions = discover(build_dir)
    manifest = make_manifest(version or version_from_git(root), partitions, app, icon_source)
    output_dir.mkdir(parents=True, exist_ok=True)
    for item in partitions:
        shutil.copyfile(item["source"], output_dir / item["asset"])
    shutil.copyfile(icon_source, output_dir / app["icon"]["asset"])
    output = output_dir / "app-market.json"
    output.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    validate(output)
    return output


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    generate_parser = subparsers.add_parser("generate")
    generate_parser.add_argument("--build-dir", type=Path, default=Path("build"))
    generate_parser.add_argument("--output-dir", type=Path, default=Path("release"))
    generate_parser.add_argument("--version")
    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("manifest", type=Path, nargs="?", default=Path("release/app-market.json"))
    prerelease_parser = subparsers.add_parser("is-prerelease")
    prerelease_parser.add_argument("version")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.command == "generate":
        print(generate(root, args.build_dir, args.output_dir, args.version))
    elif args.command == "validate":
        validate(args.manifest)
        print(f"valid: {args.manifest}")
    else:
        print("true" if is_prerelease(args.version) else "false")


if __name__ == "__main__":
    main()
