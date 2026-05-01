from __future__ import annotations

import atexit
import hashlib
import json
import os
import shutil
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Dict, List, Optional

import streamlit as st

from deltadct_manager import GlobalDedupEngine, pydeltadct

DEFAULT_OUTPUT_ROOT = os.path.abspath("DeltaDCT_Output")

st.set_page_config(
    page_title="DeltaDCT Control Room",
    page_icon="📦",
    layout="wide",
)

st.markdown(
    """
<style>
@import url('https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;600;700&family=IBM+Plex+Sans:wght@400;600;700&display=swap');

html, body, [class*="css"] {
    font-family: 'Space Grotesk', 'IBM Plex Sans', sans-serif;
}

.main {
    background:
        radial-gradient(circle at 15% 20%, rgba(255, 214, 171, 0.35), transparent 45%),
        radial-gradient(circle at 85% 10%, rgba(143, 230, 224, 0.35), transparent 40%),
        linear-gradient(180deg, #f7fbff 0%, #fefcf9 100%);
}
.block-container {
    padding-top: 1.2rem;
    padding-bottom: 2rem;
}
.panel {
    border: 1px solid rgba(19, 78, 124, 0.15);
    border-radius: 18px;
    padding: 1rem 1.2rem;
    background: rgba(255, 255, 255, 0.9);
    box-shadow: 0 18px 40px rgba(22, 63, 84, 0.12);
}
.metric-card {
    border: 1px solid rgba(15, 76, 129, 0.12);
    border-radius: 14px;
    padding: 0.6rem 0.8rem;
    background: rgba(255, 255, 255, 0.95);
    box-shadow: 0 8px 24px rgba(28, 74, 92, 0.08);
}
</style>
""",
    unsafe_allow_html=True,
)

st.markdown("## DeltaDCT · Real-World Dynamic Dedup Control Room")
st.caption("动态投票 + 倒排索引驱动的全目录去重与即时还原验证。")

if "scan_logs" not in st.session_state:
    st.session_state.scan_logs = []
if "scan_stats" not in st.session_state:
    st.session_state.scan_stats = {
        "total_images": 0,
        "processed_images": 0,
        "base_images": 0,
        "deduped_images": 0,
        "failed_images": 0,
        "original_total_size": 0,
        "processed_original_size": 0,
        "dedup_total_size": 0,
        "compression_ratio": 0.0,
        "savings_percent": 0.0,
        "saved_mb": 0.0,
        "avg_throughput_mb_s": 0.0,
        "elapsed_seconds": 0.0,
    }
if "dependencies" not in st.session_state:
    st.session_state.dependencies = {}
if "block_maps" not in st.session_state:
    st.session_state.block_maps = {}
if "engine" not in st.session_state:
    st.session_state.engine = None
if "last_scan_dir" not in st.session_state:
    st.session_state.last_scan_dir = ""
if "ddctpack_path" not in st.session_state:
    st.session_state.ddctpack_path = ""
if "output_root" not in st.session_state:
    st.session_state.output_root = ""
if "output_root_input" not in st.session_state:
    st.session_state.output_root_input = ""
if "preview_dir" not in st.session_state:
    st.session_state.preview_dir = ""
if "strict_lossless" not in st.session_state:
    st.session_state.strict_lossless = True
if "routing_mode" not in st.session_state:
    st.session_state.routing_mode = None


def _render_metrics(stats: Dict[str, Any], container: Any) -> None:
    total = int(stats.get("total_images", 0))
    processed = int(stats.get("processed_images", 0))
    base_images = int(stats.get("base_images", 0))
    deduped_images = int(stats.get("deduped_images", 0))
    savings = float(stats.get("savings_percent", 0.0))
    throughput = float(stats.get("avg_throughput_mb_s", 0.0))
    elapsed = float(stats.get("elapsed_seconds", 0.0))
    saved_mb = float(stats.get("saved_mb", 0.0))

    with container.container():
        st.markdown('<div class="panel">', unsafe_allow_html=True)
        columns = st.columns(7)
        labels = [
            ("扫描进度", f"{processed}/{total}"),
            ("独立 Base 数量", f"{base_images}"),
            ("增量压缩数量", f"{deduped_images}"),
            ("总体节省率", f"{savings:.2f}%"),
            ("平均吞吐量", f"{throughput:.2f} MB/s"),
            ("总耗时", f"{elapsed:.1f} s"),
            ("实际节省空间", f"{saved_mb:.2f} MB"),
        ]
        for slot, (name, value) in zip(columns, labels):
            with slot:
                st.markdown('<div class="metric-card">', unsafe_allow_html=True)
                st.metric(name, value)
                st.markdown("</div>", unsafe_allow_html=True)
        st.markdown("</div>", unsafe_allow_html=True)


def _sanitize_dataframe_rows(rows: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    int_columns = {
        "blocks_total",
        "blocks_exact",
        "blocks_similar",
        "blocks_different",
    }
    sanitized: List[Dict[str, Any]] = []
    for row in rows:
        cleaned: Dict[str, Any] = {}
        for key, value in row.items():
            if value is None or value == "None":
                value = 0
            if key in int_columns:
                try:
                    value = int(value)
                except (TypeError, ValueError):
                    value = 0
            cleaned[key] = value
        for key in int_columns:
            if key in cleaned:
                try:
                    cleaned[key] = int(cleaned[key])
                except (TypeError, ValueError):
                    cleaned[key] = 0
        sanitized.append(cleaned)
    return sanitized


def _resolve_output_root(user_value: str) -> tuple[str, bool, str]:
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
        return DEFAULT_OUTPUT_ROOT, True, (
            f"无法创建指定输出根目录，已回退到默认路径: {DEFAULT_OUTPUT_ROOT}"
        )


def _render_log_table(rows: List[Dict[str, Any]], container: Any) -> None:
    with container.container():
        st.markdown("### 📊 处理日志")
        if not rows:
            st.info("尚无处理记录，启动扫描后将实时更新。")
        else:
            columns = [
                "status",
                "image",
                "base",
                "output_ddct",
                "compression_ratio",
                "vote_count",
                "header_bytes",
                "copy_meta_bytes",
                "insert_raw_bytes",
                "reason",
                "original_size",
                "compressed_size",
                "Block_Similarity",
                "blocks_total",
                "blocks_exact",
                "blocks_similar",
                "blocks_different",
                "block_width",
                "block_height",
            ]
            display_rows = _sanitize_dataframe_rows(
                [{key: row.get(key, 0) for key in columns} for row in rows]
            )
            st.dataframe(display_rows, width="stretch", height=320)


def _restore_ddct(ddct_path: str, base_path: str, output_dir: str) -> Optional[str]:
    if not os.path.isfile(ddct_path) or not os.path.isfile(base_path):
        return None

    os.makedirs(output_dir, exist_ok=True)
    name = Path(ddct_path).stem
    output_path = os.path.join(output_dir, f"{name}.restored.jpg")
    if os.path.isfile(output_path):
        return output_path

    ok = pydeltadct.decompress_image(ddct_path, base_path, output_path)
    return output_path if ok and os.path.isfile(output_path) else None


def _sha256_file(path: str) -> str:
    try:
        hasher = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                hasher.update(chunk)
        return hasher.hexdigest()
    except OSError:
        return ""


def _md5_file(path: str) -> str:
    try:
        hasher = hashlib.md5()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                hasher.update(chunk)
        return hasher.hexdigest()
    except OSError:
        return ""


def _cleanup_preview_dir() -> None:
    preview_dir = st.session_state.get("preview_dir", "")
    if preview_dir and os.path.isdir(preview_dir):
        shutil.rmtree(preview_dir, ignore_errors=True)
    st.session_state.preview_dir = ""


atexit.register(_cleanup_preview_dir)


def _ensure_preview_dir() -> str:
    preview_dir = st.session_state.get("preview_dir", "")
    if not preview_dir or not os.path.isdir(preview_dir):
        st.session_state.preview_dir = tempfile.mkdtemp(prefix="deltadct_preview_")
    return st.session_state.preview_dir


def _resolve_ddct_for_preview(ddct_path: str, ddctpack_path: str, preview_dir: str) -> str:
    if os.path.isfile(ddct_path):
        return ddct_path
    if not ddctpack_path or not os.path.isfile(ddctpack_path):
        return ""

    ddct_name = os.path.basename(ddct_path)
    try:
        with zipfile.ZipFile(ddctpack_path, "r") as zf:
            names = set(zf.namelist())
            manifest = {}
            if "manifest.json" in names:
                manifest = json.loads(zf.read("manifest.json").decode("utf-8"))

            archive_ddct = ""
            if isinstance(manifest, dict):
                for info in manifest.values():
                    if not isinstance(info, dict):
                        continue
                    candidate = str(info.get("ddct_file", ""))
                    if os.path.basename(candidate) == ddct_name:
                        archive_ddct = candidate
                        break
            if not archive_ddct and ddct_name in names:
                archive_ddct = ddct_name

            if archive_ddct:
                extracted_path = zf.extract(archive_ddct, path=preview_dir)
                return extracted_path if os.path.isfile(extracted_path) else ""
    except Exception:
        return ""

    extracted_path = os.path.join(preview_dir, ddct_name)
    return extracted_path if os.path.isfile(extracted_path) else ""


def _load_ddctpack_manifest(ddctpack_path: str) -> Dict[str, Any]:
    with zipfile.ZipFile(ddctpack_path, "r") as zf:
        if "manifest.json" not in zf.namelist():
            raise ValueError("ddctpack 缺少 manifest.json。")
        payload = zf.read("manifest.json").decode("utf-8")
    data = json.loads(payload)
    if not isinstance(data, dict):
        raise ValueError("manifest.json 格式不正确。")
    return data


def _extract_pack_entry(zf: zipfile.ZipFile, entry_name: str, temp_dir: str) -> str:
    if not entry_name:
        return ""
    names = set(zf.namelist())
    if entry_name not in names:
        return ""
    extracted_path = zf.extract(entry_name, path=temp_dir)
    return extracted_path if os.path.isfile(extracted_path) else ""


def _compute_pixel_mse(path_a: str, path_b: str) -> Optional[float]:
    try:
        from PIL import Image, ImageChops
    except Exception:
        return None

    try:
        img_a = Image.open(path_a).convert("RGB")
        img_b = Image.open(path_b).convert("RGB")
    except Exception:
        return None

    if img_a.size != img_b.size:
        return float("inf")

    diff = ImageChops.difference(img_a, img_b)
    if diff.getbbox() is None:
        return 0.0

    hist = diff.histogram()
    total = img_a.size[0] * img_a.size[1] * 3
    if total == 0:
        return 0.0
    sq_sum = 0.0
    for idx, value in enumerate(hist):
        sq_sum += value * float((idx % 256) ** 2)
    return sq_sum / float(total)


def _find_log_by_ddct(rows: List[Dict[str, Any]], ddct_path: str) -> Optional[Dict[str, Any]]:
    for row in reversed(rows):
        if row.get("output_ddct") == ddct_path:
            return row
    return None


def _build_block_heatmap(
    block_map: List[int],
    width: int,
    height: int,
) -> Optional[Any]:
    try:
        from PIL import Image, ImageDraw
    except Exception:
        return None

    if width <= 0 or height <= 0:
        return None
    if len(block_map) < width * height:
        return None

    scale = 6 if max(width, height) <= 96 else 3
    image = Image.new("RGB", (width * scale, height * scale), (245, 245, 245))
    draw = ImageDraw.Draw(image)

    idx = 0
    for row in range(height):
        for col in range(width):
            value = block_map[idx]
            idx += 1
            color = (27, 170, 111) if value == 1 else (227, 81, 93)
            x0 = col * scale
            y0 = row * scale
            x1 = x0 + scale - 1
            y1 = y0 + scale - 1
            draw.rectangle([x0, y0, x1, y1], fill=color)

    return image


def _render_pair_viewer(dependencies: Dict[str, str], container: Any) -> None:
    with container.container():
        st.markdown("### 🧭 Dynamic Pair Viewer")
        if not dependencies:
            st.info("尚未发现可展示的 .ddct 配对。")
            return

        options = sorted(dependencies.keys())
        selected = st.selectbox(
            "选择一个 .ddct 结果",
            options,
            key="ddct_viewer_selectbox",
        )
        base_path = dependencies.get(selected, "")

        if not base_path:
            st.warning("找不到该 .ddct 的 Base 依赖。")
            return

        preview_dir = _ensure_preview_dir()
        resolved_ddct = _resolve_ddct_for_preview(
            selected,
            st.session_state.ddctpack_path,
            preview_dir,
        )
        if not resolved_ddct:
            st.warning("找不到对应的 .ddct 文件（可能已打包）。")
            return

        restored_path = _restore_ddct(resolved_ddct, base_path, preview_dir)

        log_entry = _find_log_by_ddct(st.session_state.scan_logs, selected)
        block_similarity = log_entry.get("Block_Similarity", "") if log_entry else ""
        map_entry = st.session_state.block_maps.get(selected, {})
        block_map = map_entry.get("block_map", [])
        block_width = int(map_entry.get("block_width", 0))
        block_height = int(map_entry.get("block_height", 0))

        col1, col2 = st.columns(2)
        with col1:
            st.image(base_path, caption="Base 图像", width="stretch")
        with col2:
            if restored_path:
                st.image(restored_path, caption="还原后的 Target", width="stretch")
            else:
                st.warning("还原失败或输出不存在。")

        if block_similarity:
            st.markdown(f"**Block_Similarity:** {block_similarity}")

        heatmap = _build_block_heatmap(block_map, block_width, block_height)
        if heatmap is not None:
            st.image(heatmap, caption="Block Layout Heatmap", width="stretch")
        else:
            st.info("热力图不可用（缺少 block_map 或 Pillow）。")


with st.sidebar:
    st.markdown("### 去重参数")
    vote_threshold = st.slider(
        "投票阈值 (>=2)",
        1,
        10,
        2,
        help=(
            "候选 Base 累计到多少票才会触发尝试压缩。"
            "值越高越保守，误匹配更少但去重率可能下降。"
        ),
    )
    force_dedup = st.toggle(
        "强制去重",
        value=False,
        help=(
            "即使压缩比 < 1.0 也保留 .ddct 结果。"
            "谨慎用于大批量扫描。"
        ),
    )
    strict_lossless = st.toggle(
        "严格无损",
        value=True,
        help="仅当 DCT 系数完全一致时才允许 COPY，确保像素级无损还原。",
    )
    routing_mode = st.selectbox(
        "🚦 路由模式",
        ["auto", "fpm", "dchash"],
        format_func=lambda x: {"auto": "自动路由", "fpm": "强制 FPM", "dchash": "强制 DCHash"}[x],
        help=(
            "auto: 根据块相似度自动选择最优路由\n"
            "fpm: 强制使用 FPM 编码（高对齐场景优化）\n"
            "dchash: 强制使用 DCHash 编码（低对齐/容错能力强）"
        ),
    )
    output_root_input = st.text_input(
        "输出根目录",
        value=st.session_state.output_root_input,
        help="为空或不可创建时会自动回退到 ./DeltaDCT_Output。",
    ).strip()
    resolved_output_root, used_default_output_root, output_root_message = _resolve_output_root(
        output_root_input
    )
    st.session_state.output_root_input = output_root_input
    if used_default_output_root:
        st.warning(output_root_message)
    else:
        st.info(output_root_message)
    st.caption("推荐：投票阈值设为 2 或 3，可平衡速度与检出率。")
    st.caption("推荐：强制去重默认关闭，避免负压缩占用磁盘。")

tab_scan, tab_batch = st.tabs(
    ["🚀 全局智能去重", "📦 归档管理与一键还原"]
)

with tab_scan:
    st.markdown("### 扫描与动态投票")
    st.info(f"本次扫描的输出根目录: {resolved_output_root}")
    scan_col, config_col = st.columns([3, 2])
    with scan_col:
        folder = st.text_input(
            "待扫描目录（绝对路径）",
            value=st.session_state.last_scan_dir,
        )
    with config_col:
        start_scan = st.button("开始扫描", type="primary", width="stretch")

    metrics_placeholder = st.empty()
    progress_placeholder = st.empty()
    status_placeholder = st.empty()
    log_placeholder = st.empty()
    viewer_placeholder = st.empty()

    _render_metrics(st.session_state.scan_stats, metrics_placeholder)
    _render_log_table(st.session_state.scan_logs, log_placeholder)

    if start_scan:
        folder = folder.strip()
        st.session_state.last_scan_dir = folder
        if not folder:
            status_placeholder.error("请输入目标目录路径。")
        elif not os.path.isabs(folder):
            status_placeholder.error("请输入绝对路径，以避免扫描范围歧义。")
        elif not os.path.isdir(folder):
            status_placeholder.error("输入路径不存在或不是目录。")
        else:
            _cleanup_preview_dir()
            st.session_state.scan_logs = []
            st.session_state.dependencies = {}
            st.session_state.block_maps = {}
            st.session_state.ddctpack_path = ""
            st.session_state.output_root = ""
            st.session_state.strict_lossless = strict_lossless
            st.session_state.output_root = resolved_output_root
            routing_mode_enum = None
            if routing_mode != "auto":
                mode_map = {
                    "fpm": pydeltadct.RoutingMode.FPM_ONLY,
                    "dchash": pydeltadct.RoutingMode.DCHASH_ONLY,
                }
                routing_mode_enum = mode_map.get(routing_mode)
            engine = GlobalDedupEngine(
                vote_threshold=vote_threshold,
                force_dedup=force_dedup,
                strict_lossless=strict_lossless,
                routing_mode=routing_mode_enum,
            )
            engine.output_root_base = resolved_output_root
            st.session_state.engine = engine
            progress_bar = progress_placeholder.progress(0.0)

            try:
                for event in engine.scan_directory(folder):
                    stats = event.get("stats", {})
                    st.session_state.scan_stats = stats
                    result = event.get("result")
                    if result is not None:
                        output_ddct = result.get("output_ddct")
                        block_map = result.get("block_map")
                        if output_ddct and block_map:
                            st.session_state.block_maps[output_ddct] = {
                                "block_map": block_map,
                                "block_width": result.get("block_width", 0),
                                "block_height": result.get("block_height", 0),
                            }
                        log_row = {
                            key: value
                            for key, value in result.items()
                            if key not in {"block_map", "block_width", "block_height"}
                        }
                        st.session_state.scan_logs.append(log_row)
                    st.session_state.dependencies = dict(engine.dependencies)

                    total = max(1, int(stats.get("total_images", 0)))
                    processed = int(stats.get("processed_images", 0))
                    progress_bar.progress(min(1.0, float(processed) / float(total)))

                    _render_metrics(stats, metrics_placeholder)
                    _render_log_table(st.session_state.scan_logs, log_placeholder)

                    if result is not None and result.get("status") == "kept_original":
                        status_placeholder.warning("该图组冗余度不足，已保持原样。")
                    elif event.get("event") == "done":
                        ddctpack_path = event.get("ddctpack_path") or ""
                        output_root = event.get("output_root") or ""
                        if output_root:
                            st.session_state.output_root = output_root
                        if ddctpack_path:
                            st.session_state.ddctpack_path = ddctpack_path
                            status_placeholder.success(
                                "扫描完成，已生成 ddctpack。"
                            )
                            if output_root:
                                status_placeholder.info(f"输出目录: {output_root}")
                        else:
                            status_placeholder.success("扫描完成，已生成动态去重映射。")
                    else:
                        status_placeholder.info(f"正在处理: {event.get('image', '')}")

            except RuntimeError as exc:
                status_placeholder.error(f"C++ 运行时异常: {exc}")
            except Exception as exc:
                status_placeholder.error(f"未预期异常: {exc}")

    _render_pair_viewer(st.session_state.dependencies, viewer_placeholder)

with tab_batch:
    st.markdown("### 归档管理与一键还原")
    st.markdown('<div class="panel">', unsafe_allow_html=True)
    ddctpack_path = st.text_input(
        ".ddctpack 文件路径",
        value=st.session_state.ddctpack_path,
    )
    source_root = st.text_input(
        "原始目录 (可选，用于校验)",
        value=st.session_state.last_scan_dir,
    )
    output_dir = st.text_input(
        "输出目录",
        value=os.path.join(st.session_state.output_root or "", "restored"),
    )
    run_batch = st.button("一键还原并校验", type="primary")
    st.markdown("</div>", unsafe_allow_html=True)

    if run_batch:
        if not ddctpack_path or not output_dir:
            st.error("请填写 ddctpack 路径与输出目录。")
        elif not os.path.isfile(ddctpack_path):
            st.error("ddctpack 文件不存在。")
        else:
            try:
                manifest = _load_ddctpack_manifest(ddctpack_path)
                total = len(manifest)
                if total == 0:
                    st.info("manifest 为空，没有可还原的条目。")
                else:
                    source_root = source_root.strip()
                    has_source_root = bool(source_root)
                    if has_source_root and not os.path.isdir(source_root):
                        st.error("原始目录不存在。")
                    else:
                        os.makedirs(output_dir, exist_ok=True)
                        temp_dir = os.path.join(output_dir, "_ddctpack_tmp")
                        os.makedirs(temp_dir, exist_ok=True)
                        progress = st.progress(0.0)

                        restored = 0
                        verified = 0
                        mismatched = 0
                        critical = 0
                        missing_base = 0
                        missing_original = 0
                        failed = 0
                        rows: List[Dict[str, Any]] = []

                        with zipfile.ZipFile(ddctpack_path, "r") as zf:
                            names = set(zf.namelist())
                            for idx, (target_rel, info) in enumerate(manifest.items(), start=1):
                                ddct_name = str(info.get("ddct_file", ""))
                                base_name = str(info.get("base_file", ""))
                                expected_sha = str(info.get("original_sha256", ""))

                                row = {
                                    "target": target_rel,
                                    "ddct_file": ddct_name,
                                    "base": base_name,
                                    "status": "pending",
                                    "sha256": "",
                                    "md5": "",
                                    "mse": "",
                                }

                                ddct_path = _extract_pack_entry(zf, ddct_name, temp_dir)
                                if not ddct_path:
                                    row["status"] = "missing_ddct"
                                    failed += 1
                                    rows.append(row)
                                    progress.progress(min(1.0, idx / float(total)))
                                    continue

                                base_path = _extract_pack_entry(zf, base_name, temp_dir)
                                if not base_path:
                                    row["status"] = "missing_base"
                                    missing_base += 1
                                    rows.append(row)
                                    progress.progress(min(1.0, idx / float(total)))
                                    continue

                                original_path = os.path.join(source_root, target_rel) if has_source_root else ""
                                if has_source_root and not os.path.isfile(original_path):
                                    row["status"] = "missing_original"
                                    missing_original += 1
                                    rows.append(row)
                                    progress.progress(min(1.0, idx / float(total)))
                                    continue

                                output_path = os.path.join(output_dir, target_rel)
                                output_parent = os.path.dirname(output_path)
                                if output_parent:
                                    os.makedirs(output_parent, exist_ok=True)

                                ok = pydeltadct.decompress_image(ddct_path, base_path, output_path)
                                if not ok or not os.path.isfile(output_path):
                                    row["status"] = "restore_failed"
                                    failed += 1
                                    rows.append(row)
                                    progress.progress(min(1.0, idx / float(total)))
                                    continue

                                restored += 1
                                actual_sha = _sha256_file(output_path)
                                actual_md5 = _md5_file(output_path)
                                row["sha256"] = actual_sha
                                row["md5"] = actual_md5

                                mse = None
                                if has_source_root:
                                    mse = _compute_pixel_mse(output_path, original_path)
                                    row["mse"] = "n/a" if mse is None else f"{mse:.6f}"
                                else:
                                    row["mse"] = "n/a"

                                pixel_ok = True if mse is None else (mse == 0.0)
                                sha_match = bool(expected_sha) and actual_sha == expected_sha
                                if has_source_root and strict_lossless and (not pixel_ok or not sha_match):
                                    row["status"] = "critical_failure"
                                    critical += 1
                                elif has_source_root and expected_sha and sha_match:
                                    if pixel_ok:
                                        row["status"] = "verified"
                                        verified += 1
                                    else:
                                        row["status"] = "pixel_mismatch"
                                        mismatched += 1
                                elif has_source_root and expected_sha:
                                    row["status"] = "sha_mismatch"
                                    mismatched += 1
                                else:
                                    row["status"] = "restored"

                                rows.append(row)
                                progress.progress(min(1.0, idx / float(total)))

                        shutil.rmtree(temp_dir, ignore_errors=True)
                        st.success("归档还原与校验完成。")
                        if has_source_root:
                            success_rate = (100.0 * float(verified) / float(total)) if total > 0 else 0.0
                            st.metric("校验成功率", f"{success_rate:.1f}%")
                        st.markdown(
                            f"- 总条目: {total}\n"
                            f"- 已还原: {restored}\n"
                            f"- 校验通过: {verified}\n"
                            f"- 校验失败: {mismatched}\n"
                            f"- 严格无损失败: {critical}\n"
                            f"- Base 缺失: {missing_base}\n"
                            f"- 原图缺失: {missing_original}\n"
                            f"- 还原失败: {failed}"
                        )
                        st.dataframe(_sanitize_dataframe_rows(rows), width="stretch", height=320)
            except Exception as exc:
                st.error(f"批量还原异常: {exc}")
