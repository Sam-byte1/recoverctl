/*
 * G3（P6 GUI壳层，2026-11-06小树裁决）：后台线程worker，承担"扫描/恢复"
 * 这条主干流程——继承QObject、配合moveToThread使用（不是继承QThread
 * 重写run()的旧写法，小树裁决④明确要求的写法）。
 *
 * G6（VSS接入GUI，2026-11-06小树裁决）：scan/recover/generateThumbnail
 * 三个槽函数的参数从裸的`QString devicePath`改成`ScanSource`——
 * `ScanSourceType::DirectDevice`涵盖"直接scan/recover"（G3，卷盘符
 * 路径）和"在线VSS卷影副本"（G6，两者走完全相同的`ntfs_bfio_adapter`
 * 路径，差异只是设备路径字符串形态，见scan_source.h头注释）；
 * `ScanSourceType::VssOffline`是离线VSS，走`libvshadow_volume_open`→
 * `get_store`→`vshadow_bfio_adapter_initialize`这条不同的bfio构造
 * 路径。不包含scan-partition/recover-partition（按偏移，针对未挂载
 * 分区，UI流程不同，独立的一刀）、不包含PhotoRec（原子"搜索+写入"
 * 操作，没有候选列表概念，不套用这套抽象，小树裁决维持原判断）。
 *
 * G7（BitLocker接入GUI，2026-11-06小树裁决C）：`ScanSourceType::
 * BitLocker`走和VSS同构的bfio构造路径，但多一步"解锁"——凭据内容绝不
 * 进入`ScanSource`，只通过`scanBitLocker()`这一个专门的槽函数、用
 * 一次性的`BitLockerCredentialInput`参数传入，解锁成功后缓存"已解锁
 * 会话"（`m_bitlockerVolume`/`m_bitlockerUnlockBfioHandle`，不含任何
 * 凭据内容）供同一轮后续的`recover()`/`generateThumbnail()`复用，不
 * 需要为每个候选项重新问一次凭据。会话在两个时机释放：下一次
 * `scanBitLocker()`开始前、本对象析构时——不依赖进程退出时操作系统
 * 兜底回收。`m_bitlockerVolume`/`m_bitlockerUnlockBfioHandle`用
 * `intptr_t*`存储（和`libbde_volume_t`/`libbfio_handle_t`的真实typedef
 * 完全一致，见third_party/lgpl-dynamic/libbde/libbfio/libbfio_types.h
 * 等文件），不在这个Qt头文件里include任何libbde/libbfio头文件。
 */
#ifndef SCAN_WORKER_H
#define SCAN_WORKER_H

#include <cstdint>

#include <QObject>
#include <QPixmap>
#include <QString>
#include <QVector>

#include "candidate_item.h"
#include "scan_source.h"

class ScanWorker : public QObject
{
	Q_OBJECT

public:
	explicit ScanWorker( QObject *parent = nullptr );
	~ScanWorker() override;

	/* 真正emit progress信号的公开方法——C侧的进度回调trampoline（普通
	 * 非成员static函数，见scan_worker.cpp）拿到user_data（就是这个
	 * ScanWorker实例）之后调用这个方法，不直接碰signals段（Qt生成的
	 * 信号访问权限是protected，非成员函数够不到，这个公开方法是唯一
	 * 的桥）。 */
	void notifyProgress( quint64 current, quint64 total );

public slots:
	/* source.type==DirectDevice：复刻cli/main.c command_scan()的自动
	 * 探测分发，依次probe NTFS→FAT32→exFAT。
	 * source.type==VssOffline：VSS卷影副本内容固定是NTFS（Windows系统
	 * 卷的卷影副本不会是FAT32/exFAT），不走probe，直接按NTFS处理。
	 * 扫描完成后统一发scanFinished信号，不管走的是哪条路径。 */
	void scan( ScanSource source );

	/* item.fsType决定在DirectDevice分支里走哪条文件系统恢复路径；
	 * source.type决定整体走DirectDevice还是VssOffline这条bfio构造
	 * 路径。直接用item里已经携带的定位参数（mftIndex 或
	 * firstCluster+size+noFatChain），不重新扫描一遍去用序号查——这是
	 * GUI比CLI多出来的一个优势：CLI的recover命令是独立进程调用，拿不到
	 * 上次scan在内存里的结果，只能靠"候选项序号"重新扫一遍再按下标取；
	 * GUI是同一个长驻进程，扫描结果本来就还在内存里，没有理由绕回
	 * "重新扫描+按序号取"这条更脆弱的路径（万一两次扫描之间磁盘内容
	 * 变了，序号对应的候选项就可能对不上）。 */
	void recover( ScanSource source, CandidateItem item, const QString &outputPath );

	/* H2（批量恢复，2026-11-06小树裁决）：多选恢复的入口——直接复用
	 * recoverDirectDevice/recoverVssOffline/recoverBitLocker这三个
	 * 既有私有函数内部循环，recover()一个字不改。不新建"一次性打开卷、
	 * 循环取多个文件"的共享会话机制——BitLocker那条路径本来就通过
	 * m_bitlockerVolume缓存会话自然复用，DirectDevice/VssOffline每个
	 * 文件重新打开一次卷的开销，在"批量恢复几十个文件"这个量级上可以
	 * 接受，不是要优化的目标（小树裁决：避免GUI侧N次跨线程调用对表格
	 * 行号的竞态风险，比榨干每次恢复的I/O开销更重要）。 */
	void recoverBatch( ScanSource source, QVector<CandidateItem> items, QString outputDir );

	/* G4b（2026-11-06小树裁决①②）：懒加载缩略图——选中某一项才调用，
	 * 不在scan完成时批量生成。复用现有recoverXxx写到一个QTemporaryFile，
	 * 再用QImageReader解码——不新建"读到内存buffer、不落盘"的轻量引擎
	 * 接口，这是在没有证明存在的性能问题上超前投资，等真实测出瓶颈
	 * 再优化不迟（裁决②原话）。row原样回传，GUI那边不需要自己追踪
	 * "这是哪一次请求的结果"。 */
	void generateThumbnail( int row, ScanSource source, CandidateItem item );

	/* G7：唯一需要凭据内容的入口——scan/recover/generateThumbnail三个
	 * 槽函数的ScanSource参数永远不含凭据，BitLocker分支的"扫描"必须
	 * 专门走这个槽函数，用一次性的BitLockerCredentialInput现场解锁。
	 * 解锁成功后，source.type==BitLocker的后续recover()/
	 * generateThumbnail()调用会复用这里缓存下来的已解锁会话，不会
	 * 再弹一次凭据输入。 */
	void scanBitLocker( ScanSource source, BitLockerCredentialInput credential );

signals:
	/* total==0表示"总数未知"（FAT32/exFAT目录树递归遍历场景），UI侧
	 * 应该用QProgressBar的不确定模式表现，不要假装total是一个真实值。 */
	void progress( quint64 current, quint64 total );
	void scanFinished( bool success, const QString &errorMessage, const QVector<CandidateItem> &candidates );
	/* badSectorCount/badSectorBytes：这次恢复过程中有多少处读取失败后
	 * 0填充跳过、总共跳过了多少字节——success==true不代表内容完整，
	 * 这两个字段不为0时，文件虽然"恢复成功"（流程走完了），但内容
	 * 本身已知不完整（2026-11-06小树裁决补上的既有数据，不是新功能） */
	void recoverFinished( bool success, const QString &errorMessage, int badSectorCount, quint64 badSectorBytes );
	/* H2：批量恢复逐项完成通知——index是items里的下标（不是表格行号，
	 * 调用方自己维护这个映射），outputPath是这一项实际写到的路径（可能
	 * 因为重名被改过文件名，调用方想展示"恢复到了哪"需要这个真实路径，
	 * 不是item.name本身）。 */
	void recoverBatchItemFinished( int index, bool success, const QString &errorMessage, const QString &outputPath, int badSectorCount, quint64 badSectorBytes );
	/* totalBadSectorCount/totalBadSectorBytes是这一批所有item的坏道
	 * 累加和，不是"有多少个item带坏道"——调用方想知道具体是哪几个文件
	 * 受影响，去看每一项的recoverBatchItemFinished就够了，汇总信号只
	 * 给一个总览数字 */
	void recoverBatchFinished( int successCount, int totalCount, const QStringList &failedNames, int totalBadSectorCount, quint64 totalBadSectorBytes );
	/* success==false时reason区分两种不同性质的失败（小树裁决③）：
	 * "unsupported"——文件扩展名不在QImageReader::supportedImageFormats()
	 * 范围内，这项根本没有预览能力，不是这个文件出了什么问题；
	 * "decode-failed"——扩展名看着像图片，但实际恢复出来的字节解码
	 * 失败（内容已经损坏/被部分覆盖，这是数据恢复场景下的正常情况，
	 * 不是bug）。GUI侧要能分别展示这两种状态，不能都显示成同一个笼统
	 * 占位图标。 */
	void thumbnailReady( int row, bool success, const QString &reason, const QPixmap &pixmap );

private:
	bool scanDirectDevice( const QString &devicePath, QVector<CandidateItem> &outCandidates, QString &outError );
	bool scanVssOffline( const QString &imagePath, int storeIndex, QVector<CandidateItem> &outCandidates, QString &outError );
	/* 要求m_bitlockerVolume已经是一个对得上source.devicePath/baseOffset
	 * 的有效会话——不在这里现场问凭据（凭据只在scanBitLocker()里问），
	 * 会话不存在/不匹配就返回false+错误信息，提示调用方需要先重新走
	 * 一次scanBitLocker() */
	bool scanBitLockerWithSession( const ScanSource &source, QVector<CandidateItem> &outCandidates, QString &outError );

	bool scanNtfs( const QString &devicePath, QVector<CandidateItem> &outCandidates, QString &outError );
	bool scanFat32( const QString &devicePath, QVector<CandidateItem> &outCandidates, QString &outError );
	bool scanExFat( const QString &devicePath, QVector<CandidateItem> &outCandidates, QString &outError );

	/* 2026-11-06（小树裁决：坏道计数上报补齐——世杰真机反馈部分恢复出的
	 * 图片打不开，排查后确认`ntfs_bfio_adapter.c`等既有代码早就在记录
	 * 读取失败后0填充跳过的区间，只是从没把这份数据传出ScanWorker之外，
	 * GUI完全看不到。这6个恢复函数新增`outBadSectorCount`/
	 * `outBadSectorBytes`两个出参——不是新采集什么数据，是把已经在采集、
	 * 一直被`bad_sector_log_free()`原地丢弃的数据取出来传给调用方，
	 * 照抄`CarveWorker::carveFinished`已经验证过的既有模式）。 */
	bool recoverDirectDevice( const QString &devicePath, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes );
	bool recoverVssOffline( const QString &imagePath, int storeIndex, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes );
	bool recoverBitLocker( const ScanSource &source, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes );

	bool recoverNtfs( const QString &devicePath, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes );
	bool recoverFat32( const QString &devicePath, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes );
	bool recoverExFat( const QString &devicePath, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes );

	/* G7：释放已解锁会话（两个触发点：下一次scanBitLocker()开始前、
	 * 本对象析构时），空会话调用是安全的no-op */
	void releaseBitLockerSession();

	bool m_bitlockerSessionActive = false;
	QString m_bitlockerSessionDevicePath;
	quint64 m_bitlockerSessionBaseOffset = 0;
	/* 不含任何凭据内容，只是"已经解锁打开的卷"——类型见本文件顶部注释 */
	intptr_t *m_bitlockerVolume = nullptr;
	intptr_t *m_bitlockerUnlockBfioHandle = nullptr;
};

#endif /* SCAN_WORKER_H */
