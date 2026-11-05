#!/usr/bin/env python3
"""
生成 ntfs-basic-undelete 这个 fixture 的脚本，记录可复现的构造过程。

基础镜像来源：libfsntfs 自己的测试套件引用的公开 NTFS 测试镜像
（third_party/lgpl-dynamic/libfsntfs/tests/input/public/ntfs.raw，
来自 log2timeline/dfvfs 项目的公开测试数据，通过 libfsntfs 的
synctestdata.sh 脚本拉取）。

做的事：
1. 把其中两个真实存在的小文件（passwords.txt、a_directory\\another_file）
   对应的 MFT 记录头部的 flags 字段的 bit0（in-use 位）清零，模拟"文件被删除，
   但 $DATA 指向的簇还没被覆盖"这种最常见、最该优先支持的恢复场景——
   这正是真实 Windows 删除文件时文件系统层面实际发生的事：只改元数据标志位，
   不碰数据本身。
2. 把 entry 16（NTFS 规范里"预留给未来系统文件"的区域）整条清零，模拟真实
   Windows 格式化出来的卷里那种"从未被初始化、连 FILE 魔数都没有"的 MFT 槽位。
   这个场景是 2026-11-04 在虚拟机里对真实格式化的 NTFS 卷做 Task #9 真实设备
   测试时发现的：我们合成的旧镜像里这类"从未用过"的槽位都被提前盖了一个
   "空白但合法"的记录戳（带 FILE 魔数），但真实 Windows 格式化后这个区域是
   彻底的全零字节，根本不是一条合法记录。libfsntfs 对着这种垃圾字节调用
   is_allocated() 会报错（"invalid MFT entry - missing header"），而不是
   返回"未分配"——这个场景能触发并回归测试这个真实世界的边界情况。

用法：
    python3 make_fixture.py <源 ntfs.raw 路径> <输出 volume.img 路径>

跑完之后，volume.img 里这两个文件：
  - 在正常的目录遍历里可能还看得到（因为没有改目录索引，这是预期行为，
    所以恢复逻辑不能依赖目录遍历，必须按 MFT 索引全表扫描，这也是
    真实数据恢复工具的标准做法）
  - 用 libfsntfs 的 is_allocated() 检查会返回 false
  - $FILE_NAME / $DATA 属性仍然完整，可以正常读出文件名和内容
"""
import struct
import sys

MFT_START = 16384     # 这个测试镜像里 $MFT 第一条记录的字节偏移（已用十六进制魔数 "FILE" 验证过）
ENTRY_SIZE = 1024      # 该卷的 MFT entry size（来自 fsntfsinfo 输出）
FLAGS_OFFSET = 22      # MFT 记录头部 flags 字段相对条目起始的偏移（标准 NTFS 结构，bit0=in-use, bit1=is-directory）

# 要模拟删除的条目：(MFT 索引, 说明)
TARGET_ENTRIES = [
    (66, "passwords.txt"),
    (67, "a_directory\\another_file"),
]

# 要整条清零、模拟"从未初始化的垃圾槽位"的条目
ZERO_OUT_ENTRIES = [16]


def main():
    if len(sys.argv) != 3:
        print(f"用法: {sys.argv[0]} <源 ntfs.raw> <输出 volume.img>", file=sys.stderr)
        sys.exit(1)

    src_path, dst_path = sys.argv[1], sys.argv[2]

    with open(src_path, "rb") as f:
        data = bytearray(f.read())

    for index, label in TARGET_ENTRIES:
        offset = MFT_START + index * ENTRY_SIZE
        assert data[offset:offset + 4] == b"FILE", f"entry {index} 没有找到 FILE 魔数，偏移计算可能不对"

        flags = struct.unpack_from("<H", data, offset + FLAGS_OFFSET)[0]
        new_flags = flags & ~0x0001
        struct.pack_into("<H", data, offset + FLAGS_OFFSET, new_flags)
        print(f"entry {index} ({label}) @ offset {offset}: flags 0x{flags:04x} -> 0x{new_flags:04x}")

    for index in ZERO_OUT_ENTRIES:
        offset = MFT_START + index * ENTRY_SIZE
        data[offset:offset + ENTRY_SIZE] = b"\x00" * ENTRY_SIZE
        print(f"entry {index} @ offset {offset}: 整条清零（模拟从未初始化的槽位）")

    with open(dst_path, "wb") as f:
        f.write(data)

    print(f"已写出: {dst_path}")


if __name__ == "__main__":
    main()
