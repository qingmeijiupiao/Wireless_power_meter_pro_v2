#!/usr/bin/env python3
"""
从 TrueType/OpenType 字体生成包含字体数据的 C++ 头文件和源文件。
输出格式适用于嵌入式系统，假设 Font_t 结构体已在 Font.h 中定义。
此版本保留原始字符间距（步进宽度）、侧边距和统一基线。

用法: python generate_font.py <字体文件> <字体大小> <字体名称>
示例: python generate_font.py /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf 16 DejaVuSans
"""

import sys
import os
import argparse
# 检查 PIL 库是否已安装
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("错误：未找到 PIL 库（Pillow）。")
    print("请先安装依赖：")
    print("    pip install -r requirements.txt")
    sys.exit(1)
    
def char_advance(font, char):
    """返回字符的步进宽度（像素），四舍五入取整。"""
    try:
        return int(round(font.getlength(char)))
    except:
        return 0

def get_char_info(font, char):
    """
    获取字符相对统一基线的布局信息。
    返回：
        advance_width (int)   - 四舍五入的步进宽度（用作表中的宽度）
        bbox (tuple)          - 相对基线的字符边界框 (x0, y0, x1, y1)
    对于不可见字符（空格等），返回 (advance_width, None)。
    """
    bbox = font.getbbox(char, anchor='ls')
    if bbox is None or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
        w = char_advance(font, char)
        if w == 0:
            w = max(1, font.size // 2)
        return w, None

    advance = char_advance(font, char)
    left = min(0, bbox[0])
    right = max(advance, bbox[2])
    return right - left, bbox

def generate_preview(chars, char_preview_images, output_base, font_height, baseline_y):
    """生成所有字符的 BMP 预览图像。"""
    cols = 16
    rows = (len(chars) + cols - 1) // cols

    max_width = max(img.width for img in char_preview_images if img is not None) if any(img is not None for img in char_preview_images) else 1
    pad_x = 2
    pad_y = 2
    cell_w = max_width + 2 * pad_x
    cell_h = font_height + 2 * pad_y

    img_w = cols * cell_w
    img_h = rows * cell_h
    preview = Image.new('L', (img_w, img_h), 255)  # 白色背景

    # 灰色基线便于检查 g/p/q/y 等下伸字母是否与其他字符共用同一基线。
    for row in range(rows):
        baseline = row * cell_h + pad_y + baseline_y
        ImageDraw.Draw(preview).line((0, baseline, img_w - 1, baseline), fill=210)

    for idx, ch in enumerate(chars):
        row = idx // cols
        col = idx % cols
        x0 = col * cell_w
        y0 = row * cell_h

        img = char_preview_images[idx]
        if img is None:
            continue

        inverted = Image.eval(img, lambda p: 255 - p)  # 反转颜色，白底黑字更易读
        x_offset = x0 + pad_x + (max_width - img.width) // 2
        y_offset = y0 + pad_y
        preview.paste(inverted, (x_offset, y_offset))

    preview_filename = f"{output_base}_preview.bmp"
    preview.save(preview_filename)
    print(f"预览图像已保存为 {preview_filename}")

def generate_font_files(font_path, font_size, font_name, characters=None, output_dir=None):
    """生成 C++ 头文件和源文件，保留原始间距。"""
    try:
        font = ImageFont.truetype(font_path, font_size)
    except Exception as e:
        print(f"加载字体出错: {e}")
        sys.exit(1)

    # 标准可打印 ASCII 字符（32-126）
    chars = [chr(i) for i in range(32, 127)]
    if characters is not None:
        if not characters or any(ch not in chars for ch in characters):
            raise ValueError("--chars must contain printable ASCII characters")
        characters = set(characters)

    # 对每个字符存储步进宽度和相对统一基线的边界框。
    widths = []
    bboxes = []

    for ch in chars:
        advance, bbox = get_char_info(font, ch)
        widths.append(advance)
        bboxes.append(bbox)

    # 所有字符使用同一基线。上伸和下伸空间按整套字库统一计算，
    # 避免 g/p/q/y 等字符因逐字符贴底而与其他字母上下错位。
    visible_bboxes = [bbox for bbox in bboxes if bbox is not None]
    top = min(bbox[1] for bbox in visible_bboxes)
    bottom = max(bbox[3] for bbox in visible_bboxes)
    font_height = bottom - top
    baseline_y = -top

    # 构建字体数据（每个字符占用 font_height * width 字节）
    font_data = bytearray()
    char_preview_images = []  # 用于预览的最终图像（已垂直居中）

    for i, ch in enumerate(chars):
        # Keep the 95-entry lookup table compatible with Font_t. Missing glyphs
        # have zero width and consume no bitmap bytes. Baseline stays unchanged.
        if characters is not None and ch not in characters:
            widths[i] = 0
            char_preview_images.append(None)
            continue
        w = widths[i]                 # 步进宽度（用于宽度表）
        bbox = bboxes[i]

        if bbox is None:
            # 不可见字符：填充全零，宽度为步进宽度
            data = bytearray(font_height * w)
            char_preview_images.append(None)
        else:
            # 创建画布（步进宽度, 字体高度），黑色背景
            canvas = Image.new('L', (w, font_height), 0)
            draw = ImageDraw.Draw(canvas)
            x_offset = -min(0, bbox[0])
            draw.text((x_offset, baseline_y), ch, fill=255, font=font, anchor='ls')

            # 获取像素数据
            data = canvas.tobytes()
            char_preview_images.append(canvas)

        font_data.extend(data)

    # 创建以字体名称命名的文件夹（如果不存在）
    output_dir = output_dir or font_name
    os.makedirs(output_dir, exist_ok=True)

    # 构建输出文件路径
    header_path = os.path.join(output_dir, f"{font_name}.h")
    source_path = os.path.join(output_dir, f"{font_name}.cpp")
    preview_base = os.path.join(output_dir, font_name)  # 用于预览图像的基础名

    # 写入头文件
    guard = font_name.upper() + "_H"
    with open(header_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f"#ifndef {guard}\n")
        f.write(f"#define {guard}\n\n")
        f.write(f"// Font Name : {font_name}\n\n")
        f.write('#include "Font.h"\n\n')
        f.write(f"constexpr int {font_name}_FONT_HEIGHT = {font_height};\n\n")
        f.write(f"extern const Font_t {font_name};\n\n")
        f.write(f"#endif // {guard}\n")

    print(f"已生成头文件 {header_path}")

    # 写入源文件
    with open(source_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'// Generated by scripts/generate_font.py: {font_name}\n')
        if characters is not None:
            f.write(f'// Glyph subset: {"".join(sorted(characters))}\n')
        f.write(f'#include "{font_name}.h"\n\n')
        # 字体数据数组（静态）
        f.write(f"static const uint8_t {font_name}_font_data[] = {{\n")
        for start in range(0, len(font_data), 16):
            chunk = font_data[start:start + 16]
            f.write("    " + ", ".join(f"0x{byte:02x}" for byte in chunk))
            f.write(",\n" if start + 16 < len(font_data) else "\n")
        f.write('\n};\n\n')
        # 宽度表（常量）
        f.write(f"const uint8_t {font_name}_width_table[] = {{\n")
        for start in range(0, len(widths), 16):
            chunk = widths[start:start + 16]
            f.write("    " + ", ".join(f"{width:3d}" for width in chunk))
            f.write(",\n" if start + 16 < len(widths) else "\n")
        f.write('\n};\n\n')
        # Font_t 实例
        f.write(f"const Font_t {font_name}{{\n")
        f.write(f"    .font_height = {font_name}_FONT_HEIGHT,\n")
        f.write(f"    .width_table = {font_name}_width_table,\n")
        f.write(f"    .font_data = {font_name}_font_data\n")
        f.write("};\n")

    print(f"已生成源文件 {source_path}")
    print(f"Bitmap: {len(font_data)} bytes; width table: {len(widths)} bytes")

    # 生成预览图像
    generate_preview(chars, char_preview_images, preview_base, font_height, baseline_y)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('font_path')
    parser.add_argument('font_size', type=int, choices=range(1, 256), metavar='SIZE')
    parser.add_argument('font_name')
    parser.add_argument('--chars', help='Only store these printable ASCII glyphs')
    parser.add_argument('--output-dir', help='Generated files directory')
    args = parser.parse_args()
    generate_font_files(args.font_path, args.font_size, args.font_name, args.chars, args.output_dir)
