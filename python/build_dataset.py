import os
import random
from PIL import Image

SOURCE_DIR = "/home/daiyi/data/VOC2012"
LOGO_DIR = "/home/daiyi/data/logos"
OUTPUT_BASE_DIR = "/home/daiyi/data/experiment_datasets"

NUM_BASE_IMAGES = 500  
GLOBAL_QUALITY = 85
GLOBAL_SUBSAMPLING = 0  # 4:4:4 格式

def ensure_dir(path):
    if not os.path.exists(path):
        os.makedirs(path)

def generate_watermark_variants(src_images, logos):
    variants = [
        ("1_Watermark_Light", 1, 0.05, 0.10),   # 轻度：1个小Logo (5%-10%大小)
        ("1_Watermark_Medium", 3, 0.10, 0.15),  # 中度：3个中Logo (10%-15%大小)
        ("1_Watermark_Heavy", 6, 0.15, 0.25)    # 重度：6个大Logo (15%-25%大小)
    ]
    
    for dir_name, num_logos, scale_min, scale_max in variants:
        print(f"\n🚀 构建 [{dir_name}]...")
        out_dir = os.path.join(OUTPUT_BASE_DIR, dir_name)
        ensure_dir(out_dir)
        
        for i, img_name in enumerate(src_images):
            try:
                raw_img = Image.open(os.path.join(SOURCE_DIR, img_name)).convert("RGB")
                raw_img = raw_img.resize((1024, 1024), Image.Resampling.LANCZOS)
            except: continue
                
            base_path = os.path.join(out_dir, f"{i}_base.jpg")
            raw_img.save(base_path, "JPEG", quality=GLOBAL_QUALITY, subsampling=GLOBAL_SUBSAMPLING)
            
            target_rgb = raw_img.copy()
            for _ in range(num_logos):
                logo = Image.open(os.path.join(LOGO_DIR, random.choice(logos))).convert("RGBA")
                scale = random.uniform(scale_min, scale_max)
                lw, lh = int(1024 * scale), int(logo.height * (1024 * scale / logo.width))
                if lw <= 0 or lh <= 0: continue
                logo = logo.resize((lw, lh), Image.Resampling.NEAREST)
                px = random.randint(1, (1024 - lw) // 8) * 8
                py = random.randint(1, (1024 - lh) // 8) * 8
                target_rgb.paste(logo, (px, py), logo)
                
            target_rgb.save(os.path.join(out_dir, f"{i}_target.jpg"), "JPEG", quality=GLOBAL_QUALITY, subsampling=GLOBAL_SUBSAMPLING)
            if (i+1) % 100 == 0: print(f"  已处理 {i+1} 张")

def generate_cropping_dataset(src_images):
    print("\n🚀 2/3 构建 [裁剪错位冗余集] (专门测试 DCHash 路由优势)...")
    out_dir = os.path.join(OUTPUT_BASE_DIR, "2_Cropping")
    ensure_dir(out_dir)
    
    for i, img_name in enumerate(src_images):
        try:
            raw_img = Image.open(os.path.join(SOURCE_DIR, img_name)).convert("RGB")
            raw_img = raw_img.resize((1024, 1024), Image.Resampling.LANCZOS)
        except:
            continue
            
        # 1. 正常 Base
        raw_img.save(os.path.join(out_dir, f"{i}_base.jpg"), "JPEG", quality=GLOBAL_QUALITY, subsampling=GLOBAL_SUBSAMPLING)
        
        # 2. 完美的平移测试 (Shift Without Resize)
        # 创建一个全黑的 1024x1024 画布
        target_shift = Image.new("RGB", (1024, 1024), (0, 0, 0))
        # 把原图向右平移 32 像素 (恰好错开 4 个 8x8 块)，向下平移 16 像素
        # 这样原图的像素完好无损，但它们的坐标全部改变了！
        target_shift.paste(raw_img, (32, 16)) 
        
        target_shift.save(os.path.join(out_dir, f"{i}_target_shifted.jpg"), "JPEG", quality=GLOBAL_QUALITY, subsampling=GLOBAL_SUBSAMPLING)

def generate_quality_drift_dataset(src_images):
    print("\n🚀 3/3 构建 [量化漂移测试集] (测试对抗不同画质的极限)...")
    out_dir = os.path.join(OUTPUT_BASE_DIR, "3_Quality_Drift")
    ensure_dir(out_dir)
    
    for i, img_name in enumerate(src_images[:100]): # 跑 100 张即可
        try:
            raw_img = Image.open(os.path.join(SOURCE_DIR, img_name)).convert("RGB")
            raw_img = raw_img.resize((1024, 1024), Image.Resampling.LANCZOS)
        except:
            continue
            
        # Base 为 90 画质
        base_path = os.path.join(out_dir, f"{i}_base_Q90.jpg")
        raw_img.save(base_path, "JPEG", quality=90, subsampling=GLOBAL_SUBSAMPLING)
        
        # Target 降质保存 (这会导致严重的比特级不一致，用来测试强制容差去重的效果)
        raw_img.save(os.path.join(out_dir, f"{i}_target_Q75.jpg"), "JPEG", quality=75, subsampling=GLOBAL_SUBSAMPLING)
        raw_img.save(os.path.join(out_dir, f"{i}_target_Q60.jpg"), "JPEG", quality=60, subsampling=GLOBAL_SUBSAMPLING)


def generate_stress_dataset(src_images, logos):
    variants = [
        ("4_Stress_Mixed_Light", 16, 2, 0.06, 0.12),
        ("4_Stress_Mixed_Medium", 32, 3, 0.08, 0.16),
        ("4_Stress_Mixed_Heavy", 48, 5, 0.10, 0.20),
    ]

    for dir_name, shift_px, num_logos, scale_min, scale_max in variants:
        print(
            f"\n🚀 构建 [{dir_name}] (Shift={shift_px}px + {num_logos} Logos 复合变异)..."
        )
        out_dir = os.path.join(OUTPUT_BASE_DIR, dir_name)
        ensure_dir(out_dir)

        for i, img_name in enumerate(src_images):
            try:
                raw_img = Image.open(os.path.join(SOURCE_DIR, img_name)).convert("RGB")
                raw_img = raw_img.resize((1024, 1024), Image.Resampling.LANCZOS)
            except:
                continue

            raw_img.save(
                os.path.join(out_dir, f"{i}_base.jpg"),
                "JPEG",
                quality=GLOBAL_QUALITY,
                subsampling=GLOBAL_SUBSAMPLING,
            )

            shifted = Image.new("RGB", (1024, 1024), (0, 0, 0))
            shifted.paste(raw_img, (shift_px, shift_px))

            target_rgb = shifted.copy()
            for _ in range(num_logos):
                logo = Image.open(os.path.join(LOGO_DIR, random.choice(logos))).convert("RGBA")
                scale = random.uniform(scale_min, scale_max)
                lw = int(1024 * scale)
                lh = int(logo.height * (1024 * scale / logo.width))
                if lw <= 0 or lh <= 0:
                    continue
                logo = logo.resize((lw, lh), Image.Resampling.NEAREST)
                px = random.randint(1, max(1, (1024 - lw) // 8)) * 8
                py = random.randint(1, max(1, (1024 - lh) // 8)) * 8
                target_rgb.paste(logo, (px, py), logo)

            target_rgb.save(
                os.path.join(out_dir, f"{i}_target_stress.jpg"),
                "JPEG",
                quality=GLOBAL_QUALITY,
                subsampling=GLOBAL_SUBSAMPLING,
            )

            if (i + 1) % 100 == 0:
                print(f"  已处理 {i + 1} 张")

if __name__ == "__main__":
    print("初始化...")
    ensure_dir(OUTPUT_BASE_DIR)
    
    all_raw = [f for f in os.listdir(SOURCE_DIR) if f.endswith(('.jpg', '.jpeg'))]
    random.seed(2024) # 固定种子，保证可以无限次复现这一批数据
    random.shuffle(all_raw)
    selected_images = all_raw[:NUM_BASE_IMAGES]
    logos = [f for f in os.listdir(LOGO_DIR) if f.endswith('.png')]
    
    if not logos:
        print("❌ 错误: /home/daiyi/data/logos 下没有PNG图片！")
    else:
        generate_watermark_variants(selected_images, logos)
        generate_cropping_dataset(selected_images)
        generate_quality_drift_dataset(selected_images)
        generate_stress_dataset(selected_images, logos)
        print(f"\n✅ 数据集生成完毕！路径: {OUTPUT_BASE_DIR}")