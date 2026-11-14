#!/usr/bin/env python3
"""
生成 ntfs-ads-undelete 这个 fixture 的脚本，记录可复现的构造过程。

和 ntfs-basic-undelete 用同一套"翻转 in-use 位模拟删除"的方法论，但基础
镜像来源不一样——J系列（NTFS 备用数据流 ADS 恢复支持）核实过
libfsntfs 自带的公开测试镜像（third_party/.../ntfs.raw，dfvfs 项目）
完全不含任何具名备用数据流（用 fsntfsinfo -E all 核实过，4 个真实文件
的 MFT 记录都只有 $STANDARD_INFORMATION/$FILE_NAME/$SECURITY_DESCRIPTOR/
无名 $DATA 四个属性，没有第二条带 Name 字段的 $DATA 属性），没法复用
已有基础镜像，所以本次按 2026-11-14 J系列任务拆解里列的方案①——在
VM 上真实建一个带 ADS 的文件，导出裸字节做新 fixture。

基础镜像来源：真实 Windows 11 虚拟机，用 diskpart 建一个 20MB 的固定
VHD、格式化 NTFS（默认簇大小 4096 字节、MFT 记录大小 1024 字节）、
用 PowerShell 的 `Set-Content -Stream` 在卷根目录建一个文件
`ads_test.txt`（主数据流 30 字节）+ 一个具名备用数据流 `secret`
（29 字节），`fsutil volume flush` 确认落盘后 detach VHD、直接从 VHD
文件里按 `Get-Partition` 报告的分区偏移/大小切出纯 NTFS 卷字节
（固定 VHD 没有动态块分配表，分区范围内就是裸卷字节，不需要额外解析
VHD 格式）。生成过程的真实命令行输出见
docs/开发日志.md 对应 J系列执行记录。

做的事：
1. 解析卷的 $Boot 扇区拿到真实的簇大小（而不是假设和 ntfs-basic-undelete
   一样）、MFT 起始簇号、MFT 记录大小——这个卷格式化出来的 $MFT 不是
   从卷最前面开始的（卷最前面偏移 8192 字节是 $MFTMirr，真正的 $MFT
   在簇 1616，即字节偏移 6619136），这是构造这个 fixture 时踩到的一个
   真实的坑：不能照抄 ntfs-basic-undelete 里硬编码的 MFT_START 常量，
   必须每次重新从 $Boot 扇区算，否则会把 $MFTMirr 误当成 $MFT 去改，
   表面上"看起来改对了"（因为 $MFTMirr 前几条记录内容和 $MFT 一样），
   实际上改的是镜像副本，不会影响 libfsntfs 真正读取用的 $MFT。
2. 定位到目标文件 `ads_test.txt` 对应的 MFT 记录（用 fsntfsinfo -E all
   核实过是索引 38，记录里确实有 4 个属性：$STANDARD_INFORMATION、
   $FILE_NAME、无名 $DATA（主数据流，30 字节）、具名 $DATA（Name=secret，
   29 字节）——证明一个真实的 Windows ADS 文件走 Set-Content -Stream
   产生的具名数据流确实和主数据流落在同一条 MFT 记录里，不需要处理
   跨记录的 ATTRIBUTE_LIST 场景），把这条记录头部 flags 字段的 bit0
   （in-use 位）清零，模拟真实 Windows 删除这个文件时文件系统层面
   实际发生的事——和 ntfs-basic-undelete 的方法论完全一致。

用法：
    python3 make_fixture.py <源 ads_volume.raw 路径> <输出 volume.img 路径>

跑完之后，volume.img 里 `ads_test.txt` 这条记录：
  - is_allocated() 返回 false（已删除）
  - $FILE_NAME 仍然完整，文件名 ads_test.txt 可以正常读出
  - 主数据流（无名 $DATA，30 字节）和具名备用数据流（$DATA, Name=secret，
    29 字节）都完整保留，可以用
    libfsntfs_file_entry_get_number_of_alternate_data_streams /
    libfsntfs_file_entry_get_alternate_data_stream_by_utf8_name 正常读出来
    ——这正是 J1/J2 要验证的核心假设：已删除文件 MFT 记录上的具名数据流
    属性和 $FILE_NAME 一样完整保留，不会因为 is_allocated()==0 就读不到。
"""
import struct
import sys

# MFT 记录索引：(索引, 说明)
TARGET_ENTRY = (38, "ads_test.txt（含具名备用数据流 secret）")


def locate_mft_entry_offset(data, mft_entry_index):
    """从 $Boot 扇区（NTFS BPB）解析真实的簇大小/MFT 起始簇号/MFT 记录大小，
    算出某个 MFT 索引对应的字节偏移。不能硬编码一个固定的 MFT_START
    常量——同一份 make_fixture.py 换一个新建的卷跑一次，$MFT 的物理
    位置完全可能不一样（本次就实测撞到卷最前面 8192 字节偏移其实是
    $MFTMirr，真正的 $MFT 在别处）。
    """
    bytes_per_sector = struct.unpack_from("<H", data, 0x0B)[0]
    sectors_per_cluster = data[0x0D]
    mft_start_cluster = struct.unpack_from("<q", data, 0x30)[0]
    clusters_per_mft_record_raw = struct.unpack_from("<b", data, 0x40)[0]

    cluster_size = bytes_per_sector * sectors_per_cluster

    if clusters_per_mft_record_raw < 0:
        # 负数表示"2的负数次方字节"，这是 NTFS BPB 这个字段的标准编码方式
        mft_record_size = 2 ** (-clusters_per_mft_record_raw)
    else:
        mft_record_size = clusters_per_mft_record_raw * cluster_size

    mft_offset = mft_start_cluster * cluster_size
    return mft_offset + mft_entry_index * mft_record_size, mft_record_size


def main():
    if len(sys.argv) != 3:
        print(f"用法: {sys.argv[0]} <源 ads_volume.raw> <输出 volume.img>", file=sys.stderr)
        sys.exit(1)

    src_path, dst_path = sys.argv[1], sys.argv[2]

    with open(src_path, "rb") as f:
        data = bytearray(f.read())

    index, label = TARGET_ENTRY
    offset, mft_record_size = locate_mft_entry_offset(data, index)

    assert data[offset:offset + 4] == b"FILE", (
        f"entry {index} 没有找到 FILE 魔数，偏移计算可能不对（offset={offset}）"
    )

    record_number = struct.unpack_from("<I", data, offset + 44)[0]
    assert record_number == index, (
        f"entry {index} 处记录自己的编号字段是 {record_number}，和预期不符，"
        f"说明定位到了错误的记录（比如撞到了 $MFTMirr 或 $LogFile 里的旧副本）"
    )

    flags = struct.unpack_from("<H", data, offset + 22)[0]
    new_flags = flags & ~0x0001
    struct.pack_into("<H", data, offset + 22, new_flags)
    print(f"entry {index} ({label}) @ offset {offset} (mft_record_size={mft_record_size}): "
          f"flags 0x{flags:04x} -> 0x{new_flags:04x}")

    with open(dst_path, "wb") as f:
        f.write(data)

    print(f"已写出: {dst_path}")


if __name__ == "__main__":
    main()
