"""
视频帧生成工具 — 生成 RGB565 呼吸小球动画帧
输出:
  videos/frame_000.bin ~ frame_NNN.bin   (逐帧文件)
  videos/video_data.h                     (C语言数组头文件，可直接嵌入固件)

用法:
  python tools/generate_video.py --frames 60 --width 172 --height 320
"""

import os, sys, math, struct

def make_ball_frame(w, h, r, cx, cy):
    """生成一帧：白色背景 + 黑色实心圆，返回 RGB565 bytes"""
    pixels = bytearray(w * h * 2)
    # 全白
    for i in range(0, len(pixels), 2):
        pixels[i] = 0xFF
        pixels[i+1] = 0xFF
    
    # 画黑色实心圆
    for dy in range(-r, r + 1):
        py = cy + dy
        if py < 0 or py >= h:
            continue
        dx = int(math.sqrt(r * r - dy * dy))
        x0 = max(cx - dx, 0)
        x1 = min(cx + dx, w - 1)
        base = py * w
        for px in range(x0, x1 + 1):
            idx = (base + px) * 2
            pixels[idx] = 0
            pixels[idx + 1] = 0
    
    return bytes(pixels)

def generate_video(frames=60, width=172, height=320, out_dir="videos"):
    """生成呼吸小球视频帧"""
    os.makedirs(out_dir, exist_ok=True)
    cx, cy = width // 2, height // 2
    
    frames_bin = b""  # 合并后的所有帧
    frame_offsets = []
    
    for i in range(frames):
        # 三角波半径：10 → 150 → 10
        if i < frames // 2:
            r = 10 + (i * 140) // (frames // 2 - 1)
        else:
            r = 150 - ((i - frames // 2) * 140) // (frames // 2 - 1)
        
        frame_data = make_ball_frame(width, height, r, cx, cy)
        frame_offsets.append(len(frames_bin))
        frames_bin += frame_data
        
        # 写入逐帧文件
        fname = os.path.join(out_dir, f"frame_{i:03d}.bin")
        with open(fname, "wb") as f:
            f.write(frame_data)
        
        if i % 10 == 0 or i == frames - 1:
            print(f"  [{i+1:3d}/{frames}] r={r:3d} -> {fname}")
    
    # 写入合并文件
    combined_path = os.path.join(out_dir, "video_all.bin")
    with open(combined_path, "wb") as f:
        f.write(frames_bin)
    print(f"\n合并文件: {combined_path} ({len(frames_bin)} bytes, {len(frames_bin)/1024:.0f} KB)")
    
    # 写入 C 头文件
    h_path = os.path.join(out_dir, "video_data.h")
    with open(h_path, "w") as f:
        f.write(f"// 自动生成 - {frames}帧 {width}x{height} RGB565 呼吸小球动画\n")
        f.write(f"// 生成时间: {__import__('datetime').datetime.now()}\n\n")
        f.write(f"#define VIDEO_FRAMES {frames}\n")
        f.write(f"#define VIDEO_WIDTH  {width}\n")
        f.write(f"#define VIDEO_HEIGHT {height}\n")
        f.write(f"#define FRAME_SIZE   {(width * height * 2)}\n\n")
        f.write(f"// 每帧在 video_data 中的起始偏移量（字节）\n")
        f.write(f"static const uint32_t video_offsets[{frames}] = {{\n")
        for i, off in enumerate(frame_offsets):
            f.write(f"    {off}, // frame {i:03d}\n")
        f.write(f"}};\n\n")
        f.write(f"// 合并帧数据\n")
        f.write(f"static const uint8_t video_data[{len(frames_bin)}] = {{\n")
        # 每行16字节输出
        for i in range(0, len(frames_bin), 16):
            chunk = frames_bin[i:i+16]
            hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
            f.write(f"    {hex_str},\n")
        f.write(f"}};\n")
    
    print(f"C头文件: {h_path}")
    print(f"帧数: {frames}, 每帧: {width*height*2} bytes, 总计: {len(frames_bin)/1024:.0f} KB")

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="生成呼吸小球视频帧")
    parser.add_argument("--frames", type=int, default=60, help="帧数")
    parser.add_argument("--width", type=int, default=172, help="宽度")
    parser.add_argument("--height", type=int, default=320, help="高度")
    parser.add_argument("--out", type=str, default="videos", help="输出目录")
    args = parser.parse_args()
    
    print(f"生成视频帧: {args.frames}帧 {args.width}x{args.height}")
    generate_video(args.frames, args.width, args.height, args.out)
