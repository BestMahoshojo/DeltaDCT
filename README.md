# DeltaDCT

DeltaDCT是一个基于JPEG频率域DCT块级比对的无损图像去重系统。它将目标JPEG转换为可回放的增量包，支持恢复为与原图一致的输出，并提供命令行与Streamlit两种使用方式。

## 环境要求

- Linux或WSL2环境
- Python3.11
- C++17编译器
- CMake3.18+
- Git
- 可用的pip环境

安装Python依赖：

```bash
pip install -r requirements.txt
```

如果需要重新编译C++扩展，请确保系统已安装常用构建工具链。

## 构建与运行

构建项目：

```bash
bash build.sh
```

启动Streamlit界面：

```bash
streamlit run python/app.py
```

命令行扫描：

```bash
python3 main.py scan --src /path/to/dataset --out /path/to/output
```

命令行恢复：

```bash
python3 main.py restore --pack /path/to/archive.ddctpack --out /path/to/restored
```

## Benchmark

运行基准测试：

```bash
python3 main.py benchmark --dir /path/to/benchmark_dataset
```

如需强制路由模式，可使用：

```bash
python3 main.py benchmark --dir /path/to/benchmark_dataset --force-route auto
python3 main.py benchmark --dir /path/to/benchmark_dataset --force-route fpm
python3 main.py benchmark --dir /path/to/benchmark_dataset --force-route dchash
```