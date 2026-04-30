import os
import time
import hashlib
import zlib
import csv
from deltadct_manager import pydeltadct
from tqdm import tqdm

DATASETS = [
    "/home/daiyi/data/experiment_datasets/1_Watermark_Light",
    "/home/daiyi/data/experiment_datasets/1_Watermark_Medium",
    "/home/daiyi/data/experiment_datasets/1_Watermark_Heavy",
    "/home/daiyi/data/experiment_datasets/2_Cropping",
    "/home/daiyi/data/experiment_datasets/4_Stress_Mixed_Light",
    "/home/daiyi/data/experiment_datasets/4_Stress_Mixed_Medium",
    "/home/daiyi/data/experiment_datasets/4_Stress_Mixed_Heavy",
]

def md5_dedup(target_path, base_path):
    """基线 1: 传统精确去重 (如果 MD5 一样则大小为0，否则保持原大小)"""
    with open(target_path, 'rb') as f1, open(base_path, 'rb') as f2:
        if hashlib.md5(f1.read()).hexdigest() == hashlib.md5(f2.read()).hexdigest():
            return 0 # 完美去重，存指针即可
    return os.path.getsize(target_path)

def byte_compression(target_path):
    """基线 2: 传统字节级压缩 (模拟 Zstd/Zip 直接压缩目标文件)"""
    with open(target_path, 'rb') as f:
        compressed = zlib.compress(f.read(), level=9)
    return len(compressed)

def run_benchmarks():
    results = []
    
    # 确定输出目录：脚本所在目录的上一级 -> benchmark_data
    script_dir = os.path.dirname(os.path.abspath(__file__))
    parent_dir = os.path.dirname(script_dir)          # 上一级目录
    output_dir = os.path.join(parent_dir, "benchmark_data")
    os.makedirs(output_dir, exist_ok=True)            # 创建目录（如果不存在）
    
    for dataset in DATASETS:
        print(f"\n📊 正在评测数据集: {os.path.basename(dataset)}")
        if not os.path.exists(dataset): continue
        
        # 寻找所有的配对
        pairs = []
        files = os.listdir(dataset)
        for f in files:
            if (
                f.endswith('_target.jpg')
                or f.endswith('_target_shifted.jpg')
                or f.endswith('_target_stress.jpg')
            ):
                base_name = f.split('_target')[0] + "_base.jpg"
                if base_name in files:
                    pairs.append((os.path.join(dataset, f), os.path.join(dataset, base_name)))
        
        total_original = 0
        total_md5_size = 0
        total_zlib_size = 0
        total_deltadct_size = 0
        total_full_original_size = 0
        unique_base_paths = set()
        deltadct_time = 0.0
        
        for target, base in tqdm(pairs, desc="Processing Pairs"):
            orig_size = os.path.getsize(target)
            total_original += orig_size
            base_size = os.path.getsize(base)
            total_full_original_size += orig_size + base_size
            unique_base_paths.add(os.path.abspath(base))
            
            # 1. MD5 去重
            total_md5_size += md5_dedup(target, base)
            
            # 2. Zlib 压缩
            total_zlib_size += byte_compression(target)
            
            # 3. DeltaDCT 压缩
            out_ddct = target + ".ddct"
            t_start = time.perf_counter()
            # mode=0 为自动路由
            res = pydeltadct.compress_image(target, base, out_ddct) 
            t_end = time.perf_counter()
            
            if res.success:
                total_deltadct_size += res.compressed_size
                deltadct_time += (t_end - t_start)
                os.remove(out_ddct) # 测完即删，保护硬盘
            else:
                total_deltadct_size += orig_size
                deltadct_time += (t_end - t_start)
                
        # 计算统计学指标
        md5_ratio = total_original / total_md5_size if total_md5_size > 0 else 1.0
        zlib_ratio = total_original / total_zlib_size if total_zlib_size > 0 else 1.0
        deltadct_ratio = total_original / total_deltadct_size if total_deltadct_size > 0 else 1.0
        total_base_pack_size = sum(os.path.getsize(path) for path in unique_base_paths)
        total_archive_size = total_base_pack_size + total_deltadct_size
        system_ratio = total_original / total_archive_size if total_archive_size > 0 else 1.0
        system_saving_pct = (1.0 - 1.0 / system_ratio) * 100.0 if system_ratio > 0 else 0.0
        full_flow_storage_size = total_archive_size
        full_flow_ratio = (
            total_full_original_size / full_flow_storage_size
            if full_flow_storage_size > 0
            else 1.0
        )
        system_wide_saving_pct = system_saving_pct
        throughput_mbps = (total_original / 1024 / 1024) / deltadct_time if deltadct_time > 0 else 0
        
        results.append({
            "Dataset": os.path.basename(dataset),
            "Original Size (MB)": round(total_original / 1024 / 1024, 2),
            "Full Original Size (MB)": round(total_full_original_size / 1024 / 1024, 2),
            "Base Pack Size (MB)": round(total_base_pack_size / 1024 / 1024, 2),
            "Total_Archive_Size (MB)": round(total_archive_size / 1024 / 1024, 2),
            "Full Flow Ratio": round(full_flow_ratio, 2),
            "MD5 Ratio": round(md5_ratio, 2),
            "Zlib Ratio": round(zlib_ratio, 2),
            "DeltaDCT Ratio": round(deltadct_ratio, 2),
            "System_Wide_Saving_Pct": round(system_wide_saving_pct, 2),
            "Throughput (MB/s)": round(throughput_mbps, 2)
        })

    # 输出为 CSV，存放于上一级的 benchmark_data 文件夹
    csv_file = os.path.join(output_dir, "benchmark_results.csv")
    with open(csv_file, mode='w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=results[0].keys())
        writer.writeheader()
        writer.writerows(results)
    
    print(f"\n✅ 数据已保存至 {csv_file}")
    print("\n📦 全流程存储空间压缩比例 (含 Base):")
    for row in results:
        print(f"- {row['Dataset']}: {row['Full Flow Ratio']:.2f}x")
    print("\n🧮 系统级节省率 (Original / Archive):")
    for row in results:
        print(f"- {row['Dataset']}: {row['System_Wide_Saving_Pct']:.2f}%")

if __name__ == "__main__":
    run_benchmarks()