# fixture: ntfs-basic-undelete

最基础的"文件被删除、但数据未被覆盖"测试场景，给 Task #3/#4 的 TDD 用作标准答案。

## 文件说明

| 文件 | 说明 |
|---|---|
| `volume.img` | 4.2MB 的 NTFS 卷镜像，里面有 2 个文件被模拟删除 |
| `make_fixture.py` | 生成 `volume.img` 的脚本，可从源镜像重新生成，结果字节级可复现 |
| `passwords.txt.original` | MFT 索引 66 对应文件的原始内容 |
| `another_file.original` | MFT 索引 67 对应文件（`a_directory\another_file`）的原始内容 |

## 场景细节

基础镜像来自 `third_party/lgpl-dynamic/libfsntfs/tests/input/public/ntfs.raw`（libfsntfs 自己测试套件引用的公开数据，来自 log2timeline/dfvfs 项目，通过 `synctestdata.sh` 拉取）。这个卷里原本就有 5 个真实文件：`a_directory`、`a_directory\a_file`、`a_directory\another_file`、`a_link`、`passwords.txt`，分别对应 MFT 索引 64-68。

`make_fixture.py` 对其中两条 MFT 记录（索引 66 = `passwords.txt`，索引 67 = `a_directory\another_file`）做了一件事：把记录头部 flags 字段的 bit0（in-use 位，偏移 0x16/22）清零，其余字节完全不动。这模拟了真实 Windows 删除文件时文件系统层面实际发生的事——只改元数据标志位，不碰 `$DATA` 指向的簇。

**重要的预期行为**：由于没有修改父目录的索引（$I30），用 `fsntfsinfo -H` 这类按目录遍历的方式看，这两个文件可能还会出现在列表里。这是故意的、符合预期的——真实的数据恢复工具不能依赖目录遍历去找已删除文件（目录索引通常也被一起清掉了），必须按 MFT 索引对全表做扫描，逐条检查分配状态。这正是我们 Task #3/#4 要实现的核心逻辑，这个 fixture 的设计就是逼着实现走这条正确路径，而不是抄近路用目录遍历蒙混过关。

## 验证方法（标准答案）

1. 用 `libfsntfs_volume_get_number_of_file_entries` + 循环 `libfsntfs_volume_get_file_entry_by_index` 遍历全部 69 个 MFT 条目（不走目录索引、不走路径查找）
2. 对每条用 `libfsntfs_file_entry_is_allocated` 检查，索引 66、67 应返回 `false`（0），其余正常文件应返回 `true`（1）
3. 对判定为"未分配但仍有 $FILE_NAME 属性"的条目，读出文件名应该分别是 `passwords.txt` 和 `another_file`
4. 读出其 `$DATA` 内容，SHA256 应该分别与 `passwords.txt.original`、`another_file.original` 一致

当前哈希（随 fixture 固定，不会变）：

```
02a2a6af2f1ecf4720d7d49d640f0d0a269a7ec733e41973bdd34f09dad0e252  passwords.txt.original
c7fbc0e821c0871805a99584c6a384533909f68a6bbe9a2a687d28d9f3b10c16  another_file.original
```

## 已经验证过一次（探索性验证，不是正式单元测试）

`engine/vendor/listdeleted/`（按上面的验证方法实现的一个命令行小工具，不是正式产品代码）跑这个 fixture，结果：恢复出 2 个文件，SHA256 与上面记录的完全一致。这证明了这个 fixture 本身是有效的、整条思路是可行的；Task #3/#4 要做的是把这套逻辑拆成 `engine/core` 里可以被 TDD 覆盖的纯函数，而不是一个一次性跑完就完事的脚本。

## 重新生成

```sh
python3 engine/tests/fixtures/ntfs-basic-undelete/make_fixture.py \
  third_party/lgpl-dynamic/libfsntfs/tests/input/public/ntfs.raw \
  engine/tests/fixtures/ntfs-basic-undelete/volume.img
```
