#!/usr/bin/env python3
"""
3DGS.cpp Benchmark工具 V5
正确激活窗口并发送F12，捕获FPS数据
"""

import os
import sys
import json
import time
import subprocess
import shutil
import re
from pathlib import Path
from datetime import datetime


class BenchmarkRunner:
    """Benchmark运行器 - 正确使用F12截图"""

    def __init__(self):
        self.project_root = Path("d:/liuyue/physDreamerVulkanDemo/3DGS.cpp")
        self.viewer_path = self.project_root / "build/apps/viewer/Release/3dgs_viewer.exe"
        self.scene_path = Path("D:/liuyue/physDreamerVulkanDemo/physics_dreamer/physics_dreamer/carnations/point_cloud.ply")
        self.camera_path = self.project_root / "camera.txt"
        self.output_dir = self.project_root / "verification"

    def print_header(self, title):
        """打印标题"""
        print("=" * 60)
        print(f"  {title}".center(60))
        print("=" * 60)
        print()

    def check_prerequisites(self):
        """检查前提条件"""
        if not self.viewer_path.exists():
            print(f"[错误] 找不到viewer: {self.viewer_path}")
            return False

        if not self.scene_path.exists():
            print(f"[错误] 找不到场景: {self.scene_path}")
            return False

        print("[配置信息]")
        print(f"  Viewer: {self.viewer_path.name}")
        print(f"  Scene:  {self.scene_path.name}")
        print(f"  Camera: {self.camera_path.name}")
        print()
        return True

    def trigger_f12_screenshot(self):
        """触发F12截图"""
        try:
            import ctypes
            from ctypes import wintypes

            # 定义常量
            WM_KEYDOWN = 0x0100
            WM_KEYUP = 0x0101
            WM_CHAR = 0x0102
            VK_F12 = 0x7B
            WM_ACTIVATE = 0x0006
            WA_ACTIVE = 1

            user32 = ctypes.windll.user32

            # 查找viewer窗口
            class WindowFinder:
                def __init__(self):
                    self.hwnd = None

                def callback(self, hwnd, _):
                    if user32.IsWindowVisible(hwnd):
                        length = user32.GetWindowTextLengthW(hwnd) + 1
                        title_buf = ctypes.create_unicode_buffer(length)
                        user32.GetWindowTextW(hwnd, title_buf, length)
                        title = title_buf.value
                        if "Vulkan" in title or "Splatting" in title:
                            self.hwnd = hwnd
                            return False  # 停止枚举
                    return True

            finder = WindowFinder()
            WNDENUMPROC = ctypes.CFUNCTYPE(
                wintypes.BOOL,
                wintypes.HWND,
                wintypes.LPARAM
            )
            user32.EnumWindows(WNDENUMPROC(finder.callback), 0)

            if finder.hwnd:
                # 激活窗口（使其获得焦点）
                user32.SetForegroundWindow(finder.hwnd)
                time.sleep(0.2)  # 等待激活完成

                # 发送F12按键
                user32.PostMessageW(finder.hwnd, WM_KEYDOWN, VK_F12, 0)
                time.sleep(0.05)
                user32.PostMessageW(finder.hwnd, WM_KEYUP, VK_F12, 0)

                print("[成功] F12已发送到viewer窗口")
                return True
            else:
                print("[警告] 未找到viewer窗口")
                return False

        except Exception as e:
            print(f"[错误] 发送F12失败: {e}")
            return False

    def run_benchmark(self, stage_name="baseline", duration=15):
        """运行benchmark测试"""
        self.print_header(f"阶段验收: {stage_name}")

        # 创建输出目录
        output_dir = self.output_dir / stage_name
        output_dir.mkdir(parents=True, exist_ok=True)

        # 清理旧截图
        for old_screenshot in self.project_root.glob("screenshot_*.png"):
            try:
                old_screenshot.unlink()
                print(f"[清理] 删除: {old_screenshot.name}")
            except:
                pass
        print()

        # 启动viewer，捕获输出
        cmd = [str(self.viewer_path), "--verbose", "--camera", str(self.camera_path), str(self.scene_path)]
        print(f"[启动] Viewer进程...")

        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            universal_newlines=True
        )

        print(f"[运行] PID: {process.pid}")
        print()

        # 等待场景加载
        print(f"[等待] 场景加载...")
        time.sleep(5)

        # 收集性能数据
        fps_samples = []
        start_time = time.time()

        print(f"[运行] 收集性能数据 ({duration}秒)...")
        print("-" * 60)

        screenshot_triggered = False
        warmup_time = 8  # warmup 8秒后截图

        try:
            while True:
                # 读取输出行
                line = process.stdout.readline()
                if not line:
                    if process.poll() is not None:
                        break
                    time.sleep(0.1)
                    continue

                elapsed = time.time() - start_time

                # 解析FPS数据
                fps_match = re.search(r'FPS:\s+(\d+\.?\d*)', line)
                if fps_match:
                    fps = float(fps_match.group(1))
                    fps_samples.append((elapsed, fps))

                    if len(fps_samples) % 30 == 0:
                        print(f"\r  [FPS] {fps:.1f} fps ({elapsed:.1f}s)", end="", flush=True)

                # 触发截图 (warmup后)
                if elapsed >= warmup_time and not screenshot_triggered:
                    print(f"\n\n[截图] 触发F12截图...")
                    if self.trigger_f12_screenshot():
                        screenshot_triggered = True
                        time.sleep(3)  # 等待截图保存
                    else:
                        # 使用PIL fallback
                        print(f"[回退] 使用屏幕截图...")
                        try:
                            from PIL import ImageGrab
                            screenshot = ImageGrab.grab()
                            fallback_path = self.project_root / "screenshot_1.png"
                            screenshot.save(fallback_path)
                            print(f"[成功] 截图已保存")
                            screenshot_triggered = True
                        except:
                            print(f"[失败] 截图保存失败")

                # 超时检查
                if elapsed >= duration:
                    print(f"\n[完成] 达到指定时长")
                    break

        except KeyboardInterrupt:
            print("\n[中断] 用户取消")
        finally:
            # 终止进程
            print(f"\n[清理] 终止viewer...")
            process.terminate()
            time.sleep(2)
            if process.poll() is None:
                process.kill()

        print()

        # 查找截图文件
        print(f"[检查] 查找截图...")
        screenshots = list(self.project_root.glob("screenshot_*.png"))
        if screenshots:
            # 使用最新的截图
            screenshot_path = max(screenshots, key=lambda p: p.stat().st_mtime)
            dest_path = output_dir / f"{stage_name}_frame.png"
            shutil.move(str(screenshot_path), str(dest_path))
            print(f"[成功] 截图: {dest_path.name}")

            # 获取实际分辨率
            try:
                from PIL import Image
                img = Image.open(dest_path)
                resolution = [img.width, img.height]
            except:
                resolution = [1920, 1080]
        else:
            print(f"[警告] 未找到截图文件")
            dest_path = None
            resolution = [1920, 1080]

        # 分析FPS数据
        results = self.analyze_data(fps_samples, stage_name, resolution, dest_path)

        # 保存结果
        self.save_results(results, output_dir, stage_name)

        # 打印摘要
        self.print_summary(results, stage_name)

        return results

    def analyze_data(self, fps_samples, stage_name, resolution, screenshot_path):
        """分析收集的数据"""
        if not fps_samples:
            return {
                "stage": stage_name,
                "status": "failed",
                "error": "未收集到FPS数据"
            }

        # 移除前20%的数据（warmup期间）
        warmup_samples = int(len(fps_samples) * 0.2)
        stable_samples = fps_samples[warmup_samples:]

        fps_values = [s[1] for s in stable_samples]

        # 计算统计数据
        avg_fps = sum(fps_values) / len(fps_values)
        min_fps = min(fps_values)
        max_fps = max(fps_values)
        median_fps = sorted(fps_values)[len(fps_values) // 2]

        # 标准差
        variance = sum((x - avg_fps) ** 2 for x in fps_values) / len(fps_values)
        std_dev = variance ** 0.5

        results = {
            "timestamp": datetime.now().isoformat(),
            "stage": stage_name,
            "scene": "carnations",
            "scene_path": str(self.scene_path),
            "camera_config": str(self.camera_path),
            "resolution": resolution,
            "screenshot": f"{stage_name}_frame.png" if screenshot_path else None,
            "status": "completed",
            "fps": {
                "avg": round(avg_fps, 2),
                "min": round(min_fps, 2),
                "max": round(max_fps, 2),
                "median": round(median_fps, 2),
                "samples": len(fps_values)
            },
            "frame_time": {
                "avg": round(1000.0 / avg_fps, 2) if avg_fps > 0 else 0
            },
            "stability": {
                "std_dev": round(std_dev, 2),
                "variance": round(max_fps - min_fps, 2)
            }
        }

        return results

    def save_results(self, results, output_dir, stage_name):
        """保存结果到JSON"""
        result_file = output_dir / f"{stage_name}_results.json"
        with open(result_file, 'w', encoding='utf-8') as f:
            json.dump(results, f, indent=2, ensure_ascii=False)
        print(f"[保存] 结果: {result_file.name}")

    def print_summary(self, results, stage_name):
        """打印结果摘要"""
        print()
        print("=" * 60)
        print("  验收结果摘要".center(60))
        print("=" * 60)
        print()

        if results.get("status") == "failed":
            print(f"[错误] {results.get('error', '未知错误')}")
            return

        print(f"[阶段] {stage_name}")
        print(f"[时间] {results['timestamp']}")
        print(f"[场景] {results['scene']}")
        print(f"[分辨率] {results['resolution'][0]}x{results['resolution'][1]}")
        print()

        print(f"[FPS数据]")
        print(f"  平均:   {results['fps']['avg']:.2f} fps")
        print(f"  最小:   {results['fps']['min']:.2f} fps")
        print(f"  最大:   {results['fps']['max']:.2f} fps")
        print(f"  中位数: {results['fps']['median']:.2f} fps")
        print(f"  样本数: {results['fps']['samples']}")
        print()

        print(f"[帧时间]")
        print(f"  平均: {results['frame_time']['avg']:.2f} ms")
        print()

        print(f"[稳定性]")
        print(f"  标准差: {results['stability']['std_dev']:.2f}")
        print(f"  波动范围: {results['stability']['variance']:.2f} fps")
        print()


def main():
    """主函数"""
    import argparse

    parser = argparse.ArgumentParser(description="3DGS.cpp Benchmark工具")
    parser.add_argument("stage", default="baseline", nargs="?",
                       help="阶段名称 (baseline, stage_1, stage_2, etc.)")
    parser.add_argument("--duration", type=int, default=15,
                       help="运行时长（秒）")
    args = parser.parse_args()

    runner = BenchmarkRunner()

    if not runner.check_prerequisites():
        return 1

    results = runner.run_benchmark(args.stage, args.duration)

    if results and results.get("status") == "completed":
        print("=" * 60)
        print(f"[完成] 验收完成!")
        print(f"[输出] {runner.output_dir / args.stage}")
        print("=" * 60)
        print()
        return 0
    else:
        print("[错误] 验收失败")
        return 1


if __name__ == "__main__":
    sys.exit(main())
