#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import sys
import tempfile
import zipfile
from typing import Dict, Tuple


def _import_pydeltadct() -> object:
    python_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(python_dir)
    py_tag = f"py{sys.version_info.major}{sys.version_info.minor}"
    build_candidates = [
        os.path.join(project_dir, f"build_{py_tag}"),
        os.path.join(project_dir, "build"),
        os.path.join(project_dir, "build_py311"),
    ]

    for build_dir in build_candidates:
        if os.path.isdir(build_dir) and build_dir not in sys.path:
            sys.path.insert(0, build_dir)

    try:
        return __import__("pydeltadct")
    except ImportError as exc:
        raise SystemExit(f"Failed to import pydeltadct: {exc}")


def _hash_file(path: str, algo: str) -> str:
    try:
        hasher = hashlib.new(algo)
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                hasher.update(chunk)
        return hasher.hexdigest()
    except OSError:
        return ""


def _compute_pixel_mse(path_a: str, path_b: str) -> Tuple[bool, float]:
    try:
        from PIL import Image, ImageChops
    except Exception:
        return False, -1.0

    try:
        img_a = Image.open(path_a).convert("RGB")
        img_b = Image.open(path_b).convert("RGB")
    except Exception:
        return False, -1.0

    if img_a.size != img_b.size:
        return False, float("inf")

    diff = ImageChops.difference(img_a, img_b)
    if diff.getbbox() is None:
        return True, 0.0

    hist = diff.histogram()
    total = img_a.size[0] * img_a.size[1] * 3
    if total == 0:
        return True, 0.0

    sq_sum = 0.0
    for idx, value in enumerate(hist):
        sq_sum += value * float((idx % 256) ** 2)
    return False, sq_sum / float(total)


def _load_manifest(zf: zipfile.ZipFile) -> Dict[str, Dict[str, str]]:
    if "manifest.json" not in zf.namelist():
        raise ValueError("ddctpack missing manifest.json")
    payload = zf.read("manifest.json").decode("utf-8")
    data = json.loads(payload)
    if not isinstance(data, dict):
        raise ValueError("manifest.json must be a dict")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify ddctpack integrity.")
    parser.add_argument("ddctpack", help="Path to .ddctpack archive")
    parser.add_argument("output_dir", help="Output directory for restored images")
    parser.add_argument(
        "--source-root",
        default="",
        help="Optional root directory for original images, used for pixel/hash verification.",
    )
    parser.add_argument(
        "--strict-lossless",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Require MSE=0 and matching hashes; report critical failures.",
    )
    args = parser.parse_args()

    ddctpack_path = os.path.abspath(args.ddctpack)
    output_dir = os.path.abspath(args.output_dir)
    source_root = os.path.abspath(args.source_root) if args.source_root.strip() else ""

    if not os.path.isfile(ddctpack_path):
        raise SystemExit(f"Missing ddctpack: {ddctpack_path}")
    if source_root and not os.path.isdir(source_root):
        raise SystemExit(f"Missing source root: {source_root}")

    os.makedirs(output_dir, exist_ok=True)

    pydeltadct = _import_pydeltadct()

    report_path = os.path.join(output_dir, "Integrity_Report.txt")
    temp_dir = tempfile.mkdtemp(prefix="ddctpack_verify_")

    total = 0
    restored = 0
    verified = 0
    mismatched = 0
    critical = 0
    missing_base = 0
    missing_original = 0
    missing_ddct = 0
    failed = 0

    with zipfile.ZipFile(ddctpack_path, "r") as zf, open(report_path, "w", encoding="utf-8") as report:
        manifest = _load_manifest(zf)
        report.write("DeltaDCT Integrity Report\n")
        report.write(f"Archive: {ddctpack_path}\n")
        report.write(f"Source root: {source_root or '(not provided)'}\n")
        report.write(f"Output dir: {output_dir}\n\n")

        for target_rel, info in manifest.items():
            total += 1
            ddct_name = str(info.get("ddct_file", ""))
            base_name = str(info.get("base_file", info.get("required_base", "")))
            original_sha_in_manifest = str(info.get("original_sha256", ""))

            if not ddct_name or ddct_name not in zf.namelist():
                missing_ddct += 1
                report.write(f"[MISSING_DDCT] {target_rel}\n")
                continue

            if not base_name or base_name not in zf.namelist():
                missing_base += 1
                report.write(f"[MISSING_BASE] {target_rel} -> {base_name}\n")
                continue

            original_path = os.path.join(source_root, target_rel) if source_root else ""
            if source_root and not os.path.isfile(original_path):
                missing_original += 1
                report.write(f"[MISSING_ORIGINAL] {target_rel}\n")
                continue

            extracted_ddct = zf.extract(ddct_name, path=temp_dir)
            extracted_base = zf.extract(base_name, path=temp_dir)
            output_path = os.path.join(output_dir, target_rel)
            output_parent = os.path.dirname(output_path)
            if output_parent:
                os.makedirs(output_parent, exist_ok=True)

            ok = pydeltadct.decompress_image(extracted_ddct, extracted_base, output_path)
            if not ok or not os.path.isfile(output_path):
                failed += 1
                report.write(f"[RESTORE_FAILED] {target_rel}\n")
                continue

            restored += 1
            restored_sha = _hash_file(output_path, "sha256")
            restored_md5 = _hash_file(output_path, "md5")
            expected_sha = original_sha_in_manifest

            if source_root:
                original_md5 = _hash_file(original_path, "md5")
                original_sha = _hash_file(original_path, "sha256")
                pixel_equal, mse = _compute_pixel_mse(original_path, output_path)
                if not expected_sha:
                    expected_sha = original_sha
                sha_match = bool(expected_sha) and restored_sha == expected_sha
                md5_match = restored_md5 == original_md5

                if sha_match and md5_match and pixel_equal:
                    verified += 1
                    status = "VERIFIED"
                else:
                    mismatched += 1
                    status = "MISMATCH"

                if args.strict_lossless and (not pixel_equal or not sha_match or not md5_match):
                    status = "CRITICAL FAILURE"
                    critical += 1
            else:
                mse = -1.0
                sha_match = bool(expected_sha) and restored_sha == expected_sha
                md5_match = True
                verified += 1 if sha_match or not expected_sha else 0
                status = "VERIFIED" if (sha_match or not expected_sha) else "RESTORED"

            report.write(
                f"[{status}] {target_rel}\n"
                f"  sha256: {restored_sha}\n"
                f"  md5: {restored_md5}\n"
                f"  pixel_mse: {mse}\n"
                f"  sha_match: {sha_match}, md5_match: {md5_match}, pixel_equal: {mse == 0.0 if mse >= 0 else 'n/a'}\n"
            )

        report.write("\nSummary\n")
        report.write(f"Total: {total}\n")
        report.write(f"Restored: {restored}\n")
        report.write(f"Verified: {verified}\n")
        report.write(f"Mismatched: {mismatched}\n")
        report.write(f"Critical: {critical}\n")
        report.write(f"Missing ddct: {missing_ddct}\n")
        report.write(f"Missing base: {missing_base}\n")
        report.write(f"Missing original: {missing_original}\n")
        report.write(f"Restore failed: {failed}\n")

    try:
        for name in os.listdir(temp_dir):
            os.remove(os.path.join(temp_dir, name))
        os.rmdir(temp_dir)
    except OSError:
        pass

    print(f"Integrity report written to: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
