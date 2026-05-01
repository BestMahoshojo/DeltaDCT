from __future__ import annotations

import hashlib
import importlib
import json
import os
import shutil
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Generator, List, Optional

PYTHON_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(PYTHON_DIR)
PY_TAG = f"py{sys.version_info.major}{sys.version_info.minor}"
_build_candidates = [
    os.path.join(PROJECT_DIR, f"build_{PY_TAG}"),
    os.path.join(PROJECT_DIR, "build"),
    os.path.join(PROJECT_DIR, "build_py311"),
]
BUILD_DIR_CANDIDATES: List[str] = []
for candidate in _build_candidates:
    if candidate not in BUILD_DIR_CANDIDATES:
        BUILD_DIR_CANDIDATES.append(candidate)


def _import_pydeltadct() -> Any:
    errors: List[str] = []
    for build_dir in BUILD_DIR_CANDIDATES:
        if not os.path.isdir(build_dir):
            continue
        if build_dir not in sys.path:
            sys.path.insert(0, build_dir)

        try:
            return importlib.import_module("pydeltadct")
        except Exception as exc:
            errors.append(f"{build_dir}: {exc}")
            if "pydeltadct" in sys.modules:
                del sys.modules["pydeltadct"]

    message = "pydeltadct is not importable. Build bindings first."
    if errors:
        message += " Tried: " + " | ".join(errors)
    raise ImportError(message)


pydeltadct = _import_pydeltadct()


def _format_block_similarity(
    exact: int,
    similar: int,
    different: int,
    total: int,
) -> str:
    if total <= 0:
        return ""
    exact_pct = 100.0 * float(exact) / float(total)
    similar_pct = 100.0 * float(similar) / float(total)
    diff_pct = 100.0 * float(different) / float(total)
    return f"Exact {exact_pct:.1f}% | Similar {similar_pct:.1f}% | Different {diff_pct:.1f}%"


@dataclass
class ScanStats:
    total_images: int = 0
    processed_images: int = 0
    base_images: int = 0
    deduped_images: int = 0
    failed_images: int = 0
    original_total_size: int = 0
    processed_original_size: int = 0
    dedup_total_size: int = 0
    elapsed_seconds: float = 0.0

    def to_dict(self) -> Dict[str, Any]:
        processed = float(self.processed_original_size)
        ratio = (processed / float(self.dedup_total_size)) if self.dedup_total_size > 0 else 0.0
        savings = (
            (1.0 - (float(self.dedup_total_size) / processed)) * 100.0
            if processed > 0
            else 0.0
        )
        saved_bytes = max(0.0, processed - float(self.dedup_total_size))
        saved_mb = saved_bytes / (1024.0 * 1024.0)
        throughput = (
            (processed / (1024.0 * 1024.0)) / self.elapsed_seconds
            if self.elapsed_seconds > 0.0
            else 0.0
        )
        return {
            "total_images": self.total_images,
            "processed_images": self.processed_images,
            "base_images": self.base_images,
            "deduped_images": self.deduped_images,
            "failed_images": self.failed_images,
            "original_total_size": self.original_total_size,
            "processed_original_size": self.processed_original_size,
            "dedup_total_size": self.dedup_total_size,
            "elapsed_seconds": self.elapsed_seconds,
            "compression_ratio": ratio,
            "savings_percent": savings,
            "saved_mb": saved_mb,
            "avg_throughput_mb_s": throughput,
        }


class GlobalDedupEngine:
    def __init__(
        self,
        vote_threshold: int = 2,
        min_compression_ratio: float = 1.05,
        force_dedup: bool = False,
        output_dir: Optional[str] = None,
        window_size: int = 2,
        num_features: int = 10,
        strict_lossless: bool = True,
        routing_mode: Optional[Any] = None,
    ) -> None:
        self.feature_index: Dict[int, List[str]] = {}
        self.dependencies: Dict[str, str] = {}
        self.manifest: Dict[str, Dict[str, str]] = {}
        self.ddct_files: List[str] = []
        self.ddctpack_path: Optional[str] = None
        self.vote_threshold = max(1, int(vote_threshold))
        self.min_compression_ratio = float(min_compression_ratio)
        self.force_dedup = bool(force_dedup)
        self.window_size = max(1, int(window_size))
        self.num_features = max(1, int(num_features))
        self.strict_lossless = bool(strict_lossless)
        self.routing_mode: Optional[Any] = routing_mode
        self.output_dir = os.path.abspath(output_dir) if output_dir else None
        self.root_dir: Optional[str] = None
        self.output_root: Optional[str] = None
        self.sequence = 0

    def _scan_jpeg_files(self, dir_path: str) -> List[str]:
        jpeg_paths: List[str] = []
        for root, _, files in os.walk(dir_path):
            for name in files:
                lower = name.lower()
                if lower.endswith(".jpg") or lower.endswith(".jpeg"):
                    jpeg_paths.append(os.path.join(root, name))
        jpeg_paths.sort()
        return jpeg_paths

    @staticmethod
    def _safe_getsize(path: str) -> int:
        try:
            return int(os.path.getsize(path))
        except OSError:
            return 0

    @staticmethod
    def _safe_remove(path: str) -> None:
        try:
            os.remove(path)
        except OSError:
            return

    @staticmethod
    def _sha256_file(path: str) -> str:
        try:
            hasher = hashlib.sha256()
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(1024 * 1024), b""):
                    hasher.update(chunk)
            return hasher.hexdigest()
        except OSError:
            return ""

    def _register_features(self, img_path: str, features: List[int]) -> None:
        for feature in features:
            self.feature_index.setdefault(int(feature), []).append(img_path)

    def _write_ddctpack(self) -> Optional[str]:
        if not self.root_dir or not self.output_root or not self.ddct_files:
            return None

        pack_name = f"{Path(self.root_dir).name}_deduped.ddctpack"
        pack_path = os.path.join(self.output_root, pack_name)

        base_files: Dict[str, str] = {}
        for ddct_path, base_path in self.dependencies.items():
            if not os.path.isfile(ddct_path) or not os.path.isfile(base_path):
                continue
            base_abs = os.path.abspath(base_path)
            if base_abs in base_files:
                continue
            rel_base = os.path.relpath(base_abs, self.root_dir)
            if rel_base.startswith(".."):
                rel_base = os.path.basename(base_abs)
            base_files[base_abs] = os.path.join("bases", rel_base)

        ddct_files: Dict[str, str] = {}
        for ddct_path in self.ddct_files:
            if not os.path.isfile(ddct_path):
                continue
            ddct_files[os.path.abspath(ddct_path)] = os.path.join(
                "deltas",
                os.path.basename(ddct_path),
            )

        manifest_json = json.dumps(self.manifest, ensure_ascii=False, indent=2)
        with zipfile.ZipFile(pack_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("manifest.json", manifest_json)
            for base_path, arcname in sorted(base_files.items(), key=lambda item: item[1]):
                zf.write(base_path, arcname=arcname)
            for ddct_path, arcname in sorted(ddct_files.items(), key=lambda item: item[1]):
                zf.write(ddct_path, arcname=arcname)

        for ddct_path in list(ddct_files.keys()):
            self._safe_remove(ddct_path)
        self.ddct_files = []
        self.ddctpack_path = pack_path
        return pack_path

    def _make_output_ddct_path(self, target_path: str) -> str:
        if self.root_dir is None:
            self.root_dir = os.path.dirname(os.path.abspath(target_path))
        if self.output_root is None:
            parent_dir = os.path.dirname(self.root_dir)
            self.output_root = os.path.join(
                parent_dir,
                "DeltaDCT_Output",
                Path(self.root_dir).name,
            )
        if self.output_dir is None:
            self.output_dir = os.path.join(self.output_root, "ddct")
        os.makedirs(self.output_dir, exist_ok=True)

        rel = os.path.relpath(target_path, self.root_dir)
        rel_no_ext = os.path.splitext(rel)[0]
        safe_rel = rel_no_ext.replace(os.sep, "__")
        self.sequence += 1
        return os.path.join(self.output_dir, f"{safe_rel}.{self.sequence:06d}.ddct")

    def process_image(self, target_img_path: str) -> Dict[str, Any]:

        target_img_path = os.path.abspath(target_img_path)
        original_size = self._safe_getsize(target_img_path)

        try:
            features = pydeltadct.extract_features(
                target_img_path,
                self.window_size,
                self.num_features,
            )
        except Exception as exc:
            return {
                "status": "failed",
                "image": target_img_path,
                "reason": f"extract_features failed: {exc}",
                "original_size": original_size,
                "compressed_size": original_size,
                "vote_count": 0,
                "Block_Similarity": "",
            }

        unique_features = list(dict.fromkeys(int(feature) for feature in features))
        if os.getenv("DELTADCT_DEBUG_FEATURES") == "1":
            print(f"[features] {target_img_path}: {unique_features}")

        votes: Dict[str, int] = {}
        for feature in unique_features:
            for candidate_path in self.feature_index.get(feature, []):
                votes[candidate_path] = votes.get(candidate_path, 0) + 1

        best_base: Optional[str] = None
        max_votes = 0
        if votes:
            best_base, max_votes = max(votes.items(), key=lambda item: item[1])

        if best_base is not None and max_votes >= self.vote_threshold:
            output_ddct = self._make_output_ddct_path(target_img_path)
            try:
                dedup_result = pydeltadct.compress_image(
                    target_img_path,
                    best_base,
                    output_ddct,
                    self.routing_mode if self.routing_mode is not None else pydeltadct.RoutingMode.ROUTING_AUTO,
                    self.window_size,
                    self.num_features,
                    self.strict_lossless,
                )
            except Exception as exc:
                self._register_features(target_img_path, unique_features)
                return {
                    "status": "new_base",
                    "image": target_img_path,
                    "base": best_base,
                    "output_ddct": "",
                    "compression_ratio": 0.0,
                    "vote_count": max_votes,
                    "reason": f"compress_image failed: {exc}",
                    "original_size": original_size,
                    "compressed_size": original_size,
                    "Block_Similarity": "",
                }

            compressed_size = self._safe_getsize(output_ddct)
            ratio = float(getattr(dedup_result, "compression_ratio", 0.0))
            if ratio <= 0.0 and compressed_size > 0:
                ratio = float(original_size) / float(compressed_size)

            blocks_total = int(getattr(dedup_result, "blocks_total", 0))
            blocks_exact = int(getattr(dedup_result, "blocks_exact", 0))
            blocks_similar = int(getattr(dedup_result, "blocks_similar", 0))
            blocks_different = int(getattr(dedup_result, "blocks_different", 0))
            blocks_width = int(getattr(dedup_result, "blocks_width", 0))
            blocks_height = int(getattr(dedup_result, "blocks_height", 0))
            header_bytes = int(getattr(dedup_result, "header_bytes", 0))
            copy_meta_bytes = int(getattr(dedup_result, "copy_meta_bytes", 0))
            insert_raw_bytes = int(getattr(dedup_result, "insert_raw_bytes", 0))
            block_map = list(getattr(dedup_result, "block_map", []))
            block_similarity = _format_block_similarity(
                blocks_exact,
                blocks_similar,
                blocks_different,
                blocks_total,
            )

            ratio_ok = ratio > self.min_compression_ratio
            # When force_dedup is enabled, accept .ddct results regardless of small expansions
            force_ok = self.force_dedup

            if dedup_result.success and compressed_size > 0 and (ratio_ok or force_ok):
                target_sha256 = self._sha256_file(target_img_path)
                target_rel = (
                    os.path.relpath(target_img_path, self.root_dir)
                    if self.root_dir
                    else os.path.basename(target_img_path)
                )
                base_rel = (
                    os.path.relpath(best_base, self.root_dir)
                    if self.root_dir
                    else os.path.basename(best_base)
                )
                if base_rel.startswith(".."):
                    base_rel = os.path.basename(best_base)
                ddct_name = os.path.basename(output_ddct)
                ddct_rel = os.path.join("deltas", ddct_name)
                base_archive_rel = os.path.join("bases", base_rel)

                self.manifest[target_rel] = {
                    "ddct_file": ddct_rel,
                    "base_file": base_archive_rel,
                    "original_sha256": target_sha256,
                    "strict_lossless": self.strict_lossless,
                }
                self.ddct_files.append(output_ddct)
                self.dependencies[os.path.abspath(output_ddct)] = os.path.abspath(best_base)
                return {
                    "status": "deduped",
                    "image": target_img_path,
                    "base": best_base,
                    "output_ddct": output_ddct,
                    "compression_ratio": ratio,
                    "vote_count": max_votes,
                    "reason": "forced_dedup" if force_ok and not ratio_ok else "dedup_success",
                    "original_size": original_size,
                    "compressed_size": compressed_size,
                    "Block_Similarity": block_similarity,
                    "blocks_total": blocks_total,
                    "blocks_exact": blocks_exact,
                    "blocks_similar": blocks_similar,
                    "blocks_different": blocks_different,
                    "block_width": blocks_width,
                    "block_height": blocks_height,
                    "header_bytes": header_bytes,
                    "copy_meta_bytes": copy_meta_bytes,
                    "insert_raw_bytes": insert_raw_bytes,
                    "block_map": block_map,
                }

            self._safe_remove(output_ddct)
            self._register_features(target_img_path, unique_features)

            if not self.force_dedup and ratio < 1.0:
                return {
                    "status": "kept_original",
                    "image": target_img_path,
                    "base": best_base,
                    "output_ddct": "",
                    "compression_ratio": ratio,
                    "vote_count": max_votes,
                    "reason": "redundancy_low_keep_original",
                    "original_size": original_size,
                    "compressed_size": original_size,
                    "Block_Similarity": block_similarity,
                    "blocks_total": blocks_total,
                    "blocks_exact": blocks_exact,
                    "blocks_similar": blocks_similar,
                    "blocks_different": blocks_different,
                    "block_width": blocks_width,
                    "block_height": blocks_height,
                    "header_bytes": header_bytes,
                    "copy_meta_bytes": copy_meta_bytes,
                    "insert_raw_bytes": insert_raw_bytes,
                    "block_map": block_map,
                }

            return {
                "status": "new_base",
                "image": target_img_path,
                "base": best_base,
                "output_ddct": "",
                "compression_ratio": ratio,
                "vote_count": max_votes,
                "reason": "low_ratio_or_failed",
                "original_size": original_size,
                "compressed_size": original_size,
                "Block_Similarity": block_similarity,
                "blocks_total": blocks_total,
                "blocks_exact": blocks_exact,
                "blocks_similar": blocks_similar,
                "blocks_different": blocks_different,
                "block_width": blocks_width,
                "block_height": blocks_height,
                "header_bytes": header_bytes,
                "copy_meta_bytes": copy_meta_bytes,
                "insert_raw_bytes": insert_raw_bytes,
                "block_map": block_map,
            }

        self._register_features(target_img_path, unique_features)
        return {
            "status": "new_base",
            "image": target_img_path,
            "base": "",
            "output_ddct": "",
            "compression_ratio": 0.0,
            "vote_count": max_votes,
            "reason": "no_match",
            "original_size": original_size,
            "compressed_size": original_size,
            "Block_Similarity": "",
            "header_bytes": 0,
            "copy_meta_bytes": 0,
            "insert_raw_bytes": 0,
        }

    def scan_directory(self, dir_path: str) -> Generator[Dict[str, Any], None, None]:
        if not os.path.isdir(dir_path):
            raise ValueError(f"dir_path is not a directory: {dir_path}")

        self.feature_index = {}
        self.dependencies = {}
        self.sequence = 0
        self.manifest = {}
        self.ddct_files = []
        self.ddctpack_path = None

        self.root_dir = os.path.abspath(dir_path)
        base_output_root = getattr(self, "output_root_base", None)
        if base_output_root:
            self.output_root = os.path.join(os.path.abspath(base_output_root), Path(self.root_dir).name)
        else:
            parent_dir = os.path.dirname(self.root_dir)
            self.output_root = os.path.join(parent_dir, "DeltaDCT_Output", Path(self.root_dir).name)
        if self.output_dir is None:
            self.output_dir = os.path.join(self.output_root, "ddct")
        os.makedirs(self.output_dir, exist_ok=True)

        images = self._scan_jpeg_files(self.root_dir)
        stats = ScanStats(total_images=len(images))
        stats.original_total_size = sum(self._safe_getsize(p) for p in images)
        stats.processed_original_size = 0

        begin = time.perf_counter()
        yield {
            "event": "start",
            "folder": self.root_dir,
            "stats": stats.to_dict(),
        }

        for idx, img_path in enumerate(images, start=1):
            result = self.process_image(img_path)

            stats.processed_images = idx
            stats.elapsed_seconds = time.perf_counter() - begin

            status = result.get("status")
            original_size = int(result.get("original_size", 0))
            compressed_size = int(result.get("compressed_size", original_size))
            stats.processed_original_size += original_size

            if status == "deduped":
                stats.deduped_images += 1
                stats.dedup_total_size += compressed_size
            elif status in {"new_base", "kept_original"}:
                stats.base_images += 1
                stats.dedup_total_size += original_size
            else:
                stats.failed_images += 1
                stats.dedup_total_size += original_size

            yield {
                "event": "progress",
                "image": img_path,
                "result": result,
                "stats": stats.to_dict(),
            }

        stats.elapsed_seconds = time.perf_counter() - begin
        ddctpack_path = self._write_ddctpack()
        yield {
            "event": "done",
            "folder": self.root_dir,
            "stats": stats.to_dict(),
            "dependencies": dict(self.dependencies),
            "ddctpack_path": ddctpack_path,
            "output_root": self.output_root,
        }
