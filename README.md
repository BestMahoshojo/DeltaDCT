# DeltaDCT 1.0

DeltaDCT 是一个基于 JPEG 频率域 DCT 块级比对的无损图像去重系统。它会把目标图像转换成系数流、生成可回放的增量包，并在恢复阶段重建出与原图一致的 JPEG 输出。当前发布版把扫描、归档、还原都收敛到统一输出根目录，`.ddctpack` 也变成了可自包含搬运的归档卷。

## 主要特性

- DCT 级增量编码，避免依赖整图像素级对比。
- 全通道重构，恢复结果可做到 MSE = 0。
- 自包含 `.ddctpack`，包内同时带 `bases/`、`deltas/` 和 `manifest.json`。
- Streamlit UI 只保留全局扫描和归档还原两个主入口。
- 命令行入口 `main.py` 支持扫描、恢复和基准测试。
- 批量校验脚本支持只从归档内容恢复，不再强制要求原始 Base 目录。

## 构建要求

- C++17 编译器
- CMake 3.18+
- 可访问 GitHub 的网络环境，用于首次 FetchContent 下载 libjpeg-turbo、zstd 和 pybind11
- Python 运行环境

安装 Python 依赖：

```bash
pip install -r requirements.txt
```

构建 C++ 侧：

```bash
bash build.sh
```

说明：C++ 构建已封装在 `build.sh` 中，日常使用无需再手动输入 `cmake -S/-B` 和 `cmake --build`。

可选：编译后自动执行 C++ 测试

```bash
RUN_TESTS=1 bash build.sh
```

## 快速开始

启动 UI：

```bash
streamlit run python/app.py
```

命令行扫描：

```bash
python3 main.py scan --src /path/to/dataset --out /path/to/output
```

命令行还原：

```bash
python3 main.py restore --pack /path/to/archive.ddctpack --out /path/to/restored
```

命令行基准：

```bash
python3 main.py benchmark --dir /path/to/benchmark_dataset
```

归档完整性校验：

```bash
python3 python/verify_integrity.py /path/to/archive.ddctpack /path/to/output_dir --source-root /path/to/source_root
```

如果不提供 `--source-root`，脚本会直接根据 `.ddctpack` 内部内容恢复并生成报告。

## 数据集生成

发布版保留了实验数据生成脚本：

```bash
python3 python/build_dataset.py
```

它会根据源图和 Logo 目录生成水印、裁剪错位和画质漂移三类实验集，默认输出到 `/home/daiyi/data/experiment_datasets`。

Stress-Mixed 已升级为三档复合扰动强度：

- `4_Stress_Mixed_Light`：16px 平移 + 2 个 Logo
- `4_Stress_Mixed_Medium`：32px 平移 + 3 个 Logo
- `4_Stress_Mixed_Heavy`：48px 平移 + 5 个 Logo

这三档用于验证在“坐标错位 + 局部篡改”同时存在时，系统的鲁棒性退化曲线。

数据集来源说明：

- 图像源数据选取自 VOC2012 训练集中的 JPEG 图片。
- Logo 集合来自 iconfont，上面随机挑选了 40 张图片。
- 下载链接：`[https://pan.baidu.com/s/1h9fe3Dg5y_wcWCjPUrnq4Q?pwd=cnm2]`

## `.ddctpack` 结构

`.ddctpack` 本质上是一个 ZIP 包，内部结构如下：

- `manifest.json`：记录每个目标文件对应的 Base、Delta 和原始哈希信息。
- `bases/`：本批次实际引用到的 Base JPEG。
- `deltas/`：所有生成的 `.ddct` 增量文件。

每个 `.ddct` 中保存了头部信息、系数增量指令和恢复所需的元数据，所以归档可以独立迁移，不再依赖外部目录树。

## 实验分析

`DeltaDCT Ratio` 是 pair-wise 指标，只衡量单个 target 和它对应的 base 之间，目标图像本身被压缩成 `.ddct` 后能获得多少倍率收益。它适合看“单对样本”的系数流压缩效果。

`System_Wide_Saving_Pct` 是 system-wide 指标，衡量整个批次在真实落盘时的总体节省率，也就是 `Total_Original_Size / Total_Archive_Size` 推导出的结果。这里的 `Total_Archive_Size` 包含了所有去重后的 Base 文件总和，以及所有生成的 `.ddct` 文件总和，因此它反映的是“整批数据最终能省多少空间”，而不是某一对样本的局部收益。

- 当前仓库的 `benchmark_data/benchmark_results.csv` 会同时输出 `DeltaDCT Ratio` 和 `System_Wide_Saving_Pct`，前者看 pair-wise 压缩倍率，后者看批量归档后的系统级节省率。
- release 重测里，水印与裁剪类数据集的 pair-wise 压缩率可以达到 162x 级别，但系统级节省率会受到 Base 体积与批量归档开销影响，因此数值会明显低于单对样本倍率。

这两个指标不能直接互相替代：前者用于评价算法对单个样本对的压缩能力，后者用于评价整套归档方案的真实存储收益。

## 项目结构

```text
DeltaDCT/
├── CMakeLists.txt
├── build.sh
├── main.py
├── README.md
├── requirements.txt
├── bindings/
│   └── bindings.cpp
├── include/
│   ├── deltadct.h
│   ├── phase1_dct_codec.hpp
│   ├── phase2_similarity.hpp
│   ├── phase3_delta.hpp
│   ├── phase4_storage.hpp
│   ├── phase5_restore.hpp
│   └── phase6_benchmark.hpp
├── src/
│   ├── deltadct.cpp
│   ├── main.cpp
│   ├── phase1_dct_codec.cpp
│   ├── phase2_similarity.cpp
│   ├── phase3_delta.cpp
│   ├── phase4_storage.cpp
│   ├── phase5_restore.cpp
│   └── phase6_benchmark.cpp
├── python/
│   ├── app.py
│   ├── benchmark_runner.py
│   ├── build_dataset.py
│   ├── deltadct_manager.py
│   ├── verify_features.py
│   └── verify_integrity.py
├── tests/
│   ├── phase3_tests.cpp
│   ├── phase45_tests.cpp
│   ├── phase6_benchmark.py
│   └── test_bindings.py
```

注：`build/`、`benchmark_data/`、`DeltaDCT_Output/` 等构建或运行期生成目录不在上面的核心结构展示中。
