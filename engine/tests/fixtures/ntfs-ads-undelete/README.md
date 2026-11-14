# fixture: ntfs-ads-undelete

J系列（NTFS 备用数据流 ADS 恢复支持）的标准答案：一个被模拟删除的文件，
它的 MFT 记录上既有主数据流（无名 `$DATA`），也有一个具名备用数据流
（Alternate Data Stream, ADS）。

## 文件说明

| 文件 | 说明 |
|---|---|
| `volume.img` | 18.9MB 的 NTFS 卷镜像，里面有 1 个文件（带 1 个具名 ADS）被模拟删除 |
| `make_fixture.py` | 生成 `volume.img` 的脚本，可从源镜像重新生成，结果字节级可复现 |
| `ads_test.txt.original` | MFT 索引 38 对应文件主数据流的原始内容（30 字节） |
| `ads_test.txt.secret.original` | 同一条 MFT 记录上具名备用数据流 `secret` 的原始内容（29 字节） |

## 为什么这个 fixture 要专门新建，不能复用 `ntfs-basic-undelete`

`ntfs-basic-undelete` 的基础镜像来自 libfsntfs 自己测试套件引用的公开
NTFS 测试镜像（dfvfs 项目），2026-11-14 核实过（`fsntfsinfo -E all` 全表
检查）这个镜像里的全部真实文件都只有 4 个标准属性
（`$STANDARD_INFORMATION`/`$FILE_NAME`/`$SECURITY_DESCRIPTOR`/无名
`$DATA`），没有任何文件带具名 `$DATA` 属性（即 ADS），libfsntfs 自己的
单元测试里涉及 ADS 的几个函数也全是 `TODO: add tests`、没有现成的
测试数据可以抄。这是真实的 fixture 缺口，不是没找对地方。

## 基础镜像来源（真机构造过程）

在真实 Windows 11 虚拟机（Parallels，`prlctl exec`）上：

1. `diskpart` 建一个 20MB 固定（`type=fixed`）VHD，格式化 NTFS（quick），
   赋盘符 `Z:`（默认簇大小 4096 字节、MFT 记录大小 1024 字节）。
2. 在 `Z:\ads_test.txt` 写入主数据流内容 `MAIN-STREAM-CONTENT-0123456789`
   （30 字节，`[System.IO.File]::WriteAllText`，ASCII 编码，不带结尾换行）。
3. 用 PowerShell `Set-Content -Path Z:\ads_test.txt -Stream secret -Value
   "ADS-STREAM-CONTENT-ABCDEFGHIJ" -NoNewline` 写入一个名为 `secret` 的
   具名备用数据流（29 字节）。
4. `fsutil volume flush Z:` 强制落盘，等待几秒后用 `dir /r` 确认
   `ads_test.txt:secret:$DATA` 确实出现在目录列表里（Windows 原生工具
   能看到真实 ADS 的标准验证方式）。
5. `diskpart detach vdisk`，直接用 .NET `FileStream` 按
   `Get-Partition` 报告的分区偏移（65536）/大小（19857408 字节）从
   VHD 文件本身切出纯 NTFS 卷字节（固定 VHD 没有动态块分配表，分区
   范围内就是裸卷字节，不需要额外解析 VHD 格式），导出为
   `ads_volume.raw`，通过 Parallels 共享文件夹传回 Mac。

真实命令行输出（简化摘录）：

```
---VERIFY-DIR---
2026/11/14  21:08                30 ads_test.txt
                                 29 ads_test.txt:secret:$DATA
---VERIFY-MAIN-CONTENT---
MAIN-STREAM-CONTENT-0123456789
---VERIFY-ADS-CONTENT---
ADS-STREAM-CONTENT-ABCDEFGHIJ
---PARTITION-INFO---
OFFSET=65536
SIZE=19857408
---EXTRACT-RAW-VOLUME---
TOTAL_READ=19857408
```

## 场景细节 / 踩坑记录

用 `fsntfsinfo -E all ads_volume.raw` 核实 `ads_test.txt` 对应的是 MFT
索引 38，这条记录有 4 个属性：`$STANDARD_INFORMATION`、`$FILE_NAME`、
无名 `$DATA`（30 字节）、具名 `$DATA`（`Name: secret`，29 字节）——证明
一个真实 Windows ADS 文件的具名数据流和主数据流落在**同一条** MFT
记录里，这次构造没有撞上需要跨记录 `$ATTRIBUTE_LIST` 的场景（ADS 内容
很小，预期之内）。

`make_fixture.py` 翻转这条记录 flags 字段的 in-use 位（和
`ntfs-basic-undelete` 同样的手法），但**不能照抄那边硬编码的
`MFT_START = 16384`**——构造这个 fixture 时实测发现这个新卷的 `$MFT`
物理位置和旧 fixture 不一样：卷最前面偏移 8192 字节其实是 `$MFTMirr`
（前几条 MFT 记录的镜像备份，内容和真正的 `$MFT` 几乎一样，容易被
误当成"就是 MFT 起点"），真正的 `$MFT` 在簇 1616（字节偏移
6619136）。`make_fixture.py` 因此改成每次都从 `$Boot` 扇区的 NTFS BPB
（簇大小/MFT 起始簇号/MFT 记录大小三个字段）重新计算目标偏移，并且
加了一条断言——读出目标偏移处记录自己的"MFT 记录号"自描述字段
（偏移 44，NTFS 3.1+ 才有），确认它等于目标索引本身，不是蒙对了
偏移量。另外还踩到一个相关的坑：直接在整个镜像里搜索 `"FILE"` 魔数字
节试图用"第 N 个出现的位置"定位第 N 条记录，会搜到好几个 rec_num
相同（38）但偏移不同的位置——这是因为 `$LogFile`（NTFS 事务日志）里
也存着这条记录改动前后的历史副本，同一个"记录号"在镜像里可能有多份
拷贝，只有真正用 `$Boot` 算出来的那个偏移才是 libfsntfs 实际会读的
那条活记录，不能靠"搜字符串数第几个"这种办法猜。

## 验证方法（标准答案）

1. 用 `libfsntfs_volume_get_file_entry_by_index` 拿到索引 38 的
   `file_entry`，`libfsntfs_file_entry_is_allocated` 应返回 `false`（0）。
2. `libfsntfs_file_entry_get_number_of_alternate_data_streams` 应返回 1。
3. `libfsntfs_file_entry_get_alternate_data_stream_by_index(0)` /
   `libfsntfs_file_entry_get_alternate_data_stream_by_utf8_name("secret")`
   都应该能拿到同一个 `libfsntfs_data_stream_t`，`get_utf8_name` 读出
   `secret`，`get_size` 读出 29，`libfsntfs_data_stream_read_buffer`
   读出的内容应该和 `ads_test.txt.secret.original` 字节一致。
4. 主数据流（`libfsntfs_file_entry_read_buffer`）应该仍然能正常读出
   30 字节，内容和 `ads_test.txt.original` 一致——已删除文件的主数据流
   和备用数据流互不影响，分别独立验证。

当前哈希（随 fixture 固定，不会变）：

```
4ebbdea868446454eb16e6bde8065ea2fda4b0c9af666f8eab1d7b839a56c66d  ads_test.txt.original
97ff371e1c292ed7ad76a188185aeae34c0e945fd6f97a3c120fea05bd28897c  ads_test.txt.secret.original
```

## 已经验证过一次（探索性验证，不是正式单元测试）

写了一个独立于本项目产品代码的临时探测程序（直接调 libfsntfs 的
`get_number_of_alternate_data_streams`/`get_alternate_data_stream_by_index`/
`get_alternate_data_stream_by_utf8_name`/`data_stream_read_buffer`），跑
这个 fixture 的结果：

```
is_allocated=0
number_of_alternate_data_streams=1
  stream[0] name=secret size=29
  content=ADS-STREAM-CONTENT-ABCDEFGHIJ (total=29)
get_by_utf8_name('secret')=1
named_stream_content=ADS-STREAM-CONTENT-ABCDEFGHIJ (bytes=29)
main_stream_content=MAIN-STREAM-CONTENT-0123456789 (bytes=30)
```

这证实了 J 系列任务描述里"需要在实现过程中真机验证确认的假设"——
已删除文件 MFT 记录上的具名备用数据流属性，和已经验证过的
`$FILE_NAME` 属性一样完整保留，不会因为 `is_allocated()` 返回 0 就
读不到。J1/J2 的正式产品代码实现就是把这条验证路径拆成
`engine/core`/`engine/shell` 里可以被 TDD 覆盖的纯函数，不是一次性
跑完就完事的探测脚本。

## 重新生成

```sh
python3 engine/tests/fixtures/ntfs-ads-undelete/make_fixture.py \
  <ads_volume.raw 路径，真机构造过程见上> \
  engine/tests/fixtures/ntfs-ads-undelete/volume.img
```

`ads_volume.raw` 本身是真机一次性构造产物，没有纳入版本管理（体积和
`volume.img` 本身重复，没有必要留两份）；`volume.img` 本身已经是最终
产物，日常开发/测试不需要重新生成。
