#!/usr/bin/env python3
"""
生成 fat32-basic-undelete 这个 fixture 的脚本。

和 NTFS fixture 不同：这里没有复用任何第三方镜像或系统格式化工具
（`hdiutil create -fs`/`newfs_msdos`/`diskutil` 这类命令会触发系统的磁盘格式化
安全防护，而且本质上我们接下来就要自己写一个 FAT32 解析器，干脆也自己按
公开的 FAT32 规范直接拼出镜像字节内容，不依赖任何系统级格式化工具）。

构造的卷：
- 512 字节/扇区，1 扇区/簇（簇大小 512 字节，故意选小，方便手算验证）
- 数据区簇数 >= 65525，这是 FAT32 规范里"必须按 FAT32 而不是 FAT16 解释"的
  硬性门槛（Microsoft FAT 规范按簇数而不是声称的版本号判断类型），保证这个
  镜像被任何合规的 FAT 解析器（包括真实 Windows）都认定为 FAT32，不是凑巧
  能被我们自己的解析器认出来而已
- 根目录固定占 1 个簇，放 2 个短文件名文件 + 1 个子目录 + 1 个 VFAT 长
  文件名的已删除文件（2026-11-05 小树裁决第13步追加，中文名字，长度
  超过 13 个 UTF-16 字符、需要跨 2 条长文件名目录项拼接，专门验证"物理
  相邻+倒序拼接"这条重建逻辑不是只在单条目录项的简单情况下凑巧能用），
  每个文件内容都小于 1 个簇，不涉及多簇/碎片化
- 子目录本身也固定占 1 个簇（和根目录一样的简化假设，不实现 FAT 链
  跟随），内容是真实 FAT32 子目录的标准结构："." "数据区 (指向自己)、
  ".." (指向父目录，因为父目录是根目录，按 FAT32 规范这里固定填 0，
  不是根目录的簇号——这是历史兼容性约定，不是我们的 bug)、1 个已删除
  文件、1 个正常对照组文件
- SUBDIR 下面还有一层子目录 SUBSUBDIR（2026-11-05 小树裁决第14步追加，
  验证递归不是只支持固定一层），里面同样放 1 个已删除文件 + 1 个对照
  组文件；SUBSUBDIR 的 ".." 这次要指向 SUBDIR 的簇号（不是 0），因为
  它的父目录不是根目录——这是验证递归版本"深度>0时父目录不是根目录"
  这个分支和"深度=1时父目录是根目录"的行为本来就不一样，不能偷懒复用
  同一个简化假设

用法：
    python3 make_fixture.py <输出 volume.img 路径>

跑完之后，volume.img 里：
  - HELLO.TXT 的目录项首字节被改成 0xE5（FAT 规范里标记"已删除"的方式），
    模拟"文件被删除但簇链和目录项其余内容都还没被覆盖"这个最常见场景；
    注意只改目录项，FAT 表里对应的簇链也要清空（這才是真实删除行为——
    Windows 删除 FAT 文件时，目录项首字节改 0xE5 的同时，FAT 表里这个
    文件占用的簇全部标记为空闲 0x00000000，但簇里的原始数据不会被抹掉）
  - WORLD.TXT 保持正常、未删除，作为"不应该被误判为已删除"的对照组
  - SUBDIR\\SECRET.TXT 同样方式模拟删除，用于验证子目录遍历能力
  - SUBDIR\\NORMAL.TXT 保持正常、未删除，子目录内的对照组
  - SUBDIR\\SUBSUB2\\DEEP2.TXT 同样方式模拟删除，用于验证递归能深入
    第二层子目录，不是只支持固定一层
  - SUBDIR\\SUBSUB2\\STILL2.TXT 保持正常、未删除，第二层子目录内的对照组
  - 中文长文件名的文件也被模拟删除：不仅把短名条目首字节改成 0xE5，
    前面所有 VFAT 长文件名条目的首字节（序号字节）也按真实 Windows 的
    删除行为一起改成 0xE5——这是为了验证我们的重建逻辑不依赖这个已经
    被删除操作破坏掉的序号字节，而是用物理位置推断顺序
"""
import struct
import sys

BYTES_PER_SECTOR = 512
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 32
NUM_FATS = 2
DATA_CLUSTERS = 70000  # 安全高于 65525 这个 FAT32/FAT16 判定门槛
ROOT_CLUSTER = 2
FILE1_CLUSTER = 3
FILE2_CLUSTER = 4
SUBDIR_CLUSTER = 5
SUBFILE1_CLUSTER = 6  # SUBDIR\SECRET.TXT（已模拟删除）
SUBFILE2_CLUSTER = 7  # SUBDIR\NORMAL.TXT（对照组，正常未删除）
LONGNAME_CLUSTER = 8  # 中文长文件名文件（已模拟删除），根目录下
SUBSUBDIR_CLUSTER = 9      # SUBDIR\SUBSUBDIR 自己的数据簇
SUBSUBFILE1_CLUSTER = 10   # SUBDIR\SUBSUBDIR\DEEP2.TXT（已模拟删除）
SUBSUBFILE2_CLUSTER = 11   # SUBDIR\SUBSUBDIR\STILL2.TXT（对照组，正常未删除）

# 多簇目录场景（2026-11-05 第21步，"目录单簇假设"修复验证）：BIGDIR 自己
# 的目录内容故意撑满整整一簇（这个 fixture 簇大小只有 512 字节，16 条
# 目录项正好装满，没有任何 0x00 结束标记），必须跟着 FAT 链（12 -> 14，
# 中间跳过 13）找到第二个簇，才能看到藏在那里的已删除文件 DEEP3.TXT——
# 如果恢复引擎还停留在"目录只读第一个簇"的旧假设，这个文件会被静默
# 漏掉，扫描不会报任何错，这正是这个 bug 最危险的地方。和 exFAT fixture
# 里的 BIGDIR 场景是同一个验证目的
BIGDIR_CLUSTER = 12
BIGDIR_CLUSTER_PART2 = 14
BIGDIR_STILL3_CLUSTER = 15     # BIGDIR\STILL3.TXT（对照组，正常未删除）
BIGDIR_DEEP3_CLUSTER = 16      # BIGDIR\DEEP3.TXT（已模拟删除，藏在第二簇才能找到）

FILE1_NAME_83 = b'HELLO   TXT'  # "HELLO.TXT"
FILE2_CONTENT_NAME = b'WORLD   TXT'  # "WORLD.TXT"
SUBDIR_NAME_83 = b'SUBDIR     '  # "SUBDIR"，无扩展名
SUBFILE1_NAME_83 = b'SECRET  TXT'  # "SECRET.TXT"
SUBFILE2_NAME_83 = b'NORMAL  TXT'  # "NORMAL.TXT"
SUBSUBDIR_NAME_83 = b'SUBSUB2    '  # "SUBSUB2"，无扩展名
                                     # （8.3 短名主名最多 8 个字符，"SUBSUBDIR"
                                     # 有 9 个字符装不下——这是构造这个 fixture
                                     # 时自己犯的一个小错误，不是解析器的 bug，
                                     # 第一次跑出来显示成"SUBSUBDI.R"才发现）
SUBSUBFILE1_NAME_83 = b'DEEP2   TXT'  # "DEEP2.TXT"
SUBSUBFILE2_NAME_83 = b'STILL2  TXT'  # "STILL2.TXT"
BIGDIR_NAME_83 = b'BIGDIR     '  # "BIGDIR"，无扩展名
BIGDIR_STILL3_NAME_83 = b'STILL3  TXT'  # "STILL3.TXT"
BIGDIR_DEEP3_NAME_83 = b'DEEP3   TXT'  # "DEEP3.TXT"
DOT_NAME_83 = b'.          '
DOTDOT_NAME_83 = b'..         '
# VFAT 长文件名文件必须配一个合法的 8.3 短名（Windows 会自动生成类似
# "ZHONGW~1.TXT" 这种，我们的解析器不校验短名和长名是否匹配，这里随便
# 起一个结构合法的占位短名即可）
LONGNAME_SHORT_NAME_83 = b'CHINES~1TXT'

FILE1_CONTENT = b'Hello from FAT32 test file one! This is HELLO.TXT.'
FILE2_CONTENT = b'This is WORLD.TXT, the second FAT32 test file.'
SUBFILE1_CONTENT = b'A secret file inside a FAT32 subdirectory, now deleted.'
SUBFILE2_CONTENT = b'A normal file inside a FAT32 subdirectory, not deleted.'
SUBSUBFILE1_CONTENT = b'A deleted file two levels deep inside FAT32 subdirectories.'
SUBSUBFILE2_CONTENT = b'A normal file two levels deep inside FAT32 subdirectories.'
BIGDIR_STILL3_CONTENT = b'A normal file inside a two-cluster FAT32 directory, not deleted.'
BIGDIR_DEEP3_CONTENT = b'A deleted file that only exists in the SECOND cluster of its directory chain.'
LONGNAME_NAME = '中文长文件名测试文件这是一个测试.txt'  # 20 个 UTF-16 字符，需要跨 2 条长文件名条目
LONGNAME_CONTENT = '这是用来验证跨多条 VFAT 长文件名目录项拼接是否正确的测试内容。'.encode('utf-8')

ATTR_DIRECTORY = 0x10


def short_name_checksum(name_11_bytes):
    """按 VFAT 规范计算 8.3 短文件名的校验和（存在每条长文件名条目的
    偏移 13 处）。我们自己的解析器不校验这个值，这里算出正确值纯粹是
    为了让这个 fixture 在结构上更接近真实 Windows 生成的数据，不是
    因为我们的代码依赖它。"""
    checksum = 0
    for b in name_11_bytes:
        checksum = (((checksum & 1) << 7) | (checksum >> 1)) + b
        checksum &= 0xFF
    return checksum


def build_vfat_long_name_entries(name, short_name_11_bytes):
    """构造一组 VFAT 长文件名目录项，返回值已经是真实磁盘上的存储顺序
    （从上到下）：带 LAST_LONG_ENTRY 标志、序号最大的条目在最前面，
    序号 1（紧邻短名条目）的条目在最后面——这正是我们的扫描器要应对的
    真实顺序，不是为了方便测试而简化过的顺序。"""
    checksum = short_name_checksum(short_name_11_bytes)
    name_utf16 = name.encode('utf-16-le') + b'\x00\x00'  # 加上结尾的 0x0000 终止符
    num_chars = len(name_utf16) // 2
    num_entries = -(-num_chars // 13)  # 向上取整

    padded = bytearray(name_utf16)
    while len(padded) // 2 < num_entries * 13:
        padded += b'\xff\xff'  # VFAT 规范：名字结束后，同一条目里剩余字符槽位用 0xFFFF 填充

    entries = []
    for seq in range(1, num_entries + 1):
        chunk = padded[(seq - 1) * 26: seq * 26]
        entry = bytearray(32)
        order = seq | (0x40 if seq == num_entries else 0x00)
        entry[0] = order
        entry[1:11] = chunk[0:10]
        entry[11] = 0x0F  # ATTR_LONG_NAME
        entry[12] = 0x00
        entry[13] = checksum
        entry[14:26] = chunk[10:22]
        entry[26:28] = b'\x00\x00'
        entry[28:32] = chunk[22:26]
        entries.append(bytes(entry))

    entries.reverse()  # 序号最大（LAST_LONG_ENTRY）的条目要存在最前面
    return entries


def compute_fat_size_sectors(data_clusters):
    total_fat_entries = data_clusters + 2  # 簇号 0、1 是保留项，不对应真实簇
    fat_bytes = total_fat_entries * 4
    return (fat_bytes + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR


FAT_SIZE_SECTORS = compute_fat_size_sectors(DATA_CLUSTERS)
DATA_START_SECTOR = RESERVED_SECTORS + NUM_FATS * FAT_SIZE_SECTORS
TOTAL_SECTORS = DATA_START_SECTOR + DATA_CLUSTERS * SECTORS_PER_CLUSTER
IMAGE_SIZE = TOTAL_SECTORS * BYTES_PER_SECTOR


def cluster_to_sector(cluster):
    return DATA_START_SECTOR + (cluster - 2) * SECTORS_PER_CLUSTER


def build_boot_sector():
    bs = bytearray(BYTES_PER_SECTOR)
    bs[0:3] = b'\xEB\x58\x90'
    bs[3:11] = b'MSWIN4.1'
    struct.pack_into('<H', bs, 11, BYTES_PER_SECTOR)
    bs[13] = SECTORS_PER_CLUSTER
    struct.pack_into('<H', bs, 14, RESERVED_SECTORS)
    bs[16] = NUM_FATS
    struct.pack_into('<H', bs, 17, 0)       # root entry count，FAT32 固定为 0
    struct.pack_into('<H', bs, 19, 0)       # total sectors(16)，FAT32 固定为 0，用 32 位字段
    bs[21] = 0xF8                            # media descriptor
    struct.pack_into('<H', bs, 22, 0)       # FAT size(16)，FAT32 固定为 0，用 32 位字段
    struct.pack_into('<H', bs, 24, 63)      # sectors per track（惯例值，不影响解析）
    struct.pack_into('<H', bs, 26, 255)     # number of heads（惯例值）
    struct.pack_into('<I', bs, 28, 0)       # hidden sectors
    struct.pack_into('<I', bs, 32, TOTAL_SECTORS)
    struct.pack_into('<I', bs, 36, FAT_SIZE_SECTORS)
    struct.pack_into('<H', bs, 40, 0)       # ext flags：两份 FAT 都是活跃镜像
    struct.pack_into('<H', bs, 42, 0)       # fs version 0.0
    struct.pack_into('<I', bs, 44, ROOT_CLUSTER)
    struct.pack_into('<H', bs, 48, 1)       # FSInfo 扇区号
    struct.pack_into('<H', bs, 50, 6)       # 备份引导扇区号
    bs[64] = 0x80                            # drive number
    bs[65] = 0
    bs[66] = 0x29                            # boot signature（0x28 或 0x29 都合法，0x29 表示后面带卷标+文件系统类型字符串）
    struct.pack_into('<I', bs, 67, 0x12345678)  # volume ID，随便给一个非零值
    bs[71:82] = b'FATTEST    '[:11]
    bs[82:90] = b'FAT32   '
    bs[510] = 0x55
    bs[511] = 0xAA
    return bytes(bs)


def build_fsinfo_sector():
    fs = bytearray(BYTES_PER_SECTOR)
    struct.pack_into('<I', fs, 0, 0x41615252)     # FSI_LeadSig
    struct.pack_into('<I', fs, 484, 0x61417272)   # FSI_StrucSig
    struct.pack_into('<I', fs, 488, 0xFFFFFFFF)   # FSI_Free_Count：未知，正常情况
    struct.pack_into('<I', fs, 492, 0xFFFFFFFF)   # FSI_Nxt_Free：未知
    struct.pack_into('<I', fs, 508, 0xAA550000)   # FSI_TrailSig
    return bytes(fs)


def build_fat_table():
    total_entries = DATA_CLUSTERS + 2
    entries = [0] * total_entries
    entries[0] = 0x0FFFFFF8  # 复制 media descriptor
    entries[1] = 0x0FFFFFFF  # 按惯例填 EOC
    entries[ROOT_CLUSTER] = 0x0FFFFFFF    # 根目录只占 1 个簇
    entries[FILE1_CLUSTER] = 0x0FFFFFFF   # HELLO.TXT 只占 1 个簇
    entries[FILE2_CLUSTER] = 0x0FFFFFFF   # WORLD.TXT 只占 1 个簇
    entries[SUBDIR_CLUSTER] = 0x0FFFFFFF    # SUBDIR 只占 1 个簇
    entries[SUBFILE1_CLUSTER] = 0x0FFFFFFF  # SUBDIR\SECRET.TXT 只占 1 个簇
    entries[SUBFILE2_CLUSTER] = 0x0FFFFFFF  # SUBDIR\NORMAL.TXT 只占 1 个簇
    entries[LONGNAME_CLUSTER] = 0x0FFFFFFF  # 中文长文件名文件只占 1 个簇
    entries[SUBSUBDIR_CLUSTER] = 0x0FFFFFFF    # SUBSUBDIR 只占 1 个簇
    entries[SUBSUBFILE1_CLUSTER] = 0x0FFFFFFF  # SUBDIR\SUBSUBDIR\DEEP2.TXT 只占 1 个簇
    entries[SUBSUBFILE2_CLUSTER] = 0x0FFFFFFF  # SUBDIR\SUBSUBDIR\STILL2.TXT 只占 1 个簇
    entries[BIGDIR_CLUSTER] = BIGDIR_CLUSTER_PART2  # BIGDIR 自己的簇链：12 -> 14（跳过 13）
    entries[BIGDIR_CLUSTER_PART2] = 0x0FFFFFFF
    entries[BIGDIR_STILL3_CLUSTER] = 0x0FFFFFFF
    entries[BIGDIR_DEEP3_CLUSTER] = 0x0FFFFFFF

    fat_bytes = bytearray(FAT_SIZE_SECTORS * BYTES_PER_SECTOR)
    for i, val in enumerate(entries):
        struct.pack_into('<I', fat_bytes, i * 4, val & 0x0FFFFFFF)
    return bytes(fat_bytes)


def build_dir_entry(name_83, size, first_cluster, attr=0x20):
    entry = bytearray(32)
    entry[0:11] = name_83
    entry[11] = attr
    # 12-19：保留/时间日期字段，测试场景不关心具体值，留零即可
    struct.pack_into('<H', entry, 20, (first_cluster >> 16) & 0xFFFF)
    struct.pack_into('<H', entry, 26, first_cluster & 0xFFFF)
    struct.pack_into('<I', entry, 28, size)
    return bytes(entry)


def build_root_directory_cluster():
    data = bytearray(SECTORS_PER_CLUSTER * BYTES_PER_SECTOR)
    offset = 0

    def put(entry_bytes):
        nonlocal offset
        data[offset:offset + 32] = entry_bytes
        offset += 32

    put(build_dir_entry(FILE1_NAME_83, len(FILE1_CONTENT), FILE1_CLUSTER))
    put(build_dir_entry(FILE2_CONTENT_NAME, len(FILE2_CONTENT), FILE2_CLUSTER))
    put(build_dir_entry(SUBDIR_NAME_83, 0, SUBDIR_CLUSTER, attr=ATTR_DIRECTORY))

    for long_entry in build_vfat_long_name_entries(LONGNAME_NAME, LONGNAME_SHORT_NAME_83):
        put(long_entry)
    put(build_dir_entry(LONGNAME_SHORT_NAME_83, len(LONGNAME_CONTENT), LONGNAME_CLUSTER))

    put(build_dir_entry(BIGDIR_NAME_83, 0, BIGDIR_CLUSTER, attr=ATTR_DIRECTORY))

    # 其余字节保持全零，第一个字节 0x00 本身就代表"目录到此结束"，天然满足
    return bytes(data)


ATTR_VOLUME_ID = 0x08


def build_bigdir_cluster1():
    """BIGDIR 的第一个簇：'.' + '..' + 1 个正常(未删除)文件 STILL3.TXT
    （3条目录项） + 13 条卷标类型(ATTR_VOLUME_ID)的占位条目，正好填满
    16 条目录项（这个 fixture 簇大小只有 512 字节 = 16 条），不留任何
    0x00 结束标记——必须靠 FAT 链找到下一个簇才能继续扫描，这正是要
    验证的点。占位条目用卷标属性（0x08）：fat_dir_entry.c 只把它当成
    "不是文件也不是已删除"的条目跳过 1 条继续扫，不会被误判成候选项
    或子目录。"""
    data = bytearray(SECTORS_PER_CLUSTER * BYTES_PER_SECTOR)
    offset = 0

    def put(entry_bytes):
        nonlocal offset
        data[offset:offset + 32] = entry_bytes
        offset += 32

    put(build_dir_entry(DOT_NAME_83, 0, BIGDIR_CLUSTER, attr=ATTR_DIRECTORY))
    put(build_dir_entry(DOTDOT_NAME_83, 0, 0, attr=ATTR_DIRECTORY))
    put(build_dir_entry(BIGDIR_STILL3_NAME_83, len(BIGDIR_STILL3_CONTENT), BIGDIR_STILL3_CLUSTER))

    placeholder = bytearray(32)
    placeholder[0] = 0x01
    placeholder[11] = ATTR_VOLUME_ID
    entries_per_cluster = (SECTORS_PER_CLUSTER * BYTES_PER_SECTOR) // 32
    while offset < entries_per_cluster * 32:
        put(bytes(placeholder))

    assert offset == SECTORS_PER_CLUSTER * BYTES_PER_SECTOR
    return bytes(data)


def build_bigdir_cluster2():
    """BIGDIR 的第二个簇（只能靠跟随 FAT 链 12 -> 14 才能读到）：1 个
    已删除文件 DEEP3.TXT，后面剩余条目保持全 0（真正的目录结束标记落在
    这一簇，不是第一簇）。"""
    data = bytearray(SECTORS_PER_CLUSTER * BYTES_PER_SECTOR)
    data[0:32] = build_dir_entry(BIGDIR_DEEP3_NAME_83, len(BIGDIR_DEEP3_CONTENT), BIGDIR_DEEP3_CLUSTER)
    return bytes(data)


def build_subdirectory_cluster():
    """真实 FAT32 子目录的标准结构：第一条是 "."（指向自己），第二条是
    ".."（指向父目录——因为父目录就是根目录，按 FAT32 规范这里固定填 0，
    不是根目录真实的簇号 2，这是历史兼容性约定，我们的扫描器不会去
    跟随 ".." 向上走，这个字段值本身不影响我们的功能，只是为了让这个
    fixture 在结构上和真实 Windows 生成的子目录完全一致）。第五条是
    SUBSUBDIR，第二层子目录，验证递归不是只支持固定一层。"""
    data = bytearray(SECTORS_PER_CLUSTER * BYTES_PER_SECTOR)
    dot_entry = build_dir_entry(DOT_NAME_83, 0, SUBDIR_CLUSTER, attr=ATTR_DIRECTORY)
    dotdot_entry = build_dir_entry(DOTDOT_NAME_83, 0, 0, attr=ATTR_DIRECTORY)
    entry1 = build_dir_entry(SUBFILE1_NAME_83, len(SUBFILE1_CONTENT), SUBFILE1_CLUSTER)
    entry2 = build_dir_entry(SUBFILE2_NAME_83, len(SUBFILE2_CONTENT), SUBFILE2_CLUSTER)
    entry3 = build_dir_entry(SUBSUBDIR_NAME_83, 0, SUBSUBDIR_CLUSTER, attr=ATTR_DIRECTORY)
    data[0:32] = dot_entry
    data[32:64] = dotdot_entry
    data[64:96] = entry1
    data[96:128] = entry2
    data[128:160] = entry3
    return bytes(data)


def build_subsubdirectory_cluster():
    """第二层子目录 SUBSUBDIR 的标准结构："." 指向自己，".." 这次要指向
    SUBDIR 的簇号（不是 0）——因为它的父目录不是根目录，这是和
    build_subdirectory_cluster 里 ".." 固定填 0 不一样的地方，专门验证
    这两种情况（父目录是根目录 vs 父目录是另一个子目录）在真实 FAT32
    规则下本来就不一样，不能图省事套用同一个简化假设。"""
    data = bytearray(SECTORS_PER_CLUSTER * BYTES_PER_SECTOR)
    dot_entry = build_dir_entry(DOT_NAME_83, 0, SUBSUBDIR_CLUSTER, attr=ATTR_DIRECTORY)
    dotdot_entry = build_dir_entry(DOTDOT_NAME_83, 0, SUBDIR_CLUSTER, attr=ATTR_DIRECTORY)
    entry1 = build_dir_entry(SUBSUBFILE1_NAME_83, len(SUBSUBFILE1_CONTENT), SUBSUBFILE1_CLUSTER)
    entry2 = build_dir_entry(SUBSUBFILE2_NAME_83, len(SUBSUBFILE2_CONTENT), SUBSUBFILE2_CLUSTER)
    data[0:32] = dot_entry
    data[32:64] = dotdot_entry
    data[64:96] = entry1
    data[96:128] = entry2
    return bytes(data)


def main():
    if len(sys.argv) != 2:
        print(f"用法: {sys.argv[0]} <输出 volume.img 路径>", file=sys.stderr)
        sys.exit(1)

    out_path = sys.argv[1]

    image = bytearray(IMAGE_SIZE)

    boot_sector = build_boot_sector()
    fsinfo_sector = build_fsinfo_sector()
    fat_table = build_fat_table()
    root_dir_cluster = build_root_directory_cluster()
    subdir_cluster = build_subdirectory_cluster()
    subsubdir_cluster = build_subsubdirectory_cluster()
    bigdir_cluster1 = build_bigdir_cluster1()
    bigdir_cluster2 = build_bigdir_cluster2()

    image[0:BYTES_PER_SECTOR] = boot_sector
    image[BYTES_PER_SECTOR:BYTES_PER_SECTOR * 2] = fsinfo_sector
    image[BYTES_PER_SECTOR * 6:BYTES_PER_SECTOR * 7] = boot_sector  # 备份引导扇区

    fat1_offset = RESERVED_SECTORS * BYTES_PER_SECTOR
    fat2_offset = fat1_offset + FAT_SIZE_SECTORS * BYTES_PER_SECTOR
    image[fat1_offset:fat1_offset + len(fat_table)] = fat_table
    image[fat2_offset:fat2_offset + len(fat_table)] = fat_table

    root_dir_offset = cluster_to_sector(ROOT_CLUSTER) * BYTES_PER_SECTOR
    image[root_dir_offset:root_dir_offset + len(root_dir_cluster)] = root_dir_cluster

    file1_offset = cluster_to_sector(FILE1_CLUSTER) * BYTES_PER_SECTOR
    image[file1_offset:file1_offset + len(FILE1_CONTENT)] = FILE1_CONTENT

    file2_offset = cluster_to_sector(FILE2_CLUSTER) * BYTES_PER_SECTOR
    image[file2_offset:file2_offset + len(FILE2_CONTENT)] = FILE2_CONTENT

    subdir_offset = cluster_to_sector(SUBDIR_CLUSTER) * BYTES_PER_SECTOR
    image[subdir_offset:subdir_offset + len(subdir_cluster)] = subdir_cluster

    subfile1_offset = cluster_to_sector(SUBFILE1_CLUSTER) * BYTES_PER_SECTOR
    image[subfile1_offset:subfile1_offset + len(SUBFILE1_CONTENT)] = SUBFILE1_CONTENT

    subfile2_offset = cluster_to_sector(SUBFILE2_CLUSTER) * BYTES_PER_SECTOR
    image[subfile2_offset:subfile2_offset + len(SUBFILE2_CONTENT)] = SUBFILE2_CONTENT

    longname_data_offset = cluster_to_sector(LONGNAME_CLUSTER) * BYTES_PER_SECTOR
    image[longname_data_offset:longname_data_offset + len(LONGNAME_CONTENT)] = LONGNAME_CONTENT

    subsubdir_offset = cluster_to_sector(SUBSUBDIR_CLUSTER) * BYTES_PER_SECTOR
    image[subsubdir_offset:subsubdir_offset + len(subsubdir_cluster)] = subsubdir_cluster

    subsubfile1_offset = cluster_to_sector(SUBSUBFILE1_CLUSTER) * BYTES_PER_SECTOR
    image[subsubfile1_offset:subsubfile1_offset + len(SUBSUBFILE1_CONTENT)] = SUBSUBFILE1_CONTENT

    subsubfile2_offset = cluster_to_sector(SUBSUBFILE2_CLUSTER) * BYTES_PER_SECTOR
    image[subsubfile2_offset:subsubfile2_offset + len(SUBSUBFILE2_CONTENT)] = SUBSUBFILE2_CONTENT

    bigdir1_offset = cluster_to_sector(BIGDIR_CLUSTER) * BYTES_PER_SECTOR
    image[bigdir1_offset:bigdir1_offset + len(bigdir_cluster1)] = bigdir_cluster1

    bigdir2_offset = cluster_to_sector(BIGDIR_CLUSTER_PART2) * BYTES_PER_SECTOR
    image[bigdir2_offset:bigdir2_offset + len(bigdir_cluster2)] = bigdir_cluster2

    bigdir_still3_offset = cluster_to_sector(BIGDIR_STILL3_CLUSTER) * BYTES_PER_SECTOR
    image[bigdir_still3_offset:bigdir_still3_offset + len(BIGDIR_STILL3_CONTENT)] = BIGDIR_STILL3_CONTENT

    bigdir_deep3_offset = cluster_to_sector(BIGDIR_DEEP3_CLUSTER) * BYTES_PER_SECTOR
    image[bigdir_deep3_offset:bigdir_deep3_offset + len(BIGDIR_DEEP3_CONTENT)] = BIGDIR_DEEP3_CONTENT

    # 模拟删除 HELLO.TXT：目录项首字节改 0xE5，FAT 表对应簇清空为空闲。
    # 簇里的原始数据（也就是上面刚写进去的 FILE1_CONTENT）完全不动——
    # 这正是真实 FAT 文件系统删除文件时实际发生的事。
    image[root_dir_offset] = 0xE5

    fat1_entry3_offset = fat1_offset + FILE1_CLUSTER * 4
    fat2_entry3_offset = fat2_offset + FILE1_CLUSTER * 4
    struct.pack_into('<I', image, fat1_entry3_offset, 0)
    struct.pack_into('<I', image, fat2_entry3_offset, 0)

    # 同样方式模拟删除 SUBDIR\SECRET.TXT（子目录里的第 3 条目录项，
    # 跳过 "." ".." 两条，偏移 64）
    subdir_secret_entry_offset = subdir_offset + 64
    image[subdir_secret_entry_offset] = 0xE5

    fat1_subfile1_offset = fat1_offset + SUBFILE1_CLUSTER * 4
    fat2_subfile1_offset = fat2_offset + SUBFILE1_CLUSTER * 4
    struct.pack_into('<I', image, fat1_subfile1_offset, 0)
    struct.pack_into('<I', image, fat2_subfile1_offset, 0)

    # 模拟删除中文长文件名的文件：和真实 Windows 的删除行为一致——不只是
    # 改短名条目的首字节，前面所有 VFAT 长文件名条目的首字节（序号字节）
    # 也要一起改成 0xE5。长文件名条目紧跟在 SUBDIR 条目（偏移 64）后面，
    # 从偏移 96 开始，一共 num_long_entries 条，之后是短名条目。
    num_long_entries = len(build_vfat_long_name_entries(LONGNAME_NAME, LONGNAME_SHORT_NAME_83))
    longname_entries_start = root_dir_offset + 96
    for i in range(num_long_entries):
        image[longname_entries_start + i * 32] = 0xE5
    longname_short_entry_offset = longname_entries_start + num_long_entries * 32
    image[longname_short_entry_offset] = 0xE5

    fat1_longname_offset = fat1_offset + LONGNAME_CLUSTER * 4
    fat2_longname_offset = fat2_offset + LONGNAME_CLUSTER * 4
    struct.pack_into('<I', image, fat1_longname_offset, 0)
    struct.pack_into('<I', image, fat2_longname_offset, 0)

    # 同样方式模拟删除 SUBDIR\SUBSUBDIR\DEEP2.TXT（第二层子目录里的第 3
    # 条目录项，跳过 "." ".." 两条，偏移 64）
    subsubdir_deep2_entry_offset = subsubdir_offset + 64
    image[subsubdir_deep2_entry_offset] = 0xE5

    fat1_subsubfile1_offset = fat1_offset + SUBSUBFILE1_CLUSTER * 4
    fat2_subsubfile1_offset = fat2_offset + SUBSUBFILE1_CLUSTER * 4
    struct.pack_into('<I', image, fat1_subsubfile1_offset, 0)
    struct.pack_into('<I', image, fat2_subsubfile1_offset, 0)

    # 同样方式模拟删除 BIGDIR\DEEP3.TXT（BIGDIR 目录自己的第二个簇里唯一
    # 一条目录项，偏移 0），验证"目录单簇假设"修复后，藏在第二簇里的
    # 已删除文件也能被正确识别+恢复
    image[bigdir2_offset] = 0xE5

    fat1_bigdir_deep3_offset = fat1_offset + BIGDIR_DEEP3_CLUSTER * 4
    fat2_bigdir_deep3_offset = fat2_offset + BIGDIR_DEEP3_CLUSTER * 4
    struct.pack_into('<I', image, fat1_bigdir_deep3_offset, 0)
    struct.pack_into('<I', image, fat2_bigdir_deep3_offset, 0)

    with open(out_path, 'wb') as f:
        f.write(image)

    import hashlib
    import os
    fixture_dir = os.path.dirname(os.path.abspath(out_path))
    with open(os.path.join(fixture_dir, 'HELLO.TXT.original'), 'wb') as f:
        f.write(FILE1_CONTENT)
    with open(os.path.join(fixture_dir, 'WORLD.TXT.original'), 'wb') as f:
        f.write(FILE2_CONTENT)
    with open(os.path.join(fixture_dir, 'SECRET.TXT.original'), 'wb') as f:
        f.write(SUBFILE1_CONTENT)
    with open(os.path.join(fixture_dir, 'NORMAL.TXT.original'), 'wb') as f:
        f.write(SUBFILE2_CONTENT)
    # 文件名用 ASCII 安全的名字落盘（避免跨平台文件名编码问题），内容是
    # 那个中文长文件名对应的原始数据
    with open(os.path.join(fixture_dir, 'LONGNAME_CHINESE.original'), 'wb') as f:
        f.write(LONGNAME_CONTENT)
    with open(os.path.join(fixture_dir, 'DEEP2.TXT.original'), 'wb') as f:
        f.write(SUBSUBFILE1_CONTENT)
    with open(os.path.join(fixture_dir, 'STILL2.TXT.original'), 'wb') as f:
        f.write(SUBSUBFILE2_CONTENT)
    with open(os.path.join(fixture_dir, 'BIGDIR_DEEP3.TXT.original'), 'wb') as f:
        f.write(BIGDIR_DEEP3_CONTENT)

    print(f"已写出: {out_path}  大小={IMAGE_SIZE}字节（{IMAGE_SIZE/1024/1024:.1f}MB）")
    print(f"FAT_SIZE_SECTORS={FAT_SIZE_SECTORS}  DATA_START_SECTOR={DATA_START_SECTOR}")
    print(f"root dir @ sector {cluster_to_sector(ROOT_CLUSTER)} (byte offset {root_dir_offset})")
    print(f"HELLO.TXT（已模拟删除，目录项偏移 {root_dir_offset}）@ cluster {FILE1_CLUSTER} "
          f"(sector {cluster_to_sector(FILE1_CLUSTER)}), size={len(FILE1_CONTENT)}, "
          f"sha256={hashlib.sha256(FILE1_CONTENT).hexdigest()}")
    print(f"WORLD.TXT（正常未删除，对照组）@ cluster {FILE2_CLUSTER} "
          f"(sector {cluster_to_sector(FILE2_CLUSTER)}), size={len(FILE2_CONTENT)}, "
          f"sha256={hashlib.sha256(FILE2_CONTENT).hexdigest()}")
    print(f"SUBDIR @ cluster {SUBDIR_CLUSTER} (sector {cluster_to_sector(SUBDIR_CLUSTER)})")
    print(f"SUBDIR\\SECRET.TXT（已模拟删除）@ cluster {SUBFILE1_CLUSTER} "
          f"(sector {cluster_to_sector(SUBFILE1_CLUSTER)}), size={len(SUBFILE1_CONTENT)}, "
          f"sha256={hashlib.sha256(SUBFILE1_CONTENT).hexdigest()}")
    print(f"SUBDIR\\NORMAL.TXT（正常未删除，对照组）@ cluster {SUBFILE2_CLUSTER} "
          f"(sector {cluster_to_sector(SUBFILE2_CLUSTER)}), size={len(SUBFILE2_CONTENT)}, "
          f"sha256={hashlib.sha256(SUBFILE2_CONTENT).hexdigest()}")
    print(f"{LONGNAME_NAME}（已模拟删除，VFAT 长文件名跨 {num_long_entries} 条目录项）"
          f"@ cluster {LONGNAME_CLUSTER} (sector {cluster_to_sector(LONGNAME_CLUSTER)}), "
          f"size={len(LONGNAME_CONTENT)}, sha256={hashlib.sha256(LONGNAME_CONTENT).hexdigest()}")
    print(f"SUBDIR\\SUBSUB2 @ cluster {SUBSUBDIR_CLUSTER} (sector {cluster_to_sector(SUBSUBDIR_CLUSTER)})")
    print(f"SUBDIR\\SUBSUB2\\DEEP2.TXT（已模拟删除，第二层子目录）@ cluster {SUBSUBFILE1_CLUSTER} "
          f"(sector {cluster_to_sector(SUBSUBFILE1_CLUSTER)}), size={len(SUBSUBFILE1_CONTENT)}, "
          f"sha256={hashlib.sha256(SUBSUBFILE1_CONTENT).hexdigest()}")
    print(f"SUBDIR\\SUBSUB2\\STILL2.TXT（正常未删除，对照组）@ cluster {SUBSUBFILE2_CLUSTER} "
          f"(sector {cluster_to_sector(SUBSUBFILE2_CLUSTER)}), size={len(SUBSUBFILE2_CONTENT)}, "
          f"sha256={hashlib.sha256(SUBSUBFILE2_CONTENT).hexdigest()}")
    print(f"BIGDIR（多簇目录，簇链 {BIGDIR_CLUSTER} -> {BIGDIR_CLUSTER_PART2}）")
    print(f"BIGDIR\\DEEP3.TXT（已模拟删除，藏在 BIGDIR 目录自己的第二个簇里）@ cluster {BIGDIR_DEEP3_CLUSTER} "
          f"(sector {cluster_to_sector(BIGDIR_DEEP3_CLUSTER)}), size={len(BIGDIR_DEEP3_CONTENT)}, "
          f"sha256={hashlib.sha256(BIGDIR_DEEP3_CONTENT).hexdigest()}")


if __name__ == '__main__':
    main()
