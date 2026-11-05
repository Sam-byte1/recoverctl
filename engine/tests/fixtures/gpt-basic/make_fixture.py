#!/usr/bin/env python3
"""
生成一个手工构造的 GPT（GUID Partition Table）磁盘镜像，用来验证
libvsgpt 能不能正确解析分区边界/类型/大小。

和 FAT32 fixture 一样的原则：不用 `diskutil partitionDisk` 这类系统级
分区命令（同样会触发磁盘操作安全防护），改成按 UEFI 规范用 Python
直接拼出 Protective MBR + GPT 主副本的字节内容。

布局：
  LBA 0            Protective MBR
  LBA 1            GPT 主头
  LBA 2-33         GPT 分区项数组（128 项 * 128 字节 = 32 扇区）
  LBA 34-...       两个分区的数据区（内容只是填充识别用的字节，不需要
                   真的是一个完整文件系统——这一步只验证分区表解析本身）
  倒数第 33 扇区起  GPT 备份分区项数组
  最后一扇区        GPT 备份头
"""
import struct
import sys
import uuid
import zlib

BYTES_PER_SECTOR = 512
TOTAL_SECTORS = 20480  # 10MB

PARTITION_ENTRY_SIZE = 128
NUMBER_OF_PARTITION_ENTRIES = 128
PARTITION_ARRAY_SECTORS = (PARTITION_ENTRY_SIZE * NUMBER_OF_PARTITION_ENTRIES) // BYTES_PER_SECTOR  # 32

PRIMARY_HEADER_LBA = 1
PRIMARY_ARRAY_START_LBA = 2
BACKUP_HEADER_LBA = TOTAL_SECTORS - 1
BACKUP_ARRAY_START_LBA = BACKUP_HEADER_LBA - PARTITION_ARRAY_SECTORS

FIRST_USABLE_LBA = PRIMARY_ARRAY_START_LBA + PARTITION_ARRAY_SECTORS  # 34
LAST_USABLE_LBA = BACKUP_ARRAY_START_LBA - 1

MS_BASIC_DATA_TYPE_GUID = uuid.UUID('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7')

PART1_START = FIRST_USABLE_LBA
PART1_SECTORS = 2048  # 1MB
PART1_END = PART1_START + PART1_SECTORS - 1

PART2_START = PART1_END + 1
PART2_SECTORS = 4096  # 2MB
PART2_END = PART2_START + PART2_SECTORS - 1

DISK_GUID = uuid.UUID('11111111-1111-1111-1111-111111111111')
PART1_GUID = uuid.UUID('22222222-2222-2222-2222-222222222222')
PART2_GUID = uuid.UUID('33333333-3333-3333-3333-333333333333')


def build_protective_mbr():
    mbr = bytearray(BYTES_PER_SECTOR)
    # 分区项 1（偏移 446）：type 0xEE 表示"GPT 保护分区"，占满（或接近占满）整个磁盘
    entry_offset = 446
    mbr[entry_offset + 0] = 0x00                      # 状态
    mbr[entry_offset + 1:entry_offset + 4] = b'\x00\x02\x00'  # 起始 CHS（占位值，现代工具不依赖这个）
    mbr[entry_offset + 4] = 0xEE                       # 分区类型：GPT 保护分区
    mbr[entry_offset + 5:entry_offset + 8] = b'\xFF\xFF\xFF'  # 结束 CHS（占位值）
    struct.pack_into('<I', mbr, entry_offset + 8, 1)   # 起始 LBA = 1
    struct.pack_into('<I', mbr, entry_offset + 12, min(TOTAL_SECTORS - 1, 0xFFFFFFFF))
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def build_partition_entry(type_guid, unique_guid, first_lba, last_lba, name):
    entry = bytearray(PARTITION_ENTRY_SIZE)
    entry[0:16] = type_guid.bytes_le
    entry[16:32] = unique_guid.bytes_le
    struct.pack_into('<Q', entry, 32, first_lba)
    struct.pack_into('<Q', entry, 40, last_lba)
    struct.pack_into('<Q', entry, 48, 0)  # attributes
    name_utf16 = name.encode('utf-16-le')[:72]
    entry[56:56 + len(name_utf16)] = name_utf16
    return bytes(entry)


def build_partition_array():
    array = bytearray(PARTITION_ARRAY_SECTORS * BYTES_PER_SECTOR)
    entry1 = build_partition_entry(MS_BASIC_DATA_TYPE_GUID, PART1_GUID, PART1_START, PART1_END, 'PART-ONE')
    entry2 = build_partition_entry(MS_BASIC_DATA_TYPE_GUID, PART2_GUID, PART2_START, PART2_END, 'PART-TWO')
    array[0:PARTITION_ENTRY_SIZE] = entry1
    array[PARTITION_ENTRY_SIZE:PARTITION_ENTRY_SIZE * 2] = entry2
    return bytes(array)


def build_gpt_header(current_lba, backup_lba, partition_array, partition_array_lba):
    header = bytearray(BYTES_PER_SECTOR)
    header[0:8] = b'EFI PART'
    struct.pack_into('<I', header, 8, 0x00010000)   # revision 1.0
    struct.pack_into('<I', header, 12, 92)          # header size
    struct.pack_into('<I', header, 16, 0)           # header CRC32，先占位填 0，算完整体后再回填
    struct.pack_into('<I', header, 20, 0)           # reserved
    struct.pack_into('<Q', header, 24, current_lba)
    struct.pack_into('<Q', header, 32, backup_lba)
    struct.pack_into('<Q', header, 40, FIRST_USABLE_LBA)
    struct.pack_into('<Q', header, 48, LAST_USABLE_LBA)
    header[56:72] = DISK_GUID.bytes_le
    struct.pack_into('<Q', header, 72, partition_array_lba)
    struct.pack_into('<I', header, 80, NUMBER_OF_PARTITION_ENTRIES)
    struct.pack_into('<I', header, 84, PARTITION_ENTRY_SIZE)

    partition_array_crc = zlib.crc32(partition_array) & 0xFFFFFFFF
    struct.pack_into('<I', header, 88, partition_array_crc)

    # 头部自身的 CRC32 只算前 92 字节（header size 字段声明的长度），且算的时候
    # header CRC32 这个字段本身必须先置零，这是 UEFI 规范明确要求的计算方式
    header_crc = zlib.crc32(bytes(header[0:92])) & 0xFFFFFFFF
    struct.pack_into('<I', header, 16, header_crc)

    return bytes(header)


def main():
    if len(sys.argv) != 2:
        print(f"用法: {sys.argv[0]} <输出 volume.img 路径>", file=sys.stderr)
        sys.exit(1)

    out_path = sys.argv[1]
    image = bytearray(TOTAL_SECTORS * BYTES_PER_SECTOR)

    protective_mbr = build_protective_mbr()
    image[0:BYTES_PER_SECTOR] = protective_mbr

    partition_array = build_partition_array()

    primary_header = build_gpt_header(PRIMARY_HEADER_LBA, BACKUP_HEADER_LBA, partition_array, PRIMARY_ARRAY_START_LBA)
    backup_header = build_gpt_header(BACKUP_HEADER_LBA, PRIMARY_HEADER_LBA, partition_array, BACKUP_ARRAY_START_LBA)

    image[PRIMARY_HEADER_LBA * BYTES_PER_SECTOR:(PRIMARY_HEADER_LBA + 1) * BYTES_PER_SECTOR] = primary_header
    image[PRIMARY_ARRAY_START_LBA * BYTES_PER_SECTOR:(PRIMARY_ARRAY_START_LBA + PARTITION_ARRAY_SECTORS) * BYTES_PER_SECTOR] = partition_array

    image[BACKUP_ARRAY_START_LBA * BYTES_PER_SECTOR:(BACKUP_ARRAY_START_LBA + PARTITION_ARRAY_SECTORS) * BYTES_PER_SECTOR] = partition_array
    image[BACKUP_HEADER_LBA * BYTES_PER_SECTOR:(BACKUP_HEADER_LBA + 1) * BYTES_PER_SECTOR] = backup_header

    # 分区数据区填一点可识别的内容，方便以后需要的话做内容级校验
    part1_marker = b'PART-ONE-DATA-MARKER\n' * 10
    part2_marker = b'PART-TWO-DATA-MARKER\n' * 10
    image[PART1_START * BYTES_PER_SECTOR:PART1_START * BYTES_PER_SECTOR + len(part1_marker)] = part1_marker
    image[PART2_START * BYTES_PER_SECTOR:PART2_START * BYTES_PER_SECTOR + len(part2_marker)] = part2_marker

    with open(out_path, 'wb') as f:
        f.write(image)

    print(f"已写出: {out_path}  大小={len(image)}字节（{len(image)/1024/1024:.1f}MB）")
    print(f"分区1: LBA {PART1_START}-{PART1_END}（{PART1_SECTORS}扇区，{PART1_SECTORS*BYTES_PER_SECTOR}字节），偏移={PART1_START*BYTES_PER_SECTOR}")
    print(f"分区2: LBA {PART2_START}-{PART2_END}（{PART2_SECTORS}扇区，{PART2_SECTORS*BYTES_PER_SECTOR}字节），偏移={PART2_START*BYTES_PER_SECTOR}")


if __name__ == '__main__':
    main()
