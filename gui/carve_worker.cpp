#include "carve_worker.h"

#include <cstring>

#include <QFile>
#include <QMutexLocker>

extern "C" {
#include "../engine/core/bad_sector_log.h"
#include "../engine/core/photorec_report_parser.h"
#include "../engine/shell/partition_extractor.h"
#include "../engine/shell/photorec_invoker.h"
}

CarveWorker::CarveWorker( QObject *parent )
	: QObject( parent )
	, m_activeHandle( nullptr )
	, m_cancelRequested( false )
{
}

/* 正常流程下startCarve()结束时m_activeHandle已经被清空；这里只是
 * 兜底——CarveDialog的closeEvent会先确保没有正在跑的雕刻才允许真正
 * 关闭，理论上不应该在雕刻进行中析构，但不假设调用时序一定完美。 */
CarveWorker::~CarveWorker()
{
	QMutexLocker locker( &m_handleMutex );
	delete reinterpret_cast<::photorec_invoke_handle_t *>( m_activeHandle );
}

void CarveWorker::requestCancel()
{
	QMutexLocker locker( &m_handleMutex );

	m_cancelRequested = true;
	if( m_activeHandle != nullptr )
	{
		photorec_invoker_cancel( reinterpret_cast<::photorec_invoke_handle_t *>( m_activeHandle ) );
	}
}

/* target_path固定用"partition_none"命令串token，和cli/main.c的
 * command_recover_carve同样的理由（2026-11-05第28步小树裁决"B为
 * 机制"）：不让PhotoRec自己重新探测分区表，把整个target_path当一块
 * 不区分分区结构的原始数据去搜索。 */
void CarveWorker::startCarve( QString photorecExePath, QString targetPath, QString outputDirBase )
{
	runCarveAndEmit( photorecExePath, targetPath, outputDirBase, 0 );
}

/* G8遗留项收尾（2026-11-06小树裁决）：对应cli/main.c的
 * recover-carve-partition——先用partition_extractor_extract_to_temp_file
 * （和CLI三处调用点共用同一份实现，2026-11-06从cli/main.c内部static
 * 函数抽出来的）把字节范围拷到临时文件，再走和startCarve()完全一样
 * 的雕刻逻辑。拷贝阶段不支持取消（见carve_worker.h头注释的范围限制
 * 说明），从这一步开始到调用runCarveAndEmit()之前，requestCancel()
 * 只会设置m_cancelRequested标记，不会真的中断拷贝。 */
void CarveWorker::startCarvePartition( QString photorecExePath, QString devicePath, qint64 offset, quint64 size, QString outputDirBase )
{
	char tempPath[ 1024 ];

	if( partition_extractor_get_temp_file_path( tempPath, sizeof( tempPath ) ) != 1 )
	{
		emit carveFinished( false, false, QStringLiteral( "无法确定临时文件路径" ), QString(), 0, 0, 0 );
		return;
	}

	{
		QMutexLocker locker( &m_handleMutex );
		if( m_cancelRequested )
		{
			emit carveFinished( false, true, QStringLiteral( "已取消" ), QString(), 0, 0, 0 );
			return;
		}
	}

	QByteArray devicePathUtf8 = devicePath.toUtf8();
	bad_sector_log_t badLog;

	memset( &badLog, 0, sizeof( badLog ) );

	if( partition_extractor_extract_to_temp_file( devicePathUtf8.constData(), (long) offset, (uint64_t) size, &badLog, tempPath ) != 1 )
	{
		bad_sector_log_free( &badLog );
		emit carveFinished( false, false, QStringLiteral( "拷贝分区内容失败，请检查设备路径/偏移/大小是否正确" ), QString(), 0, 0, 0 );
		return;
	}

	int badSectorCount = badLog.count;
	bad_sector_log_free( &badLog );

	{
		QMutexLocker locker( &m_handleMutex );
		if( m_cancelRequested )
		{
			QFile::remove( QString::fromLocal8Bit( tempPath ) );
			emit carveFinished( false, true, QStringLiteral( "已取消" ), QString(), 0, 0, 0 );
			return;
		}
	}

	runCarveAndEmit( photorecExePath, QString::fromLocal8Bit( tempPath ), outputDirBase, badSectorCount );
	QFile::remove( QString::fromLocal8Bit( tempPath ) );
}

void CarveWorker::runCarveAndEmit( const QString &photorecExePath, const QString &targetPath, const QString &outputDirBase, int badSectorCount )
{
	QByteArray exePathUtf8 = photorecExePath.toUtf8();
	QByteArray targetUtf8 = targetPath.toUtf8();
	QByteArray outputUtf8 = outputDirBase.toUtf8();

	::photorec_invoke_handle_t *handle = new ::photorec_invoke_handle_t();

	{
		QMutexLocker locker( &m_handleMutex );
		if( m_cancelRequested )
		{
			delete handle;
			emit carveFinished( false, true, QStringLiteral( "已取消" ), QString(), 0, 0, badSectorCount );
			return;
		}
		m_activeHandle = handle;
	}

	if( photorec_invoker_run_async( exePathUtf8.constData(), targetUtf8.constData(), "partition_none", outputUtf8.constData(), handle ) != 1 )
	{
		QMutexLocker locker( &m_handleMutex );
		m_activeHandle = nullptr;
		delete handle;
		emit carveFinished( false, false, QStringLiteral( "无法启动 PhotoRec 子进程，请检查可执行文件路径是否正确" ), QString(), 0, 0, badSectorCount );
		return;
	}

	photorec_invoke_result_t result;
	int waitOk = photorec_invoker_wait( handle, &result );
	bool wasCancelled;

	{
		QMutexLocker locker( &m_handleMutex );
		wasCancelled = m_cancelRequested;
		m_activeHandle = nullptr;
	}
	delete handle;

	if( waitOk != 1 )
	{
		emit carveFinished( false, wasCancelled, QStringLiteral( "等待 PhotoRec 退出失败" ), QString(), 0, 0, badSectorCount );
		return;
	}

	QString recupDirPath = QString::fromUtf8( result.recup_dir_path );
	int recoveredFileCount = 0;

	/* 取消之后PhotoRec可能连report.xml都没来得及写完整，不去读它——
	 * 只有真正自然跑完（没被取消）才解析统计数字 */
	if( !wasCancelled && !recupDirPath.isEmpty() )
	{
		QFile reportFile( recupDirPath + QStringLiteral( "/report.xml" ) );
		if( reportFile.open( QIODevice::ReadOnly ) )
		{
			QByteArray xmlData = reportFile.readAll();
			photorec_report_t report;

			memset( &report, 0, sizeof( report ) );
			if( photorec_report_parser_parse( xmlData.constData(), (size_t) xmlData.size(), &report ) == 1 )
			{
				recoveredFileCount = report.count;
			}
			photorec_report_free( &report );
		}
	}

	emit carveFinished( true, wasCancelled, QString(), recupDirPath, result.exit_code, recoveredFileCount, badSectorCount );
}
