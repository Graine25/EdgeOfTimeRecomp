from __future__ import annotations

import argparse
import re
import sys
import tomllib
from pathlib import Path


MISSING_CALL_RE = re.compile(
    rb"Call to invalid or unregistered function at guest address "
    rb"(0x[0-9A-Fa-f]{8})",
    re.IGNORECASE,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Read the newest reeot log and add its last unregistered guest "
            "function to the matching codegen TOML."
        )
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="report the change without editing the TOML",
    )
    parser.add_argument(
        "--log-dir",
        type=Path,
        help="override the default RelWithDebInfo log directory",
    )
    return parser.parse_args()


def latest_log(log_dir: Path) -> Path:
    logs = [path for path in log_dir.glob("*.log") if path.is_file()]
    if not logs:
        raise RuntimeError(f"no .log files found in {log_dir}")
    return max(logs, key=lambda path: path.stat().st_mtime_ns)


def configs_for_address(repo_root: Path, address: str) -> tuple[Path, list[Path]]:
    prefix = address[2:4].lower()
    if prefix == "82":
        boundary_config = repo_root / "config" / "reeot_default_xex_boundaries.toml"
        return boundary_config, [
            boundary_config,
            repo_root / "config" / "reeot_default_xex.toml",
        ]
    if prefix == "88":
        boundary_config = repo_root / "config" / "reeot_gamelogic_boundaries.toml"
        return boundary_config, [
            boundary_config,
            repo_root / "config" / "reeot_gamelogic.toml",
        ]
    raise RuntimeError(
        f"cannot map {address} to a module; expected a 0x82 or 0x88 guest address"
    )


def add_boundary(
    config_path: Path,
    search_paths: list[Path],
    address: str,
    dry_run: bool,
) -> bool:
    data = config_path.read_bytes()
    canonical_address = "0x" + address[2:].upper()
    address_bytes = canonical_address.encode("ascii")
    existing_re = re.compile(
        rb"(?im)^\s*" + re.escape(address_bytes) + rb"\s*=",
    )
    for search_path in search_paths:
        if existing_re.search(search_path.read_bytes()):
            print(f"Already present: {canonical_address} in {search_path}")
            return False

    newline = b"\r\n" if b"\r\n" in data else b"\n"
    updated = data.rstrip(b"\r\n") + newline + address_bytes + b" = {}" + newline

    tomllib.loads(updated.decode("utf-8"))
    if dry_run:
        print(f"Would add: {canonical_address} -> {config_path}")
        return True

    temporary_path = config_path.with_suffix(config_path.suffix + ".tmp")
    temporary_path.write_bytes(updated)
    temporary_path.replace(config_path)
    print(f"Added: {canonical_address} -> {config_path}")
    return True


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    log_dir = args.log_dir or (
        repo_root / "out" / "build" / "win-amd64-relwithdebinfo" / "logs"
    )

    try:
        log_path = latest_log(log_dir.resolve())
        matches = list(MISSING_CALL_RE.finditer(log_path.read_bytes()))
        if not matches:
            raise RuntimeError(f"no unregistered guest-function call found in {log_path}")

        address = matches[-1].group(1).decode("ascii")
        config_path, search_paths = configs_for_address(repo_root, address)
        print(f"Latest log: {log_path}")
        add_boundary(config_path, search_paths, address, args.dry_run)
        return 0
    except (OSError, RuntimeError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
