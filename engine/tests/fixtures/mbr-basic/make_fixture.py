#!/usr/bin/env python3
"""
生成一个手工构造的经典 MBR（Master Boot Record）分区表磁盘镜像，
验证 libvsmbr 能不能正确解析分区边界/类型/大小。做法和 gpt-basic 的
fixture 一样：不用系统分区命令，直接按 MBR 规范拼字节。

MBR 结构很简单：一个 512 字节的扇区，偏移 446 开始是 4 个主分区项
（每个 16 字节），最后 2 字节是 0x55AA 签名。
"""
import struct
import sys

BYTES_PER_SECTOR = 512

PART1_TYPE = 0x07  # NTFS / exFAT（惯例代码，实际内容类型由文件系统自己决定，这里只测分区表本身）
PART1_START = 63
PART1_SECTORS = 2048  # 1MB

PART2_TYPE = 0x0C  # FAT32 (LBA)
PART2_START = PART1_START + PART1_SECTORS
PART2_SECTORS = 4096  # 2MB

TOTAL_SECTORS = PART2_START + PART2_SECTORS + 100  # 留一点余量


def build_partition_entry(part_type, start_lba, num_sectors):
    entry = bytearray(16)
    entry[0] = 0x00                      # 状态（非活动）
    entry[1:4] = b'\x00\x02\x00'          # 起始 CHS（占位，现代工具不依赖这个）
    entry[4] = part_type
    entry[5:8] = b'\xFF\xFF\xFF'          # 结束 CHS（占位）
    struct.pack_into('<I', entry, 8, start_lba)
    struct.pack_into('<I', entry, 12, num_sectors)
    return bytes(entry)


def main():
    if len(sys.argv) != 2:
        print(f"用法: {sys.argv[0]} <输出 volume.img 路径>", file=sys.stderr)
        sys.exit(1)

    out_path = sys.argv[1]
    image = bytearray(TOTAL_SECTORS * BYTES_PER_SECTOR)

    mbr = bytearray(BYTES_PER_SECTOR)
    entry1 = build_partition_entry(PART1_TYPE, PART1_START, PART1_SECTORS)
    entry2 = build_partition_entry(PART2_TYPE, PART2_START, PART2_SECTORS)
    mbr[446:462] = entry1
    mbr[462:478] = entry2
    mbr[510] = 0x55
    mbr[511] = 0xAA

    image[0:BYTES_PER_SECTOR] = mbr

    part1_marker = b'MBR-PART-ONE-DATA\n' * 10
    part2_marker = b'MBR-PART-TWO-DATA\n' * 10
    image[PART1_START * BYTES_PER_SECTOR:PART1_START * BYTES_PER_SECTOR + len(part1_marker)] = part1_marker
    image[PART2_START * BYTES_PER_SECTOR:PART2_START * BYTES_PER_SECTOR + len(part2_marker)] = part2_marker

    with open(out_path, 'wb') as f:
        f.write(image)

    print(f"已写出: {out_path}  大小={len(image)}字节")
    print(f"分区1: type=0x{PART1_TYPE:02x}  起始LBA={PART1_START}  扇区数={PART1_SECTORS}  偏移={PART1_START*BYTES_PER_SECTOR}  大小={PART1_SECTORS*BYTES_PER_SECTOR}")
    print(f"分区2: type=0x{PART2_TYPE:02x}  起始LBA={PART2_START}  扇区数={PART2_SECTORS}  偏移={PART2_START*BYTES_PER_SECTOR}  大小={PART2_SECTORS*BYTES_PER_SECTOR}")


if __name__ == '__main__':
    main()
