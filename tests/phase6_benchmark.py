#!/usr/bin/env python3
import argparse
import csv
import json
import os
import re
import subprocess
import sys
from pathlib import Path


def parse_metrics(output: str) -> dict:
    metrics = {
        "router_branch": "",
        "original_size_bytes": None,
        "dedup_size_raw_bytes": None,
        "dedup_size_zstd_bytes": None,
        "compression_ratio_raw": None,
        "compression_ratio_zstd": None,
        "similarity_seconds": None,
        "idelta_seconds": None,
        "total_seconds": None,
        "throughput_mb_s": None,
        "restore_luma_dct": "",
        "restore_bytewise": "",
        "reference_hash_fnv1a64": "",
        "restored_hash_fnv1a64": "",
    }

    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line:
            continue

        if line.startswith("[Router] Using "):
            metrics["router_branch"] = line.replace("[Router] Using ", "").replace(" Branch", "")
            continue

        m = re.match(r"^Original Size \(bytes\):\s*(\d+)$", line)
        if m:
            metrics["original_size_bytes"] = int(m.group(1))
            continue

        m = re.match(r"^Dedup Size RAW \(bytes\):\s*(\d+)$", line)
        if m:
            metrics["dedup_size_raw_bytes"] = int(m.group(1))
            continue

        m = re.match(r"^Dedup Size ZSTD \(bytes\):\s*(\d+)$", line)
        if m:
            metrics["dedup_size_zstd_bytes"] = int(m.group(1))
            continue

        m = re.match(r"^Compression Ratio RAW:\s*([0-9eE+\-.]+)$", line)
        if m:
            metrics["compression_ratio_raw"] = float(m.group(1))
            continue

        m = re.match(r"^Compression Ratio ZSTD:\s*([0-9eE+\-.]+)$", line)
        if m:
            metrics["compression_ratio_zstd"] = float(m.group(1))
            continue

        m = re.match(r"^Similarity Detector Time \(s\):\s*([0-9eE+\-.]+)$", line)
        if m:
            metrics["similarity_seconds"] = float(m.group(1))
            continue

        m = re.match(r"^Idelta Compressor Time \(s\):\s*([0-9eE+\-.]+)$", line)
        if m:
            metrics["idelta_seconds"] = float(m.group(1))
            continue

        m = re.match(r"^Total Time \(s\):\s*([0-9eE+\-.]+)$", line)
        if m:
            metrics["total_seconds"] = float(m.group(1))
            continue

        m = re.match(r"^Throughput \(MB/s\):\s*([0-9eE+\-.]+)$", line)
        if m:
            metrics["throughput_mb_s"] = float(m.group(1))
            continue

        m = re.match(r"^Restore Check Luma-DCT:\s*(\w+)$", line)
        if m:
            metrics["restore_luma_dct"] = m.group(1)
            continue

        m = re.match(r"^Restore Check Bytewise:\s*(\w+)$", line)
        if m:
            metrics["restore_bytewise"] = m.group(1)
            continue

        m = re.match(r"^Reference Hash FNV1a64:\s*(0x[0-9a-fA-F]+)$", line)
        if m:
            metrics["reference_hash_fnv1a64"] = m.group(1)
            continue

        m = re.match(r"^Restored Hash FNV1a64:\s*(0x[0-9a-fA-F]+)$", line)
        if m:
            metrics["restored_hash_fnv1a64"] = m.group(1)
            continue

    return metrics


def write_csv_row(csv_path: Path, row: dict) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "base",
        "target",
        "exit_code",
        "router_branch",
        "original_size_bytes",
        "dedup_size_raw_bytes",
        "dedup_size_zstd_bytes",
        "compression_ratio_raw",
        "compression_ratio_zstd",
        "similarity_seconds",
        "idelta_seconds",
        "total_seconds",
        "throughput_mb_s",
        "restore_luma_dct",
        "restore_bytewise",
        "reference_hash_fnv1a64",
        "restored_hash_fnv1a64",
    ]

    exists = csv_path.exists()
    with csv_path.open("a", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=fieldnames)
        if not exists:
            writer.writeheader()
        writer.writerow(row)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Phase6 benchmark for one base-target pair.")
    parser.add_argument("--base", required=True, help="Base JPEG path")
    parser.add_argument("--target", required=True, help="Target JPEG path")
    parser.add_argument("--out-dir", required=True, help="Output directory for benchmark artifacts")
    parser.add_argument("--tool", default="./build/deltadct_tool", help="Path to deltadct_tool")
    parser.add_argument("--window-size", type=int, default=2)
    parser.add_argument("--num-transforms", type=int, default=10)
    parser.add_argument("--zstd-level", type=int, default=3)
    parser.add_argument("--json-out", default="", help="Optional JSON output path")
    parser.add_argument("--csv-out", default="", help="Optional CSV append path")
    args = parser.parse_args()

    cmd = [
        args.tool,
        "phase6-benchmark",
        args.base,
        args.target,
        args.out_dir,
        str(args.window_size),
        str(args.num_transforms),
        str(args.zstd_level),
    ]

    completed = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(completed.stdout)
    sys.stderr.write(completed.stderr)

    metrics = parse_metrics(completed.stdout)
    metrics["base"] = args.base
    metrics["target"] = args.target
    metrics["exit_code"] = completed.returncode

    if args.json_out:
        json_path = Path(args.json_out)
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(json.dumps(metrics, indent=2, ensure_ascii=True), encoding="utf-8")

    if args.csv_out:
        write_csv_row(Path(args.csv_out), metrics)

    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
