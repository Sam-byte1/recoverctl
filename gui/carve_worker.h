/*
 * G8（PhotoRec接入GUI，2026-11-06小树裁决）：独立于ScanWorker的worker
 * 类+独立QThread（裁决④）——PhotoRec雕刻是原子"搜索+写入"操作，没有
 * 候选列表概念，不复用ScanSource/CandidateItem这套抽象；耗时量级
 * （分钟到小时级）和ScanWorker现有操作（秒到几十秒级）差了几个数量级，
 * 如果共享ScanWorker的单线程事件队列，雕刻运行期间发给ScanWorker的
 * 其它请求会全部排在雕刻后面等——裁决①"非模态、雕刻时仍可用其它功能"
 * 会在线程层面落空（窗口不模态，但点了也没反应）。
 *
 * startCarve()本身是阻塞调用（内部`photorec_invoker_run_async()`+
 * `photorec_invoker_wait()`），运行期间CarveWorker自己这个线程的Qt
 * 事件循环是被占住的——这意味着"取消"请求不能走普通的排队信号/槽
 * （那样会排在startCarve()返回之后才被处理，等于没有取消能力）。
 * requestCancel()刻意设计成一个可以从GUI线程直接调用的普通C++方法
 * （不是Qt槽），内部用互斥锁保护"句柄是否已经就绪"这件事，真正调用
 * `photorec_invoker_cancel()`——engine层已经把这个函数设计成"可以从
 * 另一个线程安全调用"（裁决③），这里的锁只是保证读到的handle指针
 * 不是野的，不是在重新实现cancel()本身的线程安全。
 *
 * G8遗留项收尾（2026-11-06小树裁决：补recover-carve-partition GUI
 * 支持）：新增`startCarvePartition()`，对应CLI的`recover-carve-partition`
 * ——先用`engine/shell/partition_extractor.h`（2026-11-06从`cli/main.c`
 * 内部static函数抽出来的共享模块）把给定字节范围拷到临时文件，再对
 * 这个临时文件走和`startCarve()`完全一样的雕刻逻辑。**已知范围限制**：
 * 拷贝这一步（`partition_extractor_extract_to_temp_file`）本身不支持
 * 中途取消——`requestCancel()`在这一步只会设置`m_cancelRequested`标记，
 * 真正生效要等到拷贝完成、PhotoRec子进程真正启动之后。这不是遗漏，是
 * 刻意的范围边界：让拷贝本身支持取消需要改`aligned_device_reader_
 * copy_to_file`这个被`fat_reader.c`/`exfat_reader.c`等多处已验证代码
 * 共用的核心函数，风险和收益不成比例，不在这次"补技术完整性缺口"的
 * 范围内；真要动，应该是一次独立评估过的改动，不是顺手夹带。 */
#ifndef CARVE_WORKER_H
#define CARVE_WORKER_H

#include <QMutex>
#include <QObject>
#include <QString>

class CarveWorker : public QObject
{
	Q_OBJECT

public:
	explicit CarveWorker( QObject *parent = nullptr );
	~CarveWorker() override;

	/* 线程安全——从GUI线程直接调用，不是排队槽调用，见上方文件头
	 * 注释的理由 */
	void requestCancel();

public slots:
	/* 阻塞直到PhotoRec退出或者被requestCancel()强制终止——必须跑在
	 * 自己专属的QThread上，不能和ScanWorker共享线程 */
	void startCarve( QString photorecExePath, QString targetPath, QString outputDirBase );

	/* 对应cli/main.c的recover-carve-partition——先拷贝字节范围到临时
	 * 文件，再调用和startCarve()相同的雕刻逻辑，见上方文件头注释的
	 * 范围限制（拷贝阶段不支持取消）。offset和CLI的`long offset`参数
	 * 同一类型/同一含义（分区在设备上的字节偏移）。 */
	void startCarvePartition( QString photorecExePath, QString devicePath, qint64 offset, quint64 size, QString outputDirBase );

signals:
	/* cancelled==true时success必然是false；recoveredFileCount是
	 * report.xml里解析出的<fileobject>总数，cancelled或者wait失败时
	 * 是0（没有去读report.xml，PhotoRec可能连report.xml都没来得及
	 * 写完整）；badSectorCount是分区拷贝阶段记录的坏道数量，只有
	 * startCarvePartition()这条路径会非0，startCarve()固定是0 */
	void carveFinished( bool success, bool cancelled, const QString &errorMessage, const QString &recupDirPath, int exitCode, int recoveredFileCount, int badSectorCount );

private:
	/* startCarve()/startCarvePartition()共用的"跑一次PhotoRec+解析
	 * report.xml+发carveFinished信号"逻辑，badSectorCount原样透传给
	 * 信号（startCarve()传0，startCarvePartition()传分区拷贝阶段记录
	 * 的真实数量） */
	void runCarveAndEmit( const QString &photorecExePath, const QString &targetPath, const QString &outputDirBase, int badSectorCount );

	QMutex m_handleMutex;
	/* 不透明指针——真实类型是engine/shell/photorec_invoker.h的
	 * photorec_invoke_handle_t（C侧用匿名struct+typedef定义，没有
	 * struct标签可供这个Qt头文件单独前置声明），在.cpp里reinterpret_cast
	 * 回真实类型使用，不在这个头文件里引入任何engine C头文件——和
	 * scan_worker.h存BitLocker已解锁会话用intptr_t*同样的理由 */
	void *m_activeHandle;
	bool m_cancelRequested;
};

#endif /* CARVE_WORKER_H */
