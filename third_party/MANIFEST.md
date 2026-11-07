# 第三方开源组件清单（许可证合规基线）

> 所有仓库均为 `--depth 1` 浅克隆，只取最新一次提交作为参考基线。正式集成前请按下方「集成方式」栏的约束使用，不要越界（尤其是 GPL 分类）。

## A. mit-embeddable — MIT，可直接拷贝源码进闭源产品

| 仓库 | 本地路径 | commit | 日期 | 用途 |
|---|---|---|---|---|
| thewhiteninja/ntfstool | `mit-embeddable/ntfstool` | 998964d | 2026-06-26 | 调研阶段登记的参考实现（NTFS 已删除记录识别、BitLocker 解密），**实际 NTFS 已删除记录识别最终直接用的是 `libfsntfs_file_entry_is_allocated()`，没有参考/拷贝这个仓库的代码**（详见 docs/开发日志.md）；BitLocker 部分尚未开始，仍按原计划保留在这里备用 |

**集成方式**：可直接复制/修改源码并静态编译进商业闭源产品，只需保留 MIT 版权声明。

---

## B. lgpl-dynamic — LGPL-3.0，以动态库形式集成（libyal 系列，Joachim Metz 维护）

| 仓库 | 本地路径 | commit | 日期 | 用途 |
|---|---|---|---|---|
| libyal/libfsntfs | `lgpl-dynamic/libfsntfs` | faaa031 | 2026-10-11 | NTFS 文件系统解析核心引擎（$MFT、属性、已删除记录） |
| libyal/libfsfat | `lgpl-dynamic/libfsfat` | 6bafe8b | 2026-11-04 | 调研阶段登记为"FAT12/16/32、exFAT 解析"，**实际读完全部公开 API + README 后确认它不提供已删除记录判定能力**（README 原话"currently only focuses on the analysis of the format"），只能遍历完好文件系统里仍存活的目录树。FAT32/exFAT 的已删除记录判定、引导扇区/目录项解析全部改为自研（基于公开规范，不拷贝这个仓库任何代码），libfsfat 降级为辅助库（预留给"读取仍存活文件/目录树做预览"场景，目前还没有用到），详见 docs/开发日志.md 2026-11-04 相关记录 |
| libyal/libvshadow | `lgpl-dynamic/libvshadow` | fa4776f | 2026-10-11 | 卷影副本(VSS)离线解析，找回历史版本 |
| libyal/libbde | `lgpl-dynamic/libbde` | 4343f14 | 2026-11-01 | BitLocker 加密卷解密 |
| libyal/libvsgpt | `lgpl-dynamic/libvsgpt` | 313890b | 2026-10-28 | GPT 分区表解析 |
| libyal/libvsmbr | `lgpl-dynamic/libvsmbr` | ec39c3b | 2026-10-11 | MBR 分区表解析 |
| libyal/libfsext | `lgpl-dynamic/libfsext` | ab79209 | 2026-11-03 | ext2/3/4（可选，跨平台 U 盘场景） |

**集成方式**：编译为独立 `.dll`，运行时动态加载/链接；不要把源码直接静态编译进主 exe。若修改了库本身源码，该修改部分需按 LGPL 开源。应用主程序和 UI 代码本身可保持闭源商用。

**libfsntfs 在 Windows 平台的特殊处理（2026-11-05 记录，详见 docs/开发日志.md 第20步执行记录）**：本项目为实现 NTFS 直接 scan/recover 的坏道容忍，需要向 libfsntfs 注入自定义 `libbfio_handle`（`engine/shell/ntfs_bfio_adapter.c`）。排查发现 Windows 版 `libfsntfs-1.dll` 不会导出它内部依赖的 libbfio 层符号（Windows DLL 默认不导出任何符号，和 Unix 共享库默认导出所有符号的行为相反，libfsntfs 只给公开 API 标了导出宏），导致 `recoverctl.exe` 无法直接链接这几个符号。**解决方式不是把这几个文件的源码编译进 `recoverctl.exe` 本体**（那样等同于把 LGPL 代码静态链接进闭源主程序，违反上面的规则），而是新增一个独立的 Windows-only 桥接 DLL（`ntfs_bfio_bridge.dll`），只编译 libfsntfs 发行包自带的 `libbfio_handle.c` + 3 个 `libcdata` 文件（`libcdata_range_list.c`/`libcdata_range_list_value.c`/`libcdata_list_element.c`）+ `libcerror_error.c`（libbfio 层自身依赖的最小文件集合），未修改任何源码内容，未引入任何新的第三方依赖，用和 libfsntfs 本体完全相同的编译时宏配置（`-DHAVE_CONFIG_H=1` + 相同 `common/config.h`）保证跨 DLL 传递的不透明指针（`libbfio_handle_t`）结构体内存布局一致。这个桥接 DLL 本身仍然是独立 `.dll`、被 `recoverctl.exe` 动态链接，不违反 lgpl-dynamic 规则——只是把同一份 LGPL 代码，从"间接依赖 libfsntfs.dll 内部实现"变成"显式编译成第二个独立 DLL"，没有任何代码被静态编译进主程序。构建脚本目前位于 VM 本地暂存目录（尚未纳入项目版本管理，已排入第21步处理），正式归档路径确定后需要在此补充引用。

**Qt 6（P6 GUI 壳层技术选型，2026-11-05 小树裁决，详见 docs/开发日志.md 对应记录）**：

| 组件 | 获取方式 | 版本 | 用途 |
|---|---|---|---|
| Qt 6（Core / Gui / Widgets / Multimedia + Multimedia-WMF 后端） | MSYS2 CLANGARM64 子系统官方预编译包（`pacman -S mingw-w64-clang-aarch64-qt6-base mingw-w64-clang-aarch64-qt6-multimedia mingw-w64-clang-aarch64-qt6-multimedia-wmf`），**不**克隆 Qt 源码到本仓库 | 6.12.0（2026-11-05 实测核实该版本在 CLANGARM64 仓库可用） | P6 Windows GUI 产品壳层（窗口/控件/图片缩略图解码/视频首帧缩略图） |

**和 libyal 系列的关键区别**：libyal 各库是本项目自己 clone 源码、自己交叉编译；Qt 这里直接消费 MSYS2 项目官方从 Qt 官方 LGPL 开源源码构建好的预编译动态库，不在本仓库保留 Qt 源码副本，也不自己编译 Qt。集成方式仍按 lgpl-dynamic 规则执行：只用动态链接（`.dll` 运行时加载，随安装包用 `windeployqt` 收集依赖），不修改 Qt 源码，分发时附 Qt 的 LGPLv3 许可证全文 + 指向 Qt 官方开源下载页的源码获取方式说明。**只使用 Core/Gui/Widgets/Multimedia 这几个核心模块**，明确不使用 Qt 的 GPL-only / 商用双授权附加模块（例如 Qt Charts 是 GPL 或商业授权，不在 LGPL 范围内，不得用）。视频缩略图固定使用 `qt6-multimedia-wmf`（Windows Media Foundation 后端，调用的是 Windows 系统自带解码器），**不装** `qt6-multimedia-ffmpeg`，避免引入 FFmpeg 这个需要单独走一轮许可证分类的新依赖（FFmpeg 按编译选项不同可能落在 LGPL 或 GPL，没有必要为了缩略图这个次要功能新开一条许可证排查战线）。

**待核实事项，不是已验证结论（2026-11-05 记录，留给 G5 打包阶段动手前核实）**：`pacman -Si mingw-w64-clang-aarch64-qt6-base` 的 `Licenses` 字段是一行聚合 SPDX 表达式（`LGPL-3.0-only WITH Qt-GPL-exception-1.0 AND AFL-2.1 AND Apache-2.0 AND ... AND GPL-2.0-only AND GPL-2.0-or-later AND GPL-3.0-only AND custom`），这是整个 Qt 源码打包（含内部工具/测试/文档/示例等各种许可证的文件）的汇总，**不等于**我们实际要分发的 `Qt6Core.dll`/`Qt6Gui.dll`/`Qt6Widgets.dll`/`Qt6Multimedia.dll` 这几个运行时库本身是 GPL——这和 `cpl-modular/sleuthkit` "仓库混有 GPL 标注文件，只能用非 GPL 标注部分" 是同一类风险形态。目前的裁决依据是业界长期公认的惯例（QtCore/Gui/Widgets/Multimedia 等核心模块走 LGPL-3.0 路线，这是大量闭源商业 Windows 桌面软件使用 Qt 的标准方式），**不是本项目自己逐文件核实过的结论**。正式打包分发前，必须去 Qt 官方每个模块的 LICENSE 文件 / 官方许可证说明页逐一核实我们实际链接和分发的具体 DLL 确实落在 LGPL-3.0 范围内，不能只凭 pacman 这一行聚合字符串就当作已验证完毕。另外 `qt6-base` 会动态依赖 `openssl`（Apache-2.0，宽松许可，暂不构成合规风险，但如果最终产品不需要 QtNetwork/TLS 相关功能，G5 打包时可以评估是否能从分发清单里裁掉这个 DLL）。

**[2026-11-06 小树裁决，触发条件已满足]**：GUI 壳层 G1-G4 已全部完成并真机验证通过，现在正式进入 G5 打包阶段——这段话写的触发条件（"G5 打包阶段动手前核实"）已经到达，排为 G5a（绿色版打包）第一项任务，不再继续往后推，详见 `docs/开发规划.md`/`docs/开发日志.md` 对应记录。待核实的具体对象：`gui/build_windows.ps1` 实际拷贝到产物目录的全部 DLL（含目前还没查清楚为什么被 `windeployqt` 部署进来的 `Qt6Network.dll`——GUI 代码目前只链接了 `Qt6Widgets`，如果确认不需要网络功能，应该连带它可能依赖的 `openssl` 一起从分发清单里拿掉）+ MSYS2 工具链运行时 DLL（`libc++`/`libicu*` 等，此前只验证过它们能正常运行，没有专门核实过许可证条款）。

**[2026-11-06 核实完成]**：

- **`Qt6Network.dll` 疑点已解决，不再分发**：排查确认是 `windeployqt` 默认扫描全部 `generic` 类插件（触屏/平板输入插件，和本项目桌面场景完全无关），其中 `qtuiotouchplugin.dll` 依赖 `Qt6Network`，才被连带部署进来——不是 GUI 代码本身需要网络功能。`gui/build_windows.ps1` 已加 `windeployqt --skip-plugin-types generic`，真机验证应用照常运行不受影响，`Qt6Network.dll`/`networkinformation/`/`tls/` 不再出现在产物目录。因此这次核实的分发清单里**不包含** `Qt6Network.dll`，也就不需要再追查它是否依赖 OpenSSL。
- **Qt6Core/Gui/Widgets 逐模块核实结论**：去官方 `doc.qt.io/qt-6/licensing.html` 核实确认这三个模块属于 Qt Essentials，走 **LGPL-3.0-only**（不是 GPL-only、不是只能商用授权）。动态链接场景下对分发方的义务：提供 LGPLv3 许可证全文、告知用户用了 LGPL 组件；"允许用户替换库"这条因为本来就是独立 `.dll` 动态加载，天然满足，不需要额外工作。**之前"业界惯例推测、未逐一核实"的待办状态到此解除**。
- **MSYS2 工具链运行时 DLL 逐一核实结论**（这批不是本项目 vendor 的代码，是 Qt/MSYS2 生态的间接运行时依赖，这次是第一次正式核实）：

  | 库 | 真实 SPDX 许可证 | 动态链接分发义务 |
  |---|---|---|
  | libc++.dll | Apache-2.0 WITH LLVM-exception | 保留版权声明 |
  | libiconv-2.dll | LGPL-2.1-or-later | 提供许可证全文 |
  | libintl-8.dll | LGPL-2.1-or-later | 提供许可证全文 |
  | libglib-2.0-0.dll | LGPL-2.1-or-later | 提供许可证全文 |
  | libgraphite2.dll | 三选一（LGPL-2.1-or-later / GPL-2.0-or-later / MPL-2.0），本项目按 LGPL-2.1-or-later 使用 | 提供许可证全文 |
  | libpcre2-8-0.dll / libpcre2-16-0.dll | BSD-3-Clause（含 PCRE2-exception） | 保留版权声明 |
  | libzstd.dll | 双选（BSD-3-Clause / GPL-2.0-or-later），本项目按 BSD-3-Clause 使用 | 保留版权声明 |
  | zlib1.dll | Zlib License | 不得谎称来源；改动需标注；保留声明 |
  | libbz2-1.dll | bzip2-1.0.6（BSD 风格） | 保留版权声明；改动需标注 |
  | libbrotlicommon.dll / libbrotlidec.dll | MIT | 保留版权声明 |
  | libpng16-16.dll | libpng license v2（permissive） | 保留版权声明 |
  | libfreetype-6.dll | 二选一（FTL / GPL-2.0），本项目按 FTL 使用 | **要求在文档里包含指定信用声明文字**："Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved."——这是本次核实里唯一一条"必须包含特定文字"的强制条款，不是可选鸣谢，正式对外分发前不能漏 |
  | libharfbuzz-0.dll | MIT | 保留版权声明 |
  | libdouble-conversion.dll | BSD-3-Clause | 保留版权声明 |
  | libmd4c.dll | MIT | 保留版权声明 |
  | libb2-1.dll | 三选一（CC0 / OpenSSL License / Apache-2.0），本项目按 CC0 使用 | 无强制条款（CC0 等同公有领域） |

  **结论：这批 DLL 全部是宽松许可证或弱 copyleft（LGPL 系列），没有一个要求公开本项目自身源代码的强 copyleft 许可证（GPL/AGPL）**——和 `third_party/` 下 `gpl-subprocess-only`（PhotoRec/testdisk，只能独立子进程调用）是完全不同的风险类别，这批可以正常动态链接分发。

- **分阶段的合规义务**：
  - **用于内部技术验证的绿色版压缩包（不对外公开）**：以上这些许可证的义务都是"对外分发给真实终端用户时"才触发，内部验证阶段**不需要**先补许可证全文/NOTICE 文件就可以直接打包使用。
  - **以后要真正对外公开分发时，还缺的具体合规动作**（留给 G5b 或者正式对外发布前处理，现在先记下来）：① 新建一个 `THIRD_PARTY_LICENSES.txt` 汇总上表每个库的许可证全文（含 LGPL-3.0/LGPL-2.1/MIT/BSD-3-Clause/zlib/bzip2-1.0.6/PCRE2 等各自完整文本）；② **FreeType 的信用声明是强制项，必须单独确认写进去**；③ Qt 的 LGPL-3.0"告知用户使用了 LGPL 组件"这条义务要体现在产品"关于"页面这类用户能看到的地方，不能只塞进一个没人会打开的 txt 文件里；④ 对三选一/双选的库（graphite2/zstd/freetype/libb2），`THIRD_PARTY_LICENSES.txt` 里要明确写清楚本项目实际选择的是哪一个许可证，不能含糊写"多选一"。

---

## C. cpl-modular — Common Public License 1.0，弱 copyleft，模块化集成

| 仓库 | 本地路径 | commit | 日期 | 用途 |
|---|---|---|---|---|
| sleuthkit/sleuthkit | `cpl-modular/sleuthkit` | 0c3cc1e | 2026-10-27 | TSK 库：卷/文件系统通用解析参考 |

**重要**：该仓库内部混有 `licenses/GNUv2-COPYING`、`licenses/GNUv3-COPYING` 标注的 GPL 文件（通常是可选的 libewf/afflib 集成代码）。**只使用 `tsk/` 目录下明确标注 CPL-1.0 / Apache-2.0 / MIT / BSD 的源文件**，引入前逐文件确认头部版权声明，避免误用 GPL 标注文件把 CPL 模块拖入强 copyleft。

---

## D. gpl-subprocess-only — GPL-2.0/3.0，严禁拷贝源码，只能以独立可执行文件子进程调用

| 仓库 | 本地路径 | commit | 日期 | 用途 |
|---|---|---|---|---|
| cgsecurity/testdisk | `gpl-subprocess-only/testdisk` | f9e57dc | 2026-10-25 | PhotoRec 文件雕刻兜底引擎 |
| tuxera/ntfs-3g | `gpl-subprocess-only/ntfs-3g` | 7f0f841 | 2026-09-28 | ntfsundelete 等工具参考 |

**集成方式（必须遵守）**：
1. 不静态/动态链接其代码，不拷贝任何源码片段进主程序
2. 编译出的 `.exe` 作为独立子进程调用（命令行参数驱动，解析其输出文件/stdout）
3. 随安装包分发该 `.exe` 时，必须附带其 GPL 许可证全文、保留原始版权头、提供对应版本源码的获取方式（可直接链接到此处 commit 的 GitHub 地址）
4. 不得对这些 `.exe` 做任何修改后以「自己的工具」名义重新分发，除非该修改版本本身也以 GPL 开源

---

## E. apache-reference — Apache-2.0，架构/交互设计参考（非代码复用优先级）

| 仓库 | 本地路径 | commit | 日期 | 用途 |
|---|---|---|---|---|
| sleuthkit/autopsy | `apache-reference/autopsy` | cb3dacd | 2026-05-04 | ingest module 流水线设计、取证 GUI 交互思路参考 |

**集成方式**：Apache-2.0 本身允许直接拷贝代码进闭源产品，但 Autopsy 是 Java + NetBeans RCP 技术栈，与 Windows 原生桌面产品形态不匹配，建议仅参考其架构设计，不做代码级复用。

---

## 待办

- [ ] 商用发布前，将本清单交给熟悉 FOSS 许可的律师做一次正式审查（尤其是 CPL 模块的"模块化组合"边界判定）
- [ ] 为 D 类 GPL 工具的分发包准备独立的 `THIRD_PARTY_NOTICES.txt`，附许可证全文
- [ ] 跟踪各仓库上游版本更新，定期（建议每季度）重新拉取并复核 diff
