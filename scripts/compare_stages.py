#!/usr/bin/env python3
"""
3DGS.cpp 阶段对比工具
对比两个阶段的渲染结果
"""

import json
import sys
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("安装依赖...")
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pillow", "numpy", "-q"])
    from PIL import Image
    import numpy as np


def compare_images(img1_path, img2_path):
    """对比两张图片"""
    img1 = Image.open(img1_path)
    img2 = Image.open(img2_path)

    # 确保尺寸相同
    if img1.size != img2.size:
        img2 = img2.resize(img1.size)

    arr1 = np.array(img1)
    arr2 = np.array(img2)

    # 计算差异
    diff = np.abs(arr1.astype(float) - arr2.astype(float))
    mse = np.mean(diff ** 2)

    if mse > 0:
        psnr = 10 * np.log10(255 ** 2 / mse)
    else:
        psnr = float('inf')

    return {
        "mse": float(mse),
        "psnr": float(psnr),
        "max_diff": float(np.max(diff)),
        "mean_diff": float(np.mean(diff))
    }


def compare_stages(stage1, stage2):
    """对比两个阶段"""
    project_root = Path("d:/liuyue/physDreamerVulkanDemo/3DGS.cpp")
    verification_dir = project_root / "verification"

    img1_path = verification_dir / stage1 / f"{stage1}_frame.png"
    img2_path = verification_dir / stage2 / f"{stage2}_frame.png"

    if not img1_path.exists():
        print(f"[错误] 找不到: {img1_path}")
        return None

    if not img2_path.exists():
        print(f"[错误] 找不到: {img2_path}")
        return None

    print("=" * 60)
    print(f"  阶段对比: {stage1} vs {stage2}".center(60))
    print("=" * 60)
    print()

    # 读取结果数据
    result1_file = verification_dir / stage1 / f"{stage1}_results.json"
    result2_file = verification_dir / stage2 / f"{stage2}_results.json"

    if result1_file.exists():
        with open(result1_file, 'r') as f:
            result1 = json.load(f)
            print(f"[{stage1}] {result1.get('timestamp', 'N/A')}")

    if result2_file.exists():
        with open(result2_file, 'r') as f:
            result2 = json.load(f)
            print(f"[{stage2}] {result2.get('timestamp', 'N/A')}")

    print()

    # 对比图像
    print("[对比] 图像质量分析...")
    comparison = compare_images(img1_path, img2_path)

    print(f"  PSNR:      {comparison['psnr']:.2f} dB")
    print(f"  MSE:       {comparison['mse']:.2f}")
    print(f"  最大差异:  {comparison['max_diff']:.2f}")
    print(f"  平均差异:  {comparison['mean_diff']:.2f}")
    print()

    # 评估质量
    if comparison['psnr'] > 40:
        quality = "优秀 (几乎无差异)"
    elif comparison['psnr'] > 30:
        quality = "良好 (轻微差异)"
    elif comparison['psnr'] > 20:
        quality = "一般 (明显差异)"
    else:
        quality = "差 (严重差异)"

    print(f"[评估] {quality}")
    print()

    # 保存对比结果
    output_file = verification_dir / stage2 / f"{stage1}_vs_{stage2}.json"
    with open(output_file, 'w') as f:
        json.dump(comparison, f, indent=2)

    print(f"[保存] 对比结果: {output_file.name}")
    print()

    return comparison


def main():
    import argparse
    parser = argparse.ArgumentParser(description="3DGS.cpp 阶段对比工具")
    parser.add_argument("stage1", help="基准阶段 (如 baseline)")
    parser.add_argument("stage2", help="对比阶段 (如 stage_1)")
    args = parser.parse_args()

    compare_stages(args.stage1, args.stage2)


if __name__ == "__main__":
    main()
