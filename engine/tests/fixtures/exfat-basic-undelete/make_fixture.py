#!/usr/bin/env python3
"""
手工构造一个最小但结构合法的 exFAT 卷镜像，用于 TDD 测试。

和 FAT32/GPT/MBR 那几个 fixture 一样的理由：Mac 上没有安全、不触发磁盘
格式化防护钩子的方式去调用系统级格式化工具生成 exFAT 镜像，所以直接按
Microsoft 公开的 exFAT 规范（"exFAT File System Specification"）从零拼出
字节内容。

范围刻意收窄（和 FAT32 那一轮同样的原则，不是遗漏）：
- 不追求能被真实 OS 挂载验证——exFAT 比 FAT32 复杂得多（引导扇区校验和、
  位图、大写表都有规范要求的具体内容/校验和），做到"真实 OS 愿意挂载"这个
  标准的工作量明显超过 FAT32/GPT/MBR 那几个 fixture，不划算。这个 fixture
  只保证"我们自己的解析器能正确读出写进去的内容"，用已知字节值断言验证，
  和 GPT/MBR fixture 的验证标准一致（它们也没有做真实 OS 挂载交叉验证）。
- 位图、大写表的内容本身不是我们解析器关心的东西（我们不实现"判断哪个簇
  空闲"这类分配逻辑，只读目录项里记录的簇号/大小去恢复内容），所以这两个
  系统目录项里只写了结构正确但内容不追求精确的占位数据。
- 支持任意深度子目录递归（2026-11-05 小树裁决第12步先加了一层，第14步
  扩展成真正的递归——这个 fixture 相应补了一个二级子目录 SUBSUBDIR）、
  文件名 <=15 个 UTF-16 字符（正好 1 条 File Name 目录项装得下），假设
  数据连续存储——和 FAT32 那一轮一致的范围收窄理由。非 ASCII 文件名
  （中文等）**已支持**（2026-11-05 小树裁决第13步，核查时发现原来的
  简化转码对非 ASCII 字符是真实 bug，已修复为正确的 UTF-16LE→UTF-8
  转换），这个 fixture 专门加了一个中文文件名的已删除文件用于验证。
  （补充说明：exFAT 的文件名目录项直接存完整 UTF-16 名字，没有 FAT32
  8.3 短名"主名最多 8 字符"那种限制，所以这里子目录可以直接叫
  "SUBSUBDIR"，不会重蹈 FAT32 那个 fixture 里短名超长被截断成
  "SUBSUBDI.R" 的覆辙——两种格式的构造约束本来就不一样）
- 2026-11-05 第21步新增 BIGDIR：唯一一个内容超过一个簇的目录，验证
  "目录单簇假设"修复——BIGDIR 自己的目录内容故意撑满整簇（64 条目录项，
  没有 0x00 结束标记），必须跟着 FAT 链（24 -> 26，中间跳过 25）找到
  第二个簇，才能看到藏在那里的已删除文件 DEEP3.TXT。
"""
import struct
import hashlib
import os

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

BYTES_PER_SECTOR = 512
SECTORS_PER_CLUSTER = 4
CLUSTER_SIZE = BYTES_PER_SECTOR * SECTORS_PER_CLUSTER  # 2048

FAT_OFFSET = 12          # 主引导区(0-11) 之后——真实 exFAT 规范还要求紧跟一份
                          # 一模一样的备份引导区(12-23)，但那是给 OS 做容错用的，
                          # 我们的解析器只读主引导区，不实现备份区校验/修复，
                          # 为了减少 fixture 复杂度，这个 fixture 不包含备份引导区
                          # （和"不追求真实 OS 可挂载"这条范围收窄是同一个决定）
NUMBER_OF_FATS = 1
FAT_LENGTH = 1            # 1 个扇区，足够装下 128 个 4 字节的 FAT 表项
CLUSTER_HEAP_OFFSET = FAT_OFFSET + NUMBER_OF_FATS * FAT_LENGTH  # 25
CLUSTER_COUNT = 28
VOLUME_LENGTH = CLUSTER_HEAP_OFFSET + CLUSTER_COUNT * SECTORS_PER_CLUSTER + 11  # 留一点余量

FIRST_CLUSTER_ROOT_DIR = 4
CLUSTER_BITMAP = 2
CLUSTER_UPCASE = 3
CLUSTER_HELLO_DATA = 5
CLUSTER_SECRET_DATA = 6
CLUSTER_SUBDIR = 7                 # 子目录 SUBDIR 自己的数据簇
CLUSTER_SUBDIR_DEEP_DATA = 8       # SUBDIR\DEEP.TXT（已模拟删除）
CLUSTER_SUBDIR_STILL_DATA = 9      # SUBDIR\STILL.TXT（对照组，正常未删除）
CLUSTER_CHINESE_NAME_DATA = 10     # 中文文件名"测试文件.txt"（已模拟删除），根目录下
CLUSTER_SUBSUBDIR = 11                 # SUBDIR\SUBSUBDIR 自己的数据簇（二级子目录）
CLUSTER_SUBSUBDIR_DEEP_DATA = 12       # SUBDIR\SUBSUBDIR\DEEP2.TXT（已模拟删除）
CLUSTER_SUBSUBDIR_STILL_DATA = 13      # SUBDIR\SUBSUBDIR\STILL2.TXT（对照组，正常未删除）
# 碎片化文件场景（2026-11-05 小树裁决第16步）：FRAG.BIN 的两段数据故意
# 隔开，中间留 14-19、21-22 这些簇完全不用，模拟真实碎片化文件"第一段"
# 和"第二段"物理上不相邻——这正是第15步真机实验在真实 exFAT 设备上
# 观测到的真实布局模式（不是我们凭空编的场景）
CLUSTER_FRAG_DATA_1 = 20
CLUSTER_FRAG_DATA_2 = 23

# 多簇目录场景（2026-11-05 第21步，"目录单簇假设"修复验证）：BIGDIR 自己的
# 目录内容故意撑满整整一簇（64 条目录项，没有任何 0x00 结束标记），必须
# 跟着 FAT 链找到第二个簇（24 -> 26，中间跳过 25，和 FRAG.BIN 同样的
# 非连续链式结构）才能看到第二簇里那个已删除文件 DEEP3.TXT——如果恢复
# 引擎还停留在"目录只读第一个簇"的旧假设，这个文件会被静默漏掉，
# 扫描不会报任何错，这正是这个 bug 最危险的地方
CLUSTER_BIGDIR = 24
CLUSTER_BIGDIR_PART2 = 26
CLUSTER_STILL3_DATA = 27    # BIGDIR\STILL3.TXT（对照组，正常未删除）
CLUSTER_DEEP3_DATA = 28     # BIGDIR\DEEP3.TXT（已模拟删除，藏在第二簇才能找到）

ATTR_DIRECTORY = 0x0010


def cluster_to_byte_offset(cluster):
    sector = CLUSTER_HEAP_OFFSET + (cluster - 2) * SECTORS_PER_CLUSTER
    return sector * BYTES_PER_SECTOR


def build_main_boot_sector():
    sector = bytearray(BYTES_PER_SECTOR)
    sector[0:3] = b"\xEB\x76\x90"                 # JumpBoot
    sector[3:11] = b"EXFAT   "                     # FileSystemName
    # 0x0B..0x3F (53 字节) MustBeZero，保持全 0
    struct.pack_into("<Q", sector, 0x40, 0)                      # PartitionOffset（镜像文件场景填 0）
    struct.pack_into("<Q", sector, 0x48, VOLUME_LENGTH)           # VolumeLength
    struct.pack_into("<I", sector, 0x50, FAT_OFFSET)              # FatOffset
    struct.pack_into("<I", sector, 0x54, FAT_LENGTH)              # FatLength
    struct.pack_into("<I", sector, 0x58, CLUSTER_HEAP_OFFSET)     # ClusterHeapOffset
    struct.pack_into("<I", sector, 0x5C, CLUSTER_COUNT)           # ClusterCount
    struct.pack_into("<I", sector, 0x60, FIRST_CLUSTER_ROOT_DIR)  # FirstClusterOfRootDirectory
    struct.pack_into("<I", sector, 0x64, 0x12345678)              # VolumeSerialNumber
    struct.pack_into("<H", sector, 0x68, 0x0100)                  # FileSystemRevision 1.00
    struct.pack_into("<H", sector, 0x6A, 0x0000)                  # VolumeFlags
    sector[0x6C] = 9                                               # BytesPerSectorShift (2^9=512)
    sector[0x6D] = 2                                               # SectorsPerClusterShift (2^2=4)
    sector[0x6E] = NUMBER_OF_FATS                                  # NumberOfFats
    sector[0x6F] = 0x80                                            # DriveSelect（惯例值）
    sector[0x70] = 0                                               # PercentInUse
    # 0x71..0x77 Reserved(7字节)，0x78..0x1FD BootCode(390字节)保持全0
    struct.pack_into("<H", sector, 0x1FE, 0xAA55)                  # BootSignature
    return bytes(sector)


def build_extended_boot_sector():
    # 8 个扩展引导扇区，规范要求末尾 4 字节是 ExtendedBootSignature
    sector = bytearray(BYTES_PER_SECTOR)
    struct.pack_into("<I", sector, BYTES_PER_SECTOR - 4, 0xAA550000)
    return bytes(sector)


def build_boot_region():
    """主引导区：sector0 主引导扇区 + sector1-8 扩展引导扇区(8个) +
    sector9 OEM参数(全0) + sector10 保留(全0) + sector11 校验和扇区。
    我们自己的解析器不校验这个校验和，这里填 0 占位，不影响解析正确性。"""
    region = bytearray()
    region += build_main_boot_sector()
    for _ in range(8):
        region += build_extended_boot_sector()
    region += bytes(BYTES_PER_SECTOR)   # OEM参数 sector9，全0表示未使用
    region += bytes(BYTES_PER_SECTOR)   # 保留 sector10
    region += bytes(BYTES_PER_SECTOR)   # 校验和 sector11，占位全0（不实现校验和验证）
    assert len(region) == FAT_OFFSET * BYTES_PER_SECTOR
    return bytes(region)


def build_fat_table():
    fat = bytearray(FAT_LENGTH * BYTES_PER_SECTOR)
    entries = {
        0: 0xFFFFFFF8,
        1: 0xFFFFFFFF,
        CLUSTER_BITMAP: 0xFFFFFFFF,
        CLUSTER_UPCASE: 0xFFFFFFFF,
        FIRST_CLUSTER_ROOT_DIR: 0xFFFFFFFF,
        CLUSTER_HELLO_DATA: 0xFFFFFFFF,
        CLUSTER_SECRET_DATA: 0xFFFFFFFF,
        CLUSTER_SUBDIR: 0xFFFFFFFF,
        CLUSTER_SUBDIR_DEEP_DATA: 0xFFFFFFFF,
        CLUSTER_SUBDIR_STILL_DATA: 0xFFFFFFFF,
        CLUSTER_CHINESE_NAME_DATA: 0xFFFFFFFF,
        CLUSTER_SUBSUBDIR: 0xFFFFFFFF,
        CLUSTER_SUBSUBDIR_DEEP_DATA: 0xFFFFFFFF,
        CLUSTER_SUBSUBDIR_STILL_DATA: 0xFFFFFFFF,
        # FRAG.BIN 真正的碎片链：簇 20 的下一个簇不是相邻的 21，而是跳到
        # 23，和第15步真机实验里观测到的"跳过被占用/不相邻簇"的真实
        # 链式结构完全一致
        CLUSTER_FRAG_DATA_1: CLUSTER_FRAG_DATA_2,
        CLUSTER_FRAG_DATA_2: 0xFFFFFFFF,
        # BIGDIR 自己的簇链：24 -> 26（跳过 25），验证目录扫描也要跟着
        # FAT 链走，不能只读 first_cluster 那一个簇
        CLUSTER_BIGDIR: CLUSTER_BIGDIR_PART2,
        CLUSTER_BIGDIR_PART2: 0xFFFFFFFF,
    }
    for index, value in entries.items():
        struct.pack_into("<I", fat, index * 4, value)
    return bytes(fat)


def utf16_name_entry_bytes(entry_type, name):
    """File Name 条目：byte0=EntryType, byte1=保留(0), bytes2-31=15个UTF-16LE
    字符，不足补0。name 长度必须 <= 15（本轮范围只支持单个 Name 条目）。"""
    assert len(name) <= 15
    data = bytearray(32)
    data[0] = entry_type
    data[1] = 0
    name_utf16 = name.encode("utf-16-le")
    data[2:2 + len(name_utf16)] = name_utf16
    return bytes(data)


def build_file_entry_set(name, first_cluster, file_size, in_use=True, attributes=0x0020, fragmented=False):
    """返回 (file_entry, stream_ext_entry, name_entry) 三个 32 字节条目。
    in_use=False 模拟"已删除"：把三个条目的 EntryType 最高位(0x80)清掉，
    其余字节原样保留——这是 exFAT 真实的删除行为，和 FAT32"第一个字符被
    0xE5 永久覆盖"不同，exFAT 删除后文件名、大小、起始簇号全部完整保留，
    这也是 exFAT 恢复成功率通常比 FAT32 更高的原因。
    attributes 默认 0x0020（ARCHIVE，普通文件），子目录自己的 entry set
    传 ATTR_DIRECTORY(0x0010)。
    fragmented=True（2026-11-05 小树裁决第16步新增）：GeneralSecondaryFlags
    只置 AllocationPossible 位，清掉 NoFatChain 位（变成 0x01）——这正是
    第15步真机实验里在真实 exFAT 碎片化文件上观测到的真实标志位组合，
    表示"这个文件要跟着 FAT 表走链才能找全数据"。"""
    file_entry = bytearray(32)
    file_entry[0] = 0x85
    file_entry[1] = 2                      # SecondaryCount：1个StreamExt + 1个Name
    struct.pack_into("<H", file_entry, 2, 0)   # SetChecksum，不实现校验
    struct.pack_into("<H", file_entry, 4, attributes)  # FileAttributes
    # 其余时间戳字段保持 0，本轮范围不解析时间戳

    stream_ext = bytearray(32)
    stream_ext[0] = 0xC0
    stream_ext[1] = 0x01 if fragmented else 0x03   # GeneralSecondaryFlags
    stream_ext[3] = len(name)              # NameLength
    struct.pack_into("<H", stream_ext, 4, 0)       # NameHash，不实现校验
    struct.pack_into("<Q", stream_ext, 8, file_size)   # ValidDataLength
    struct.pack_into("<I", stream_ext, 20, first_cluster)  # FirstCluster
    struct.pack_into("<Q", stream_ext, 24, file_size)      # DataLength

    name_entry = bytearray(utf16_name_entry_bytes(0xC1, name))

    if not in_use:
        file_entry[0] &= 0x7F
        stream_ext[0] &= 0x7F
        name_entry[0] &= 0x7F

    return bytes(file_entry), bytes(stream_ext), bytes(name_entry)


def build_bitmap_entry():
    entry = bytearray(32)
    entry[0] = 0x81
    entry[1] = 0                            # BitmapFlags：第一个(唯一)位图
    struct.pack_into("<I", entry, 20, CLUSTER_BITMAP)
    struct.pack_into("<Q", entry, 24, (CLUSTER_COUNT + 7) // 8)
    return bytes(entry)


def build_upcase_entry():
    entry = bytearray(32)
    entry[0] = 0x82
    struct.pack_into("<I", entry, 4, 0)     # TableChecksum，不实现校验
    struct.pack_into("<I", entry, 20, CLUSTER_UPCASE)
    struct.pack_into("<Q", entry, 24, 4)    # 占位大写表，内容不影响我们的解析器
    return bytes(entry)


def build_root_directory_cluster(hello_in_use=True, secret_in_use=True, chinese_in_use=True):
    cluster = bytearray(CLUSTER_SIZE)
    offset = 0

    def put(entry_bytes):
        nonlocal offset
        cluster[offset:offset + 32] = entry_bytes
        offset += 32

    put(build_bitmap_entry())
    put(build_upcase_entry())

    hello_set = build_file_entry_set("HELLO.TXT", CLUSTER_HELLO_DATA, len(HELLO_CONTENT), in_use=hello_in_use)
    for e in hello_set:
        put(e)

    secret_set = build_file_entry_set("SECRET.TXT", CLUSTER_SECRET_DATA, len(SECRET_CONTENT), in_use=secret_in_use)
    for e in secret_set:
        put(e)

    subdir_set = build_file_entry_set("SUBDIR", CLUSTER_SUBDIR, CLUSTER_SIZE, in_use=True, attributes=ATTR_DIRECTORY)
    for e in subdir_set:
        put(e)

    chinese_set = build_file_entry_set(
        CHINESE_NAME, CLUSTER_CHINESE_NAME_DATA, len(CHINESE_NAME_CONTENT), in_use=chinese_in_use)
    for e in chinese_set:
        put(e)

    frag_set = build_file_entry_set(
        "FRAG.BIN", CLUSTER_FRAG_DATA_1, len(FRAG_CONTENT), in_use=False, fragmented=True)
    for e in frag_set:
        put(e)

    bigdir_set = build_file_entry_set(
        "BIGDIR", CLUSTER_BIGDIR, 2 * CLUSTER_SIZE, in_use=True, attributes=ATTR_DIRECTORY, fragmented=True)
    for e in bigdir_set:
        put(e)

    # 剩余条目保持全 0 => EntryType=0x00，扫描器读到即视为目录结束
    return bytes(cluster)


def build_subdirectory_cluster(deep_in_use=False):
    """子目录自己的数据簇：exFAT 的子目录内容就是一串 entry set，和根目录
    格式完全一样，唯一区别是子目录不需要位图/大写表这两个系统专属条目
    （那两个只在根目录出现一次）。
    2026-11-05 小树裁决第14步追加：这里再放一个二级子目录 SUBSUBDIR 自己
    的 entry set（attributes=ATTR_DIRECTORY），验证递归不是只支持固定一层。"""
    cluster = bytearray(CLUSTER_SIZE)
    offset = 0

    def put(entry_bytes):
        nonlocal offset
        cluster[offset:offset + 32] = entry_bytes
        offset += 32

    deep_set = build_file_entry_set("DEEP.TXT", CLUSTER_SUBDIR_DEEP_DATA, len(DEEP_CONTENT), in_use=deep_in_use)
    for e in deep_set:
        put(e)

    still_set = build_file_entry_set("STILL.TXT", CLUSTER_SUBDIR_STILL_DATA, len(STILL_CONTENT), in_use=True)
    for e in still_set:
        put(e)

    subsubdir_set = build_file_entry_set(
        "SUBSUBDIR", CLUSTER_SUBSUBDIR, CLUSTER_SIZE, in_use=True, attributes=ATTR_DIRECTORY)
    for e in subsubdir_set:
        put(e)

    return bytes(cluster)


def build_subsubdirectory_cluster(deep2_in_use=False):
    """第二层子目录 SUBSUBDIR 自己的数据簇，格式和第一层子目录完全一样——
    exFAT 的目录结构本来就不区分"第几层"，这正是要验证的点：核心扫描
    函数不需要为更深层级写任何特殊逻辑，递归下钻完全是 shell 层
    exfat_reader.c 的职责。"""
    cluster = bytearray(CLUSTER_SIZE)
    offset = 0

    def put(entry_bytes):
        nonlocal offset
        cluster[offset:offset + 32] = entry_bytes
        offset += 32

    deep2_set = build_file_entry_set(
        "DEEP2.TXT", CLUSTER_SUBSUBDIR_DEEP_DATA, len(DEEP2_CONTENT), in_use=deep2_in_use)
    for e in deep2_set:
        put(e)

    still2_set = build_file_entry_set(
        "STILL2.TXT", CLUSTER_SUBSUBDIR_STILL_DATA, len(STILL2_CONTENT), in_use=True)
    for e in still2_set:
        put(e)

    return bytes(cluster)


def build_bigdir_cluster1():
    """BIGDIR 的第一个簇：1 个正常(未删除)文件 STILL3.TXT(3条目) + 61 条
    1 槽位占位条目，正好填满 64 条目录项，不留任何 0x00 结束标记——必须
    靠 FAT 链找到下一个簇才能继续扫描，这正是要验证的点。占位条目用
    0x81（bitmap 类型，我们的解析器只认 EntryType & 0x7F == FILE(0x05)
    才会展开成文件，其它非 0 类型一律当成"跳过 1 条继续扫"处理，参见
    exfat_dir_entry.c 对 EXFAT_DIR_ENTRY_SET_KIND_SKIPPED 的判定）。"""
    cluster = bytearray(CLUSTER_SIZE)
    offset = 0

    def put(entry_bytes):
        nonlocal offset
        cluster[offset:offset + 32] = entry_bytes
        offset += 32

    still3_set = build_file_entry_set("STILL3.TXT", CLUSTER_STILL3_DATA, len(STILL3_CONTENT), in_use=True)
    for e in still3_set:
        put(e)

    entries_per_cluster = CLUSTER_SIZE // 32
    placeholder = bytearray(32)
    placeholder[0] = 0x81
    while offset < entries_per_cluster * 32:
        put(bytes(placeholder))

    assert offset == CLUSTER_SIZE
    return bytes(cluster)


def build_bigdir_cluster2(deep3_in_use=False):
    """BIGDIR 的第二个簇（只能靠跟随 FAT 链 24 -> 26 才能读到）：1 个已
    删除文件 DEEP3.TXT，后面剩余条目保持全 0（真正的目录结束标记落在
    这一簇，不是第一簇）。"""
    cluster = bytearray(CLUSTER_SIZE)
    offset = 0

    def put(entry_bytes):
        nonlocal offset
        cluster[offset:offset + 32] = entry_bytes
        offset += 32

    deep3_set = build_file_entry_set("DEEP3.TXT", CLUSTER_DEEP3_DATA, len(DEEP3_CONTENT), in_use=deep3_in_use)
    for e in deep3_set:
        put(e)

    return bytes(cluster)


HELLO_CONTENT = b"Hello from a hand-built exFAT volume!"
SECRET_CONTENT = b"This exFAT file will be deleted but should stay fully recoverable."
DEEP_CONTENT = b"A file inside an exFAT subdirectory, now deleted."
STILL_CONTENT = b"A file inside an exFAT subdirectory, not deleted."
DEEP2_CONTENT = b"A deleted file two levels deep inside exFAT subdirectories."
STILL2_CONTENT = b"A normal file two levels deep inside exFAT subdirectories."

# 中文文件名测试场景：2026-11-05 小树裁决第13步核查发现，exFAT 原来的
# 文件名转码对非 ASCII 字符（码点 > 0x7F）是真实 bug（每个 UTF-16 码元
# 只取低字节，中文会被截成垃圾字节），这里专门构造一个中文文件名的
# 已删除文件验证修复
CHINESE_NAME = "测试文件.txt"
CHINESE_NAME_CONTENT = "这是一个用来验证中文文件名能不能被正确恢复的测试文件。".encode("utf-8")

# 碎片化文件场景：第一段整整一簇（方便断言"第一簇内容=这个值"），
# 第二段故意不是整簇（验证最后一簇只读到文件声明的真实大小为止，
# 不会把簇里剩余的未用空间也当成文件内容读出来）
FRAG_CONTENT_PART1 = b"F" * CLUSTER_SIZE
FRAG_CONTENT_PART2 = b"G" * 600
FRAG_CONTENT = FRAG_CONTENT_PART1 + FRAG_CONTENT_PART2

# 多簇目录场景内容：STILL3.TXT 是对照组(未删除)，DEEP3.TXT 是藏在
# BIGDIR 第二个簇里的已删除文件，第21步要验证的就是它能不能被找到+恢复
STILL3_CONTENT = b"A normal file inside a two-cluster exFAT directory, not deleted."
DEEP3_CONTENT = b"A deleted file that only exists in the SECOND cluster of its directory's chain."


def pad_to_cluster(data):
    if len(data) > CLUSTER_SIZE:
        raise ValueError("fixture 数据超出单簇大小，需要调整 CLUSTER_SIZE 或缩短内容")
    return data + bytes(CLUSTER_SIZE - len(data))


def build_image(secret_deleted, deep_deleted):
    image = bytearray()
    image += build_boot_region()
    assert len(image) == FAT_OFFSET * BYTES_PER_SECTOR
    image += build_fat_table()
    assert len(image) == CLUSTER_HEAP_OFFSET * BYTES_PER_SECTOR

    # 簇 2：位图（内容占位，不影响我们的解析器）
    image += pad_to_cluster(bytes((CLUSTER_COUNT + 7) // 8))
    # 簇 3：大写表（内容占位）
    image += pad_to_cluster(bytes(4))
    # 簇 4：根目录
    image += build_root_directory_cluster(hello_in_use=True, secret_in_use=not secret_deleted, chinese_in_use=False)
    # 簇 5：HELLO.TXT 数据
    image += pad_to_cluster(HELLO_CONTENT)
    # 簇 6：SECRET.TXT 数据
    image += pad_to_cluster(SECRET_CONTENT)
    # 簇 7：SUBDIR 自己的数据（entry set 列表）
    image += build_subdirectory_cluster(deep_in_use=not deep_deleted)
    # 簇 8：SUBDIR\DEEP.TXT 数据
    image += pad_to_cluster(DEEP_CONTENT)
    # 簇 9：SUBDIR\STILL.TXT 数据
    image += pad_to_cluster(STILL_CONTENT)
    # 簇 10：中文文件名"测试文件.txt"的数据（已模拟删除）
    image += pad_to_cluster(CHINESE_NAME_CONTENT)
    # 簇 11：SUBSUBDIR 自己的数据（entry set 列表，第二层子目录）
    image += build_subsubdirectory_cluster(deep2_in_use=not deep_deleted)
    # 簇 12：SUBDIR\SUBSUBDIR\DEEP2.TXT 数据
    image += pad_to_cluster(DEEP2_CONTENT)
    # 簇 13：SUBDIR\SUBSUBDIR\STILL2.TXT 数据
    image += pad_to_cluster(STILL2_CONTENT)
    # 簇 14-19：故意空着不用，模拟碎片文件两段数据之间物理上不相邻
    for _ in range(14, 20):
        image += bytes(CLUSTER_SIZE)
    # 簇 20：FRAG.BIN 第一段数据（整簇）
    image += pad_to_cluster(FRAG_CONTENT_PART1)
    # 簇 21-22：同样空着不用
    for _ in range(21, 23):
        image += bytes(CLUSTER_SIZE)
    # 簇 23：FRAG.BIN 第二段数据（不足一簇，剩余部分补 0）
    image += pad_to_cluster(FRAG_CONTENT_PART2)
    # 簇 24：BIGDIR 自己目录内容的第一个簇（撑满 64 条目录项，无结束标记）
    image += build_bigdir_cluster1()
    # 簇 25：故意空着不用，模拟 BIGDIR 的簇链非连续（24 -> 26）
    image += bytes(CLUSTER_SIZE)
    # 簇 26：BIGDIR 自己目录内容的第二个簇（只能跟 FAT 链才能读到，
    # DEEP3.TXT（已模拟删除）就藏在这里）
    image += build_bigdir_cluster2(deep3_in_use=not deep_deleted)
    # 簇 27：BIGDIR\STILL3.TXT 数据（对照组，正常未删除）
    image += pad_to_cluster(STILL3_CONTENT)
    # 簇 28：BIGDIR\DEEP3.TXT 数据（已模拟删除，藏在目录第二簇里）
    image += pad_to_cluster(DEEP3_CONTENT)

    total_bytes = VOLUME_LENGTH * BYTES_PER_SECTOR
    if len(image) < total_bytes:
        image += bytes(total_bytes - len(image))
    assert len(image) == total_bytes
    return bytes(image)


def main():
    volume_path = os.path.join(OUT_DIR, "volume.img")
    with open(volume_path, "wb") as f:
        f.write(build_image(secret_deleted=True, deep_deleted=True))

    with open(os.path.join(OUT_DIR, "SECRET.TXT.original"), "wb") as f:
        f.write(SECRET_CONTENT)
    with open(os.path.join(OUT_DIR, "HELLO.TXT.original"), "wb") as f:
        f.write(HELLO_CONTENT)
    with open(os.path.join(OUT_DIR, "DEEP.TXT.original"), "wb") as f:
        f.write(DEEP_CONTENT)
    with open(os.path.join(OUT_DIR, "STILL.TXT.original"), "wb") as f:
        f.write(STILL_CONTENT)
    # 文件名用 ASCII 安全的名字落盘（避免跨平台文件名编码问题），内容是
    # 那个中文文件名对应的原始数据
    with open(os.path.join(OUT_DIR, "CHINESE_NAME.original"), "wb") as f:
        f.write(CHINESE_NAME_CONTENT)
    with open(os.path.join(OUT_DIR, "DEEP2.TXT.original"), "wb") as f:
        f.write(DEEP2_CONTENT)
    with open(os.path.join(OUT_DIR, "STILL2.TXT.original"), "wb") as f:
        f.write(STILL2_CONTENT)
    with open(os.path.join(OUT_DIR, "FRAG.BIN.original"), "wb") as f:
        f.write(FRAG_CONTENT)
    with open(os.path.join(OUT_DIR, "DEEP3.TXT.original"), "wb") as f:
        f.write(DEEP3_CONTENT)

    print("volume.img 大小:", os.path.getsize(volume_path), "字节")
    print("HELLO.TXT 原始内容 sha256:", hashlib.sha256(HELLO_CONTENT).hexdigest())
    print("SECRET.TXT（已模拟删除）原始内容 sha256:", hashlib.sha256(SECRET_CONTENT).hexdigest())
    print("SUBDIR\\DEEP.TXT（已模拟删除）原始内容 sha256:", hashlib.sha256(DEEP_CONTENT).hexdigest())
    print("SUBDIR\\STILL.TXT（正常未删除）原始内容 sha256:", hashlib.sha256(STILL_CONTENT).hexdigest())
    print(f"{CHINESE_NAME}（已模拟删除）原始内容 sha256:", hashlib.sha256(CHINESE_NAME_CONTENT).hexdigest())
    print("SUBDIR\\SUBSUBDIR\\DEEP2.TXT（已模拟删除，第二层子目录）原始内容 sha256:",
          hashlib.sha256(DEEP2_CONTENT).hexdigest())
    print("SUBDIR\\SUBSUBDIR\\STILL2.TXT（正常未删除，第二层子目录）原始内容 sha256:",
          hashlib.sha256(STILL2_CONTENT).hexdigest())
    print(f"FRAG.BIN（已模拟删除，碎片化存储，簇 {CLUSTER_FRAG_DATA_1} -> {CLUSTER_FRAG_DATA_2}）原始内容 sha256:",
          hashlib.sha256(FRAG_CONTENT).hexdigest())
    print(f"BIGDIR\\DEEP3.TXT（已模拟删除，藏在 BIGDIR 目录自己的第二个簇里，"
          f"簇链 {CLUSTER_BIGDIR} -> {CLUSTER_BIGDIR_PART2}）原始内容 sha256:",
          hashlib.sha256(DEEP3_CONTENT).hexdigest())
    print("簇号 -> 字节偏移 对照：")
    for c in [CLUSTER_BITMAP, CLUSTER_UPCASE, FIRST_CLUSTER_ROOT_DIR, CLUSTER_HELLO_DATA, CLUSTER_SECRET_DATA,
              CLUSTER_SUBDIR, CLUSTER_SUBDIR_DEEP_DATA, CLUSTER_SUBDIR_STILL_DATA, CLUSTER_CHINESE_NAME_DATA,
              CLUSTER_SUBSUBDIR, CLUSTER_SUBSUBDIR_DEEP_DATA, CLUSTER_SUBSUBDIR_STILL_DATA,
              CLUSTER_FRAG_DATA_1, CLUSTER_FRAG_DATA_2]:
        print(f"  cluster {c} -> byte offset {cluster_to_byte_offset(c)}")


if __name__ == "__main__":
    main()
