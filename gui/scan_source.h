/*
 * G6（VSS接入GUI，2026-11-06小树裁决）：一次扫描/恢复操作的"来源"——
 * 独立于CandidateItem（候选项本身只表达"这个文件在哪、怎么定位"）。
 * 由MainWindow持有（和现有m_currentDevicePath同级），ScanWorker的
 * scan/recover/generateThumbnail三个槽函数都接受这个参数，内部按
 * type分发到不同的bfio构造路径。
 *
 * 为什么不塞进CandidateItem（小树裁决理由，不是审美判断）：
 * ①一次扫描的来源信息对这次扫描产出的全部候选项都是同一份，塞进
 * 每个候选项是冗余副本——候选项可能有几十上百个，来源信息就要复制
 * 几十上百份；②以后BitLocker接入时，凭据内容会是这个来源信息的一
 * 部分，副本数量越多，"敏感数据清零"要追踪的副本就越多，这和项目
 * B3"敏感内存主动清零"的既有方向直接冲突。
 *
 * DirectDevice同时覆盖"直接访问设备/卷"和"在线VSS卷影副本"两种场景
 * ——在线VSS卷影副本的特殊设备路径（`\\?\GLOBALROOT\Device\
 * HarddiskVolumeShadowCopyN\`）在CLI层（`cli/main.c`的`scan-shadow`）
 * 本来就是原样传给`command_scan_ntfs`，和直接访问真实设备走的是完全
 * 相同的`ntfs_bfio_adapter`代码路径，"多了一个列出卷影副本的前置
 * 向导"是UI层面的事，不代表引擎对接层面也要分叉出一个新分类（小树
 * 裁决明确纠正过这一点）。
 *
 * G7（BitLocker接入GUI，2026-11-06小树裁决C）：BitLocker分支只放
 * devicePath（物理盘路径）/baseOffset（分区在物理盘上的字节偏移）/
 * credentialType（凭据类型，不是凭据内容）这些不敏感信息——凭据内容
 * 绝不进入ScanSource，只在触发"扫描BitLocker卷"这一个动作时通过
 * BitLockerCredentialInput（见下方）一次性传给ScanWorker，解锁成功后
 * 立刻清零；解锁后得到的"已解锁会话"（不含凭据内容）缓存在ScanWorker
 * 内部，供同一轮后续的recover()/generateThumbnail()复用，不需要为
 * 每个候选项重新问一次48位恢复密钥。
 */
#ifndef SCAN_SOURCE_H
#define SCAN_SOURCE_H

#include <QMetaType>
#include <QString>

enum class ScanSourceType
{
	/* 直接访问一个设备/卷路径——涵盖"直接scan/recover"（G3）和"在线VSS
	 * 卷影副本"（G6）两种场景，devicePath字段对两者都够用，见上方
	 * 文件头注释的理由。 */
	DirectDevice,

	/* 离线VSS——devicePath存镜像文件路径，storeIndex是
	 * libvshadow_volume_get_store的序号。和DirectDevice不同，这条路径
	 * 要走libvshadow_volume_open→get_store→vshadow_bfio_adapter_initialize，
	 * 不是ntfs_bfio_adapter。 */
	VssOffline,

	/* G7新增：BitLocker加密卷——devicePath存物理盘路径（锁定的BitLocker
	 * 卷不能走卷盘符设备对象读取，B9真机验证过的既有限制），baseOffset
	 * 是分区在物理盘上的字节偏移，credentialType是凭据类型。不含任何
	 * 凭据内容，见上方文件头注释。 */
	BitLocker
};

/* 和engine/shell/bitlocker_credential_source.h的
 * bitlocker_credential_type_t一一对应，这里单独定义一份Qt侧枚举，不
 * 直接include那份C头文件——和CandidateFsType不直接复用libfsntfs的
 * 文件系统类型枚举同样的理由，避免把C库类型泄漏进Qt头文件。 */
enum class BitLockerCredentialType
{
	Password,
	RecoveryKey,
	StartupKey,
	Fvek
};

struct ScanSource
{
	ScanSourceType type = ScanSourceType::DirectDevice;

	/* DirectDevice：设备/卷路径（含在线VSS卷影副本的特殊路径）。
	 * VssOffline：镜像文件路径。BitLocker：物理盘路径。 */
	QString devicePath;

	/* 只有VssOffline用——libvshadow_volume_get_store的序号 */
	int storeIndex = 0;

	/* 只有BitLocker用——分区在物理盘上的字节偏移 */
	quint64 baseOffset = 0;
	/* 只有BitLocker用——凭据类型，不是凭据内容 */
	BitLockerCredentialType credentialType = BitLockerCredentialType::Password;
};

Q_DECLARE_METATYPE( ScanSource )

/* G7：一次性凭据输入——只作为"扫描BitLocker卷"这一个请求的参数传递，
 * 绝不进入ScanSource、绝不被MainWindow长期持有（见上方裁决说明）。
 * ScanWorker拿到之后立刻用于解锁，解锁成功或失败都会把这里的文本内容
 * 用secure_memory_zero清零，之后缓存的"已解锁会话"不含这份内容。 */
struct BitLockerCredentialInput
{
	BitLockerCredentialType type = BitLockerCredentialType::Password;

	/* Password/RecoveryKey用——凭据明文内容本身 */
	QString textValue;
	/* StartupKey/Fvek用——文件路径（不是凭据内容本身，不需要清零：
	 * StartupKey是官方.BEK二进制文件路径，直接转给
	 * libbde_volume_read_startup_key；Fvek是文本文件路径，格式
	 * `<FVEK十六进制>:<TWEAK十六进制>`，复用
	 * bitlocker_credential_source_load_from_file做解析，不在GUI侧
	 * 重新实现一遍解析逻辑） */
	QString filePath;
};

Q_DECLARE_METATYPE( BitLockerCredentialInput )

#endif /* SCAN_SOURCE_H */
