from __future__ import annotations

import argparse
import csv
import hashlib
import os
import shutil
import sys
import tempfile
import time
import zipfile
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

PROJECT_DIR = Path(__file__).resolve().parent
PYTHON_DIR = PROJECT_DIR / "python"
if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))

from tqdm import tqdm

DEFAULT_OUTPUT_ROOT = os.path.abspath("DeltaDCT_Output")


def _load_deltadct_manager():
    from deltadct_manager import GlobalDedupEngine, pydeltadct

    return GlobalDedupEngine, pydeltadct


def _resolve_output_root(user_value: str) -> Tuple[str, bool, str]:
    requested = (user_value or "").strip()
    if not requested:
        os.makedirs(DEFAULT_OUTPUT_ROOT, exist_ok=True)
        return DEFAULT_OUTPUT_ROOT, True, f"未提供输出根目录，已回退到默认路径: {DEFAULT_OUTPUT_ROOT}"

    candidate = os.path.abspath(requested)
    try:
        os.makedirs(candidate, exist_ok=True)
        return candidate, False, f"当前生效的输出根目录: {candidate}"
    except OSError:
        os.makedirs(DEFAULT_OUTPUT_ROOT, exist_ok=True)
        return DEFAULT_OUTPUT_ROOT, True, f"无法创建指定输出根目录，已回退到默认路径: {DEFAULT_OUTPUT_ROOT}"


def _safe_sha256(path: str) -> str:
    try:
        hasher = hashlib.sha256()
        with open(path, "rb") as file_handle:
            for chunk in iter(lambda: file_handle.read(1024 * 1024), b""):
                hasher.update(chunk)
        return hasher.hexdigest()
    except OSError:
        return ""


def _hash_file(path: str, algo: str) -> str:
    try:
        hasher = hashlib.new(algo)
        with open(path, "rb") as file_handle:
            for chunk in iter(lambda: file_handle.read(1024 * 1024), b""):
                hasher.update(chunk)
        return hasher.hexdigest()
    except OSError:
        return ""


def _load_manifest(zf: zipfile.ZipFile) -> Dict[str, Dict[str, str]]:
    if "manifest.json" not in zf.namelist():
        raise ValueError("ddctpack missing manifest.json")
    import json

    payload = zf.read("manifest.json").decode("utf-8")
    data = json.loads(payload)
    if not isinstance(data, dict):
        raise ValueError("manifest.json must be a dict")
    return data


def _extract_pack_entry(zf: zipfile.ZipFile, entry_name: str, temp_dir: str) -> str:
    if not entry_name:
        return ""
    names = set(zf.namelist())
    if entry_name not in names:
        return ""
    extracted_path = zf.extract(entry_name, path=temp_dir)
    return extracted_path if os.path.isfile(extracted_path) else ""


def _iter_jpeg_pairs(dataset_dir: str) -> List[Tuple[str, str]]:
    GlobalDedupEngine, _ = _load_deltadct_manager()
    engine = GlobalDedupEngine()
    jpeg_files = engine._scan_jpeg_files(dataset_dir)
    pairs: List[Tuple[str, str]] = []
    files_by_name = {os.path.basename(path): path for path in jpeg_files}
    for path in jpeg_files:
        name = os.path.basename(path)
        lower = name.lower()
        if lower.endswith("_target.jpg") or lower.endswith("_target_shifted.jpg"):
            prefix = name.split("_target")[0]
            base_path = None
            for base_name, candidate_path in files_by_name.items():
                if base_name.startswith(f"{prefix}_base") and base_name.lower().endswith((".jpg", ".jpeg")):
                    base_path = candidate_path
                    break
            if base_path:
                pairs.append((path, base_path))
    return pairs


def _get_routing_mode(mode_str: str):
    """Map string to pydeltadct.RoutingMode enum"""
    _, pydeltadct = _load_deltadct_manager()
    mode_map = {
        "auto": pydeltadct.RoutingMode.ROUTING_AUTO,
        "fpm": pydeltadct.RoutingMode.FPM_ONLY,
        "dchash": pydeltadct.RoutingMode.DCHASH_ONLY,
    }
    mode = mode_map.get(mode_str.lower())
    if mode is None:
        raise ValueError(f"Invalid routing mode: {mode_str}. Must be one of: auto, fpm, dchash")
    return mode


def _scan_command(args: argparse.Namespace) -> int:
    output_root, used_default, message = _resolve_output_root(args.out)
    print(message)

    GlobalDedupEngine, _ = _load_deltadct_manager()

    routing_mode = None
    if hasattr(args, "force_route") and args.force_route:
        routing_mode = _get_routing_mode(args.force_route)
        print(f"🚦 强制路由模式: {args.force_route}")

    engine = GlobalDedupEngine(
        vote_threshold=args.threshold,
        force_dedup=args.force,
        strict_lossless=True,
        routing_mode=routing_mode,
    )
    engine.output_root_base = output_root

    progress_bar: Optional[tqdm] = None
    last_processed = 0
    last_status = ""
    last_output_root = ""

    for event in engine.scan_directory(args.src):
        stats = event.get("stats", {})
        if event.get("event") == "start":
            total_images = max(1, int(stats.get("total_images", 0)))
            progress_bar = tqdm(
                total=total_images,
                desc="Scanning",
                unit="img",
                colour="green",
                dynamic_ncols=True,
            )
            continue

        if event.get("event") == "progress":
            if progress_bar is None:
                continue
            processed = int(stats.get("processed_images", 0))
            progress_bar.update(max(0, processed - last_processed))
            last_processed = processed
            result = event.get("result") or {}
            status = str(result.get("status", ""))
            image = os.path.basename(str(event.get("image", "")))
            if status and image:
                last_status = f"{status}:{image}"
                progress_bar.set_postfix_str(last_status, refresh=False)
            continue

        if event.get("event") == "done":
            last_output_root = str(event.get("output_root") or output_root)
            if progress_bar is not None:
                progress_bar.close()
            print(f"扫描完成，输出根目录: {last_output_root}")
            print(f"ddctpack: {event.get('ddctpack_path') or '(未生成)'}")
            print(f"状态: {last_status or 'ok'}")
            print(f"统计: {stats}")
            return 0

    if progress_bar is not None:
        progress_bar.close()
    print(f"扫描结束，输出根目录: {last_output_root or output_root}")
    return 0


def _restore_command(args: argparse.Namespace) -> int:
    _, pydeltadct = _load_deltadct_manager()
    pack_path = os.path.abspath(args.pack)
    output_dir = os.path.abspath(args.out)
    if not os.path.isfile(pack_path):
        print(f"Missing ddctpack: {pack_path}", file=sys.stderr)
        return 2

    os.makedirs(output_dir, exist_ok=True)
    temp_dir = tempfile.mkdtemp(prefix="ddctpack_restore_")
    restored = 0
    try:
        with zipfile.ZipFile(pack_path, "r") as zf:
            manifest = _load_manifest(zf)
            total = len(manifest)
            progress_bar = tqdm(total=total, desc="Restoring", unit="file", colour="green", dynamic_ncols=True)
            try:
                for target_rel, info in manifest.items():
                    ddct_name = str(info.get("ddct_file", ""))
                    base_name = str(info.get("base_file", info.get("required_base", "")))
                    ddct_path = _extract_pack_entry(zf, ddct_name, temp_dir)
                    base_path = _extract_pack_entry(zf, base_name, temp_dir)
                    if not ddct_path or not base_path:
                        progress_bar.update(1)
                        continue

                    output_path = os.path.join(output_dir, target_rel)
                    output_parent = os.path.dirname(output_path)
                    if output_parent:
                        os.makedirs(output_parent, exist_ok=True)

                    if pydeltadct.decompress_image(ddct_path, base_path, output_path) and os.path.isfile(output_path):
                        restored += 1
                    progress_bar.update(1)
            finally:
                progress_bar.close()
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)

    print(f"还原完成: {restored} / {len(manifest)}")
    print(f"输出目录: {output_dir}")
    return 0


def _benchmark_command(args: argparse.Namespace) -> int:
    GlobalDedupEngine, pydeltadct = _load_deltadct_manager()
    dataset_dir = os.path.abspath(args.dir)
    if not os.path.isdir(dataset_dir):
        print(f"Missing dataset directory: {dataset_dir}", file=sys.stderr)
        return 2

    pairs = _iter_jpeg_pairs(dataset_dir)
    if not pairs:
        print(f"No JPEG target/base pairs found in {dataset_dir}", file=sys.stderr)
        return 3

    output_dir = os.path.join(PROJECT_DIR, "benchmark_data")
    os.makedirs(output_dir, exist_ok=True)
    csv_path = os.path.join(output_dir, "benchmark_results.csv")

    total_original = 0
    total_md5_size = 0
    total_zlib_size = 0
    total_deltadct_size = 0
    total_base_size = 0
    unique_base_paths = set()
    deltadct_time = 0.0

    import zlib

    progress_bar = tqdm(pairs, desc="Benchmark", unit="pair", colour="green", dynamic_ncols=True)
    for target_path, base_path in progress_bar:
        original_size = os.path.getsize(target_path)
        base_size = os.path.getsize(base_path)
        total_original += original_size
        total_base_size += base_size
        unique_base_paths.add(os.path.abspath(base_path))

        with open(target_path, "rb") as target_handle, open(base_path, "rb") as base_handle:
            if hashlib.md5(target_handle.read()).hexdigest() == hashlib.md5(base_handle.read()).hexdigest():
                total_md5_size += 0
            else:
                total_md5_size += original_size

        with open(target_path, "rb") as target_handle:
            total_zlib_size += len(zlib.compress(target_handle.read(), level=9))

        out_ddct = os.path.join(output_dir, f"{Path(target_path).stem}.ddct")
        start = time.perf_counter()
        routing_mode = None
        if hasattr(args, "force_route") and args.force_route:
            routing_mode = _get_routing_mode(args.force_route)
        if routing_mode is not None:
            result = pydeltadct.compress_image(target_path, base_path, out_ddct, routing_mode)
        else:
            result = pydeltadct.compress_image(target_path, base_path, out_ddct)
        deltadct_time += time.perf_counter() - start
        if result.success:
            total_deltadct_size += int(result.compressed_size)
            if os.path.isfile(out_ddct):
                os.remove(out_ddct)
        else:
            total_deltadct_size += original_size
            if os.path.isfile(out_ddct):
                os.remove(out_ddct)

        progress_bar.set_postfix_str(f"{Path(target_path).name}", refresh=False)
    progress_bar.close()

    md5_ratio = total_original / total_md5_size if total_md5_size > 0 else 1.0
    zlib_ratio = total_original / total_zlib_size if total_zlib_size > 0 else 1.0
    deltadct_ratio = total_original / total_deltadct_size if total_deltadct_size > 0 else 1.0
    total_full_original_size = total_original + total_base_size
    total_base_pack_size = sum(os.path.getsize(path) for path in unique_base_paths)
    full_flow_storage_size = total_base_pack_size + total_deltadct_size
    full_flow_ratio = total_full_original_size / full_flow_storage_size if full_flow_storage_size > 0 else 1.0
    throughput_mbps = (total_original / 1024 / 1024) / deltadct_time if deltadct_time > 0 else 0.0

    results = [{
        "Dataset": os.path.basename(dataset_dir),
        "Original Size (MB)": round(total_original / 1024 / 1024, 2),
        "Full Original Size (MB)": round(total_full_original_size / 1024 / 1024, 2),
        "Base Pack Size (MB)": round(total_base_pack_size / 1024 / 1024, 2),
        "Full Flow Ratio": round(full_flow_ratio, 2),
        "MD5 Ratio": round(md5_ratio, 2),
        "Zlib Ratio": round(zlib_ratio, 2),
        "DeltaDCT Ratio": round(deltadct_ratio, 2),
        "Throughput (MB/s)": round(throughput_mbps, 2),
    }]

    with open(csv_path, mode="w", newline="", encoding="utf-8") as file_handle:
        writer = csv.DictWriter(file_handle, fieldnames=results[0].keys())
        writer.writeheader()
        writer.writerows(results)

    print(f"Benchmark results written to: {csv_path}")
    print(f"DeltaDCT Ratio: {deltadct_ratio:.2f}x vs MD5 Ratio: {md5_ratio:.2f}x")
    print(f"Full Flow Ratio: {full_flow_ratio:.2f}x")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="DeltaDCT command line interface")
    subparsers = parser.add_subparsers(dest="command", required=True)

    scan_parser = subparsers.add_parser("scan", help="Scan a directory and build .ddct/.ddctpack outputs")
    scan_parser.add_argument("--src", required=True, help="Source directory to scan")
    scan_parser.add_argument("--out", required=True, help="Output root directory")
    scan_parser.add_argument("--threshold", type=int, default=2, help="Vote threshold")
    scan_parser.add_argument("--force", action="store_true", help="Force dedup even if ratio is low")
    scan_parser.add_argument(
        "--force-route",
        choices=["auto", "fpm", "dchash"],
        default=None,
        help="Force specific routing mode: auto (automatic), fpm (FPM-only), dchash (DCHash-only)",
    )
    scan_parser.set_defaults(func=_scan_command)

    restore_parser = subparsers.add_parser("restore", help="Restore all files from a .ddctpack archive")
    restore_parser.add_argument("--pack", required=True, help="Path to .ddctpack file")
    restore_parser.add_argument("--out", required=True, help="Output directory for restored images")
    restore_parser.set_defaults(func=_restore_command)

    benchmark_parser = subparsers.add_parser("benchmark", help="Benchmark a dataset directory")
    benchmark_parser.add_argument("--dir", required=True, help="Dataset directory")
    benchmark_parser.add_argument(
        "--force-route",
        choices=["auto", "fpm", "dchash"],
        default=None,
        help="Force specific routing mode: auto (automatic), fpm (FPM-only), dchash (DCHash-only)",
    )
    benchmark_parser.set_defaults(func=_benchmark_command)

    return parser


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
