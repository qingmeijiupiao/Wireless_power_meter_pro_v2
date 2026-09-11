#!/usr/bin/env python
import subprocess
import sys
import os
import re
from os.path import join

def parse_size(size_str):
    """解析大小字符串为字节数"""
    size_str = size_str.strip()
    if size_str.startswith('0x'):
        return int(size_str, 16)
    elif size_str.endswith('K'):
        return int(size_str[:-1]) * 1024
    elif size_str.endswith('M'):
        return int(size_str[:-1]) * 1024 * 1024
    else:
        return int(size_str)

def get_littlefs_dir():
    """从main/CMakeLists.txt获取LittleFS分区的原始文件目录"""
    project_root = get_project_root()
    cmake_file = os.path.join(project_root,"CMakeLists.txt")
    
    if not os.path.exists(cmake_file):
        print(f"警告: {cmake_file} 不存在")
        return None
    
    with open(cmake_file, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # 查找littlefs_create_partition_image语句
    match = re.search(r'littlefs_create_partition_image\s*\(\s*storage\s+([^\s)]+)', content)
    if not match:
        print("警告: 在main/CMakeLists.txt中找不到littlefs_create_partition_image定义")
        return None
    
    return match.group(1)

def calculate_directory_size(directory):
    """计算目录大小（字节）"""
    total_size = 0
    for dirpath, dirnames, filenames in os.walk(directory):
        for filename in filenames:
            filepath = os.path.join(dirpath, filename)
            if os.path.exists(filepath):
                total_size += os.path.getsize(filepath)
    return total_size

def round_up_to_10kb(size):
    """向上取整到10KB"""
    return ((size + 10*1024 - 1) // (10*1024)) * 10*1024

def get_project_root():
    """获取项目根目录路径"""
    current_dir = os.getcwd()
    
    # 从当前目录开始向上查找，直到找到包含CMakeLists.txt的目录
    search_dir = current_dir
    max_levels = 10  # 最多向上查找10级目录
    
    for _ in range(max_levels):
        cmake_path = os.path.join(search_dir, 'CMakeLists.txt')
        if os.path.exists(cmake_path):
            return search_dir
        
        # 如果已经到达根目录，停止查找
        parent_dir = os.path.dirname(search_dir)
        if parent_dir == search_dir:
            break
        search_dir = parent_dir
    
    # 如果找不到，返回当前目录
    return current_dir

def get_project_info():
    """从CMakeLists.txt获取项目信息"""
    project_root = get_project_root()
    cmake_file = os.path.join(project_root, "CMakeLists.txt")
    
    if not os.path.exists(cmake_file):
        print(f"错误: {cmake_file} 不存在")
        print(f"当前工作目录: {os.getcwd()}")
        print(f"项目根目录: {project_root}")
        sys.exit(1)
    
    with open(cmake_file, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # 查找 project() 语句
    project_match = re.search(r'project\s*\(\s*([^\s)]+)', content)
    if not project_match:
        print("错误: 在CMakeLists.txt中找不到project()定义")
        sys.exit(1)
    
    project_name = project_match.group(1)
    return project_name, project_root

def parse_partition_table(build_dir):
    """解析分区表文件，获取分区信息"""
    project_root = get_project_root()
    partition_file = os.path.join(project_root, "partitions.csv")
    
    if not os.path.exists(partition_file):
        print(f"错误: {partition_file} 不存在")
        sys.exit(1)
    
    # 排除的分区（不需要合并到固件中）
    exclude_partitions = ['blackbox', 'coredump']
    
    partitions = []
    with open(partition_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            # 解析分区表行: Name, Type, SubType, Offset, Size, Flags
            parts = [p.strip() for p in line.split(',')]
            if len(parts) >= 5:
                name = parts[0]
                
                # 跳过排除的分区
                if name in exclude_partitions:
                    print(f"跳过分区: {name} (不合并到固件)")
                    continue
                
                offset_hex = parts[3]
                size_str = parts[4]
                
                # 转换偏移量和大小为整数
                try:
                    offset = int(offset_hex, 16)
                    size = parse_size(size_str)
                    project_name, _ = get_project_info()
                    
                    # 构建正确的文件路径（相对于项目根目录）
                    if name == 'app0':
                        path = join(build_dir, f"{project_name}.bin")
                    else:
                        path = join(build_dir, f"{name}.bin")
                    
                    partitions.append({
                        'name': name,
                        'offset': offset,
                        'size': size,
                        'path': path,
                        'required': name in ['app0'] # app0是必需的
                    })
                except ValueError:
                    print(f"警告: 无法解析分区 {name} 的偏移量 {offset_hex} 或大小 {size_str}")
    
    return partitions

def main():
    # 从CMakeLists.txt获取项目名称和根目录
    project_name, project_root = get_project_info()
    build_dir = os.environ.get("PROJECT_BUILD_DIR", join(project_root, "build"))
    output_file = f"{project_name}_merged.bin"
    chip = "esp32c6"
    
    print(f"项目名称: {project_name}")
    print(f"项目根目录: {project_root}")
    print(f"构建目录: {build_dir}")
    print(f"输出文件: {output_file}")

    # 定义所有需要合并的分区（按偏移排序）
    partitions = []

    # 添加 bootloader（固定偏移0x1000）
    bootloader_path = join(build_dir, "bootloader", "bootloader.bin")
    partitions.append({
        "name": "bootloader",
        "offset": 0x0,     #这里有坑 C3的bootloader偏移是0x0不是0x1000
        "path": bootloader_path,
        "required": True
    })

    # 添加分区表（固定偏移0x8000）
    partition_table_path = join(build_dir, "partition_table", "partition-table.bin")
    partitions.append({
        "name": "partition_table",
        "offset": 0x8000,
        "path": partition_table_path,
        "required": True
    })

    # 从分区表文件解析其他分区（排除blackbox和coredump）
    csv_partitions = parse_partition_table(build_dir)
    partitions.extend(csv_partitions)

    # 收集存在的镜像文件，并检查必需的
    bin_files = []
    print("检查固件文件...")
    for part in partitions:
        path = part["path"]
        if os.path.exists(path):
            bin_files.append((part["offset"], path))
            print(f"找到 {part['name']} 分区: {os.path.relpath(path, project_root)} (偏移: 0x{part['offset']:x})")
        else:
            if part.get("required", False):
                print(f"错误: 必需的镜像文件 {os.path.relpath(path, project_root)} 不存在！")
                sys.exit(1)
            else:
                print(f"{os.path.relpath(path, project_root)} 不存在，分区 {part['name']} 将在合并文件中保持为 0xFF")

    if not bin_files:
        print("错误: 没有找到任何可合并的镜像文件！")
        sys.exit(1)

    # 按偏移排序
    bin_files.sort(key=lambda x: x[0])
    print(f"\n将合并 {len(bin_files)} 个分区文件")
    
    # 计算每个分区的大小（特别是bootloader和partition_table）
    sorted_partitions = sorted(partitions, key=lambda x: x['offset'])
    for i, part in enumerate(sorted_partitions):
        if 'size' not in part:
            # 计算与下一个分区的差距作为大小
            if i < len(sorted_partitions) - 1:
                next_part = sorted_partitions[i + 1]
                part['size'] = next_part['offset'] - part['offset']
            else:
                # 最后一个分区，使用默认大小（这里设为1MB）
                part['size'] = 1024 * 1024

    # 构建 esptool 命令
    output_path = join(project_root, output_file)
    cmd = [
        sys.executable, "-m", "esptool", "--chip", chip, "merge-bin",
        # "--fill-flash-size", "4MB",  # 指定Flash大小为4MB
        # "--target-offset", "0x0",    # 从地址0开始
        "-o", output_path
    ]
    for offset, path in bin_files:
        cmd.append(f"{hex(offset)}")
        cmd.append(path)

    print(f"\n执行命令: {' '.join(cmd)}")
    print("正在合并固件...")
    
    try:
        subprocess.run(cmd, check=True)
        print(f"成功生成合并固件: {output_path}")
        
        # 显示固件信息
        file_size = os.path.getsize(output_path)
        print(f"固件大小: {file_size} 字节 ({file_size/1024:.2f} KB)")
        
        # 计算固件覆盖的地址范围（排除blackbox和coredump）
        max_offset = max(offset for offset, _ in bin_files)
        last_part_size = os.path.getsize(bin_files[-1][1])
        end_address = max_offset + last_part_size
        
        print(f"固件地址范围: 0x0 - 0x{end_address:x}")
        print(f"固件覆盖区域: {end_address/1024/1024:.2f} MB")

        # 打印Flash占用情况
        print("\n" + "=" * 100)
        print("                            Flash 分区占用情况                              ")
        print("┏━━━━━━━━━━━━━━━━━━━━━┳━━━━━━━━━━━━━━┳━━━━━━━━━━┳━━━━━━━━━━━━━━━━┳━━━━━━━━━━━━━━━┓")
        print("┃ 分区名称            ┃ 总尺寸(KB)   ┃ 占用(KB) ┃ 剩余(KB)       ┃ 占用百分比    ┃")
        print("┡━━━━━━━━━━━━━━━━━━━━━╇━━━━━━━━━━━━━━╇━━━━━━━━━━╇━━━━━━━━━━━━━━━━╇━━━━━━━━━━━━━━━┩")
        
        for part in sorted_partitions:
            part_name = part['name']
            total_size = part['size']
            total_kb = total_size / 1024
            
            # 检查文件是否存在
            if os.path.exists(part['path']):
                used_size = os.path.getsize(part['path'])
            else:
                used_size = 0
            
            used_kb = used_size / 1024
            remain_kb = total_kb - used_kb
            percent = (used_size / total_size * 100) if total_size > 0 else 0
            
            # 根据分区类型调整显示
            if part_name == 'storage':
                # 计算LittleFS分区的估算占用尺寸
                littlefs_dir = get_littlefs_dir()
                if littlefs_dir:
                    project_root = get_project_root()
                    full_dir = os.path.join(project_root, littlefs_dir)
                    if os.path.exists(full_dir):
                        dir_size = calculate_directory_size(full_dir)
                        estimated_size = round_up_to_10kb(dir_size)
                        estimated_kb = estimated_size / 1024
                        remain_kb = total_kb - estimated_kb
                        percent = (estimated_size / total_size * 100) if total_size > 0 else 0
                        print(f"│ {part_name:<10}:估算占用 │ {total_kb:>12.2f} │ {estimated_kb:>8.2f} │ {remain_kb:>14.2f} │ {percent:>11.2f}%  │")
                    else:
                        # 目录不存在，只显示总尺寸
                        print(f"│ {part_name:<19} │ {total_kb:>12.2f} │ {'':>8} │ {'':>14} │ {'':>11}   │")
                else:
                    # 无法获取目录，只显示总尺寸
                    print(f"│ {part_name:<19} │ {total_kb:>12.2f} │ {'':>8} │ {'':>14} │ {'':>11}   │")
            elif part_name == 'nvs' or part_name == 'otadata' or part_name == 'app1':
                # 只显示总尺寸，其他字段为空
                print(f"│ {part_name:<19} │ {total_kb:>12.2f} │ {'':>8} │ {'':>14} │ {'':>11}   │")
            else:
                # 显示完整数据
                print(f"│ {part_name:<19} │ {total_kb:>12.2f} │ {used_kb:>8.2f} │ {remain_kb:>14.2f} │ {percent:>11.2f}%  │")
        
        print("└─────────────────────┴──────────────┴──────────┴────────────────┴───────────────┘")
            
    except subprocess.CalledProcessError as e:
        print(f"合并失败: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
