#!/usr/bin/env python3
"""
图片转RGB565数组转换器
用于将PNG、JPG、BMP等图片转换为ST7789显示屏可用的RGB565数组
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
    
import struct

def rgb888_to_rgb565(r, g, b):
    """将RGB888颜色转换为RGB565格式"""
    # 将8位颜色值转换为5位或6位
    r5 = (r >> 3) & 0x1F  # 5位红色
    g6 = (g >> 2) & 0x3F  # 6位绿色  
    b5 = (b >> 3) & 0x1F  # 5位蓝色
    
    # 组合成16位RGB565（小端序）
    rgb565 = (r5 << 11) | (g6 << 5) | b5
    
    # 转换为小端序字节
    return struct.pack('<H', rgb565)

def rgb888_to_bgr565(r, g, b):
    """将RGB888颜色转换为BGR565格式"""
    # 将8位颜色值转换为5位或6位
    r5 = (r >> 3) & 0x1F  # 5位红色
    g6 = (g >> 2) & 0x3F  # 6位绿色  
    b5 = (b >> 3) & 0x1F  # 5位蓝色
    
    # 组合成16位BGR565（小端序）
    bgr565 = (b5 << 11) | (g6 << 5) | r5
    
    # 转换为小端序字节
    return struct.pack('<H', bgr565)

def convert_image_to_rgb565(input_path, output_path, image_name=None):
    """将图片转换为RGB565数组"""
    
    try:
        # 打开图片
        img = Image.open(input_path)
        
        # 转换为RGB模式（确保颜色正确）
        if img.mode != 'RGB':
            img = img.convert('RGB')
        
        width, height = img.size
        
        # 检查图片尺寸
        if width > 160 or height > 80:
            print(f"警告: 图片尺寸({width}x{height})超过显示屏最大尺寸(160x80)")
        
        # 获取图片数据
        pixels = img.load()
        
        # 生成数组名（使用文件名或自定义名）
        if image_name is None:
            image_name = os.path.splitext(os.path.basename(input_path))[0]
            # 清理名称，只保留字母数字和下划线
            image_name = ''.join(c if c.isalnum() or c == '_' else '_' for c in image_name)
        
        # 生成头文件内容
        header_content = generate_header_file(image_name, width, height, pixels)
        
        # 写入头文件
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(header_content)
        
        print(f"转换完成: {input_path} -> {output_path}")
        print(f"图片尺寸: {width}x{height}")
        print(f"数组大小: {width * height} 像素")
        print(f"使用方式: draw_image(0, 0, {image_name.upper()}_WIDTH, {image_name.upper()}_HEIGHT, {image_name}_data)")
        
    except Exception as e:
        print(f"错误: 无法处理图片 - {e}")
        sys.exit(1)

def generate_header_file(image_name, width, height, pixels):
    """生成C语言头文件内容"""
    
    # 生成保护宏
    guard_macro = f"__{image_name.upper()}_H__"
    
    header = f"""/**
 * @file {image_name}.h
 * @brief {image_name}图片的RGB565数组数据
 * 
 * 自动生成的图片数据，用于ST7789显示屏
 * 尺寸: {width}x{height} 像素
 * 格式: RGB565 (小端序)
 */

#ifndef {guard_macro}
#define {guard_macro}

#include <stdint.h>

// 图片尺寸定义
#define {image_name.upper()}_WIDTH {width}
#define {image_name.upper()}_HEIGHT {height}

// RGB565像素数组（小端序）
const uint16_t {image_name}_data[] = {{
"""
    
    # 生成像素数据
    pixel_count = 0
    for y in range(height):
        header += "    "
        for x in range(width):
            r, g, b = pixels[x, y]
            rgb565_bytes = rgb888_to_rgb565(r, g, b)
            rgb565_value = struct.unpack('<H', rgb565_bytes)[0]
            
            header += f"0x{rgb565_value:04X}"
            if x < width - 1 or y < height - 1:
                header += ", "
            
            pixel_count += 1
            
            # 每行最多显示16个像素值
            if (x + 1) % 16 == 0 and x < width - 1:
                header += "\n    "
        
        if y < height - 1:
            header += "\n"
    
    header += f"""
}};

#endif // {guard_macro}
"""
    
    return header

def main():
    """主函数"""
    parser = argparse.ArgumentParser(
        description='将图片转换为ST7789显示屏可用的RGB565数组',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
使用示例:
  python image_converter.py image.png image.h
  python image_converter.py -n MY_IMAGE picture.jpg output.h
  
注意事项:
  - 支持格式: PNG, JPG, JPEG, BMP, GIF等
  - 输出格式: RGB565 16位大端序
  - 最大推荐尺寸: 160x80像素
        """
    )
    
    parser.add_argument('input', help='输入图片文件路径')
    parser.add_argument('output', help='输出头文件路径')
    parser.add_argument('-n', '--name', help='自定义图片数组名称（默认使用文件名）')
    
    args = parser.parse_args()
    
    # 检查输入文件是否存在
    if not os.path.exists(args.input):
        print(f"错误: 输入文件 '{args.input}' 不存在")
        sys.exit(1)
    
    # 检查PIL是否可用（ESP-IDF环境通常已安装）
    try:
        from PIL import Image
    except ImportError:
        print("错误: 需要安装Pillow库")
        print("在ESP-IDF环境中，通常已安装。如需手动安装:")
        print("pip install Pillow")
        sys.exit(1)
    
    # 执行转换
    convert_image_to_rgb565(args.input, args.output, args.name)

if __name__ == "__main__":
    main()