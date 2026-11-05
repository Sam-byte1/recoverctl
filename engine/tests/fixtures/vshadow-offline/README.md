# vss.raw 测试镜像来源说明

和其它 fixture 目录不同，这个目录下的 `vss.raw` **不是我们自己手工构造的**（VSS 的磁盘格式从未被微软公开过规范，是 libyal 项目反向工程出来的，手工按规范拼字节的方式在这里不可行，和 FAT32/exFAT/NTFS 那几个 fixture 的做法不一样）。

- **来源**：[log2timeline/dfvfs](https://github.com/log2timeline/dfvfs) 项目的 `test_data/vss.raw`，和 `third_party/lgpl-dynamic/libvshadow/synctestdata.sh` 自己用来拉测试数据的是同一个文件，不是我们额外去找的。
- **下载地址**：`https://raw.githubusercontent.com/log2timeline/dfvfs/main/test_data/vss.raw`
- **许可证**：dfvfs 项目整体采用 Apache-2.0（通过 `https://raw.githubusercontent.com/log2timeline/dfvfs/main/LICENSE` 核实），允许自由使用，需遵守署名条款。
- **下载日期**：2026-11-05（第24步调研，小树裁决排入 libvshadow 离线解析这条独立战线时批准使用）。
- **内容**：一个真实的、包含 2 个 VSS 卷影副本（store）的卷镜像，78MB（82771968 字节），用官方 `vshadowinfo` 工具验证过能正确解析出两个 store 的身份标识/创建时间等元数据。

重新下载的方式（如果这份文件丢失或需要确认完整性）：

```sh
curl -L -o vss.raw https://raw.githubusercontent.com/log2timeline/dfvfs/main/test_data/vss.raw
```
