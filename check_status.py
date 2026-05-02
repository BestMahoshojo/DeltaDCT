#!/usr/bin/env python3
import time
import subprocess
from pathlib import Path
from datetime import datetime

print("\n" + "="*80)
print("⏱️  基准测试状态监控")
print("="*80)
print(f"开始监控时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")

benchmark_dir = Path('/home/daiyi/MyProject/DeltaDCT/benchmark_data')

# 记录已存在的文件
existing_files = {}
for f in benchmark_dir.glob('benchmark_*.csv'):
    if 'summary' not in f.name and 'results' not in f.name:
        existing_files[f.name] = f.stat().st_size

print("初始数据文件:")
for name in existing_files:
    print(f"  • {name}")

print("\n监控中... 按 Ctrl+C 退出\n")

check_interval = 60  # 每分钟检查一次
max_wait = 12 * 3600  # 12小时
start_time = time.time()

while True:
    elapsed = time.time() - start_time
    
    if elapsed > max_wait:
        print(f"\n⏰ 已达到最大监控时间")
        break
    
    # 检查新文件或大小变化
    new_size_found = False
    for f in benchmark_dir.glob('benchmark_*.csv'):
        if 'summary' in f.name or 'results' in f.name:
            continue
        
        current_size = f.stat().st_size
        prev_size = existing_files.get(f.name, 0)
        
        if current_size != prev_size:
            new_size_found = True
            growth = current_size - prev_size
            print(f"[{datetime.now().strftime('%H:%M:%S')}] 📝 {f.name}: {current_size/1024/1024:.2f} MB " + 
                  (f"(↑ {growth/1024:.1f} KB)" if growth > 0 else "(↓ 检查完成)"))
            existing_files[f.name] = current_size
    
    if not new_size_found and existing_files:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] ✅ 数据持续增长中...")
    
    time.sleep(check_interval)

print("\n监控结束")
