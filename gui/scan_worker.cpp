#include "scan_worker.h"

#include <cstring>

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QTemporaryFile>

extern "C" {
#include "../engine/core/bad_sector_log.h"
#include "../engine/core/scan_progress_callback.h"
#include "../engine/core/undelete_scanner.h"
#include "../engine/shell/bitlocker_bfio_adapter.h"
#include "../engine/shell/bitlocker_credential_source.h"
#include "../engine/shell/bitlocker_unlock.h"
#include "../engine/shell/exfat_reader.h"
#include "../engine/shell/fat_reader.h"
#include "../engine/shell/file_writer.h"
#include "../engine/shell/ntfs_bfio_adapter.h"
#include "../engine/shell/path_resolver.h"
#include "../engine/shell/secure_memory.h"
#include "../engine/shell/vshadow_bfio_adapter.h"
}

/* 和cli/main.c的NTFS_DIRECT_MAX_READ_RETRIES同一个值——FAT32/exFAT的
 * 重试次数是fat_reader.c/exfat_reader.c内部固定常量，调用方不需要
 * （也没有接口）指定，这里只有NTFS这条路径需要自己传 */
static const uint32_t kNtfsMaxReadRetries = 3;

ScanWorker::ScanWorker( QObject *parent )
	: QObject( parent )
{
}

/* G7：析构是释放已解锁会话的两个触发点之一（另一个是下一次
 * scanBitLocker()开始前）——不依赖进程退出时操作系统兜底回收。 */
ScanWorker::~ScanWorker()
{
	releaseBitLockerSession();
}

namespace
{

/* H2：批量恢复的重名处理——逐个检查目标路径是否已存在，存在就在文件名
 * （扩展名之前）追加" (1)"/" (2)"直到找到一个空位。这里查的是"磁盘上
 * 现在是什么状态"，不是"这批里还有没有别的同名候选项"——因为
 * recoverBatch()是顺序处理的，前一项刚写完的文件在处理下一项时已经
 * 真实落盘，自然会被这个检查发现，两种重名场景（和磁盘已有文件重名、
 * 和同一批里另一个候选项重名）用同一套逻辑就都覆盖到了，不需要额外
 * 维护一个"本批已用名字"的集合。 */
QString uniqueOutputPath( const QString &dir, const QString &fileName )
{
	QString baseName = fileName.isEmpty() ? QStringLiteral( "recovered_file" ) : fileName;
	QString stem = baseName;
	QString extension;
	int dotIndex = baseName.lastIndexOf( QLatin1Char( '.' ) );

	if( dotIndex > 0 )
	{
		stem = baseName.left( dotIndex );
		extension = baseName.mid( dotIndex );
	}

	QString candidate = dir + QLatin1Char( '/' ) + baseName;
	int suffix = 1;

	while( QFileInfo::exists( candidate ) )
	{
		candidate = QStringLiteral( "%1/%2 (%3)%4" ).arg( dir, stem ).arg( suffix ).arg( extension );
		suffix++;
	}
	return candidate;
}

bitlocker_credential_type_t toBitLockerCredentialTypeC( BitLockerCredentialType type )
{
	switch( type )
	{
		case BitLockerCredentialType::Password:
			return BITLOCKER_CREDENTIAL_TYPE_PASSWORD;
		case BitLockerCredentialType::RecoveryKey:
			return BITLOCKER_CREDENTIAL_TYPE_RECOVERY_KEY;
		case BitLockerCredentialType::StartupKey:
			return BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY;
		case BitLockerCredentialType::Fvek:
			return BITLOCKER_CREDENTIAL_TYPE_FVEK;
	}
	return BITLOCKER_CREDENTIAL_TYPE_PASSWORD;
}

} // namespace

void ScanWorker::notifyProgress( quint64 current, quint64 total )
{
	emit progress( current, total );
}

/* C侧进度回调的trampoline——普通非成员static函数（小树裁决④：这样
 * 不需要碰Qt生成的protected信号访问权限，trampoline只调用ScanWorker
 * 公开的notifyProgress方法）。user_data在调用扫描函数时传的就是
 * ScanWorker*本身。 */
static void scan_progress_trampoline( uint64_t current, uint64_t total, void *user_data )
{
	ScanWorker *worker = static_cast<ScanWorker *>( user_data );

	if( worker != nullptr )
	{
		worker->notifyProgress( current, total );
	}
}

/* G6（VSS接入GUI，2026-11-06小树裁决）：按source.type分发到
 * DirectDevice（G3既有逻辑原样保留，改名+搬进scanDirectDevice）或
 * VssOffline（这次新增）两条路径，分发本身不碰任何真实I/O。 */
void ScanWorker::scan( ScanSource source )
{
	QVector<CandidateItem> candidates;
	QString errorMessage;
	bool ok;

	switch( source.type )
	{
		case ScanSourceType::DirectDevice:
			ok = scanDirectDevice( source.devicePath, candidates, errorMessage );
			break;

		case ScanSourceType::VssOffline:
			ok = scanVssOffline( source.devicePath, source.storeIndex, candidates, errorMessage );
			break;

		case ScanSourceType::BitLocker:
			/* BitLocker必须走scanBitLocker()槽函数（唯一接受凭据内容的
			 * 入口），source.type==BitLocker走到这个通用scan()说明调用
			 * 方传错了入口，不是正常路径 */
			ok = false;
			errorMessage = QStringLiteral( "BitLocker卷需要先提供解锁凭据，请使用BitLocker向导发起扫描" );
			break;

		default:
			ok = false;
			errorMessage = QStringLiteral( "未知的扫描来源类型" );
			break;
	}
	emit scanFinished( ok, errorMessage, candidates );
}

/* 复刻cli/main.c command_scan()的探测顺序：NTFS→FAT32→exFAT——G3既有
 * 逻辑，原样保留，只是从scan()槽函数体里搬出来，改名以区分
 * VssOffline那条路径（G6新增，VSS卷影副本内容固定是NTFS，不需要probe，
 * 见scanVssOffline）。 */
bool ScanWorker::scanDirectDevice( const QString &devicePath, QVector<CandidateItem> &outCandidates, QString &outError )
{
	QByteArray pathUtf8 = devicePath.toUtf8();
	const char *path = pathUtf8.constData();

	/* probe只是"打得开就算数"，不碰真正的扫描逻辑，用完立刻关掉。
	 * 2026-11-14（世杰真机GUI点击测试发现，首次真实点击验证"历史版本→
	 * 本机历史版本"就撞上）：这里原来探测NTFS用的是libfsntfs自带的默认
	 * 文件IO（直接libfsntfs_volume_open(path)），对VSS卷影副本设备路径
	 * （\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN）打不开，导致
	 * GUI报"无法识别卷的文件系统"；但真正扫描时scanNtfs()用的是我们自己
	 * 的ntfs_bfio_adapter（CreateFileA+重试/坏道容忍），对同一个路径能
	 * 正常打开——用CLI的`scan-shadow`命令（它跳过这层探测，直接用
	 * ntfs_bfio_adapter）对同一个真实设备路径验证过能扫出19个候选项，
	 * 证实问题精确出在"探测这一步用错了IO方式"，不是路径或数据本身的
	 * 问题。改成探测也用ntfs_bfio_adapter，和真正扫描时的打开方式保持
	 * 一致，消除这种"扫描时打得开、探测时打不开"的不对称。 */
	{
		libbfio_handle_t *probeBfioHandle = nullptr;
		libfsntfs_volume_t *probeVolume = nullptr;
		libfsntfs_error_t *probeError = nullptr;
		bad_sector_log_t probeBadLog;
		bool isNtfs = false;

		memset( &probeBadLog, 0, sizeof( probeBadLog ) );

		if( ntfs_bfio_adapter_initialize( path, 0, kNtfsMaxReadRetries, &probeBadLog, &probeBfioHandle, &probeError ) == 1 )
		{
			if( libfsntfs_volume_initialize( &probeVolume, &probeError ) == 1 )
			{
				if( libfsntfs_volume_open_file_io_handle( probeVolume, probeBfioHandle, LIBFSNTFS_OPEN_READ, &probeError ) == 1 )
				{
					isNtfs = true;
					probeBfioHandle = nullptr;
				}
				libfsntfs_volume_free( &probeVolume, nullptr );
			}
			if( probeBfioHandle != nullptr )
			{
				libbfio_handle_free( &probeBfioHandle, nullptr );
			}
		}
		if( probeError != nullptr )
		{
			libfsntfs_error_free( &probeError );
		}
		bad_sector_log_free( &probeBadLog );

		if( isNtfs )
		{
			return scanNtfs( devicePath, outCandidates, outError );
		}
	}
	{
		fat_reader_t probeReader;

		if( fat_reader_open( path, &probeReader ) == 1 )
		{
			fat_reader_close( &probeReader );
			return scanFat32( devicePath, outCandidates, outError );
		}
	}
	{
		exfat_reader_t probeReader;

		if( exfat_reader_open( path, &probeReader ) == 1 )
		{
			exfat_reader_close( &probeReader );
			return scanExFat( devicePath, outCandidates, outError );
		}
	}

	outError = QStringLiteral( "无法识别卷的文件系统（已尝试 NTFS、FAT32、exFAT）" );
	return false;
}

/* G6（VSS接入GUI，2026-11-06小树裁决）：离线VSS——打开镜像文件→定位
 * store→构造vshadow_bfio_adapter，和cli/main.c的
 * vshadow_offline_open_store_and_bfio()走同一条路径。VSS卷影副本的
 * 内容固定是NTFS（Windows系统卷的卷影副本不会是FAT32/exFAT），不走
 * probe，直接按NTFS扫描逻辑处理——和scanNtfs()共用"MFT全表扫描+
 * 路径重建"这部分逻辑，差异只在"怎么构造bfio_handle"这一步，所以这里
 * 不复用scanNtfs()本身（它自己会重新构造一个ntfs_bfio_adapter），而是
 * 内联一份结构相同、bfio来源不同的实现。 */
bool ScanWorker::scanVssOffline( const QString &imagePath, int storeIndex, QVector<CandidateItem> &outCandidates, QString &outError )
{
	QByteArray pathUtf8 = imagePath.toUtf8();
	libvshadow_volume_t *vshadowVolume = nullptr;
	libvshadow_store_t *store = nullptr;
	libvshadow_error_t *vshadowError = nullptr;
	libbfio_handle_t *bfioHandle = nullptr;
	libfsntfs_volume_t *volume = nullptr;
	libfsntfs_error_t *error = nullptr;
	undelete_scan_result_t result;
	bad_sector_log_t badLog;
	bool success = false;

	memset( &result, 0, sizeof( result ) );
	memset( &badLog, 0, sizeof( badLog ) );

	if( libvshadow_volume_initialize( &vshadowVolume, &vshadowError ) != 1 )
	{
		outError = QStringLiteral( "无法初始化 vshadow volume 对象" );
		goto cleanup;
	}
	if( libvshadow_volume_open( vshadowVolume, pathUtf8.constData(), LIBVSHADOW_OPEN_READ, &vshadowError ) != 1 )
	{
		outError = QStringLiteral( "无法把这个镜像当作包含VSS数据的卷来解析" );
		goto cleanup;
	}
	if( libvshadow_volume_get_store( vshadowVolume, storeIndex, &store, &vshadowError ) != 1 )
	{
		outError = QStringLiteral( "找不到 store 序号 %1" ).arg( storeIndex );
		goto cleanup;
	}
	if( vshadow_bfio_adapter_initialize( store, kNtfsMaxReadRetries, &badLog, &bfioHandle, &error ) != 1 )
	{
		outError = QStringLiteral( "无法构造自定义 I/O 句柄" );
		goto cleanup;
	}
	if( libfsntfs_volume_initialize( &volume, &error ) != 1 )
	{
		outError = QStringLiteral( "无法初始化 volume 对象" );
		goto cleanup;
	}
	if( libfsntfs_volume_open_file_io_handle( volume, bfioHandle, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		outError = QStringLiteral( "打开卷影副本的 NTFS 内容失败" );
		goto cleanup;
	}
	bfioHandle = nullptr;

	if( undelete_scanner_scan_volume( volume, &result, scan_progress_trampoline, this, &error ) != 1 )
	{
		outError = QStringLiteral( "扫描过程中发生错误" );
		goto cleanup;
	}

	for( int i = 0; i < result.count; i++ )
	{
		CandidateItem item;
		char ancestorPath[ 1024 ];
		int pathIsComplete = 0;

		if( path_resolver_reconstruct_ancestor_path(
		     volume, result.candidates[ i ].parent_mft_index,
		     ancestorPath, sizeof( ancestorPath ), &pathIsComplete, &error ) != 1 )
		{
			ancestorPath[ 0 ] = '\0';
		}

		item.fsType = CandidateFsType::Ntfs;
		item.name = QString::fromUtf8( result.candidates[ i ].name );
		item.directoryPath = QString::fromUtf8( ancestorPath );
		/* 2026-11-14（L3）：undelete_candidate_t新增file_size/has_file_size
		 * 字段后，候选项如果成功读到了大小就填进去；has_file_size为0（读取
		 * 失败）时维持hasSize=false，不编造一个可能是错的0，和candidate_item.h
		 * 的既有契约一致。 */
		if( result.candidates[ i ].has_file_size )
		{
			item.hasSize = true;
			item.size = result.candidates[ i ].file_size;
		}
		else
		{
			item.hasSize = false;
		}
		item.mftIndex = result.candidates[ i ].mft_entry_index;
		outCandidates.append( item );
	}
	success = true;

cleanup:
	if( error != nullptr )
	{
		libfsntfs_error_free( &error );
	}
	if( vshadowError != nullptr )
	{
		libvshadow_error_free( &vshadowError );
	}
	undelete_scan_result_free( &result );
	if( volume != nullptr )
	{
		libfsntfs_volume_close( volume, nullptr );
		libfsntfs_volume_free( &volume, nullptr );
	}
	if( bfioHandle != nullptr )
	{
		libbfio_handle_free( &bfioHandle, nullptr );
	}
	if( store != nullptr )
	{
		libvshadow_store_free( &store, nullptr );
	}
	if( vshadowVolume != nullptr )
	{
		libvshadow_volume_close( vshadowVolume, nullptr );
		libvshadow_volume_free( &vshadowVolume, nullptr );
	}
	bad_sector_log_free( &badLog );
	return success;
}

/* G7（BitLocker接入GUI，2026-11-06小树裁决C）：唯一接受凭据内容的
 * 入口——解锁成功后立刻缓存"已解锁会话"，不保留凭据内容本身。 */
void ScanWorker::scanBitLocker( ScanSource source, BitLockerCredentialInput credential )
{
	QVector<CandidateItem> candidates;
	QString errorMessage;

	/* 新一轮扫描前先释放旧会话（2026-11-06小树裁决C触发点①）——哪怕
	 * 这次解锁失败，旧会话也不应该继续留着，用户已经明确表达了"扫描
	 * 一个新目标"的意图 */
	releaseBitLockerSession();

	bitlocker_credential_type_t ctype = toBitLockerCredentialTypeC( credential.type );
	bitlocker_resolved_credential_t resolvedCredential;

	memset( &resolvedCredential, 0, sizeof( resolvedCredential ) );

	if( ctype == BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY || ctype == BITLOCKER_CREDENTIAL_TYPE_FVEK )
	{
		/* 启动密钥/FVEK都是从文件读取——复用引擎已有的文件渠道解析
		 * 逻辑，不在GUI侧重新实现一遍格式解析（B5既有函数） */
		QByteArray pathUtf8 = credential.filePath.toUtf8();

		if( bitlocker_credential_source_load_from_file( ctype, pathUtf8.constData(), &resolvedCredential ) != 1 )
		{
			emit scanFinished( false, QStringLiteral( "无法从文件读取凭据，请检查文件路径和内容格式" ), candidates );
			return;
		}
	}
	else
	{
		/* 密码/恢复密钥是用户直接在向导里输入的文本——复用引擎已有的
		 * "直接解析文本内容"逻辑（B10原本给命令行参数渠道用，这个函数
		 * 本身只是解析一个字符串，和真正的命令行/argv没有耦合，GUI
		 * 直接复用没有问题）。和CLI B10同样的纪律：这份文本用完立刻
		 * 原地清零，不等垃圾回收/作用域结束 */
		QByteArray textUtf8 = credential.textValue.toUtf8();
		int loadResult = bitlocker_credential_source_load_from_cli_argument( ctype, textUtf8.constData(), &resolvedCredential );

		secure_memory_zero( textUtf8.data(), (size_t) textUtf8.size() );
		if( loadResult != 1 )
		{
			emit scanFinished( false, QStringLiteral( "凭据格式不正确" ), candidates );
			return;
		}
	}

	QByteArray devicePathUtf8 = source.devicePath.toUtf8();
	libbde_volume_t *volume = nullptr;
	libbfio_handle_t *unlockBfioHandle = nullptr;
	libbde_error_t *error = nullptr;

	/* bitlocker_unlock_volume内部保证：调用结束（无论成功失败）都会把
	 * resolvedCredential里的密码/恢复密钥/FVEK/TWEAK字段清零——调用方
	 * 不需要、也不应该再自己清零一遍（见bitlocker_unlock.h头注释） */
	int unlockResult = bitlocker_unlock_volume( devicePathUtf8.constData(), source.baseOffset, &resolvedCredential, &volume, &unlockBfioHandle, &error );

	if( unlockResult != 1 )
	{
		if( error != nullptr )
		{
			libbde_error_free( &error );
		}
		emit scanFinished( false, QStringLiteral( "解锁失败：凭据不正确，或者凭据类型和这个卷实际使用的保护器不匹配" ), candidates );
		return;
	}

	/* 缓存已解锁会话——不含任何凭据内容，只是"已经打开+解锁好的卷" */
	m_bitlockerVolume = volume;
	m_bitlockerUnlockBfioHandle = unlockBfioHandle;
	m_bitlockerSessionDevicePath = source.devicePath;
	m_bitlockerSessionBaseOffset = source.baseOffset;
	m_bitlockerSessionActive = true;

	bool ok = scanBitLockerWithSession( source, candidates, errorMessage );
	emit scanFinished( ok, errorMessage, candidates );
}

/* 要求会话已经存在且和source匹配——不在这里问凭据（凭据只在
 * scanBitLocker()里现场问一次）。和scanVssOffline()同样的理由，不
 * 复用scanNtfs()（它会自己重新构造一个ntfs_bfio_adapter），而是内联
 * 一份结构相同、bfio来源不同的实现：每次调用都用缓存的
 * m_bitlockerVolume构造一个*新的*bfio_handle（bitlocker_bfio_adapter
 * 不拥有volume的生命周期，可以安全地反复调用，见
 * bitlocker_bfio_adapter.c头注释），这个新bfio_handle的生命周期跟着
 * 这次调用里构造的libfsntfs_volume走，用完就释放，不影响
 * m_bitlockerVolume本身继续存活供下一次调用复用。 */
bool ScanWorker::scanBitLockerWithSession( const ScanSource &source, QVector<CandidateItem> &outCandidates, QString &outError )
{
	libbde_volume_t *volume = reinterpret_cast<libbde_volume_t *>( m_bitlockerVolume );
	libbfio_handle_t *bfioHandle = nullptr;
	libfsntfs_volume_t *fsntfsVolume = nullptr;
	libfsntfs_error_t *error = nullptr;
	undelete_scan_result_t result;
	bad_sector_log_t badLog;
	bool success = false;

	memset( &result, 0, sizeof( result ) );
	memset( &badLog, 0, sizeof( badLog ) );

	if( !m_bitlockerSessionActive || volume == nullptr )
	{
		outError = QStringLiteral( "没有已解锁的BitLocker会话，请先重新使用BitLocker向导扫描" );
		return false;
	}
	if( bitlocker_bfio_adapter_initialize( volume, kNtfsMaxReadRetries, &badLog, &bfioHandle, &error ) != 1 )
	{
		outError = QStringLiteral( "无法构造自定义 I/O 句柄" );
		goto cleanup;
	}
	if( libfsntfs_volume_initialize( &fsntfsVolume, &error ) != 1 )
	{
		outError = QStringLiteral( "无法初始化 volume 对象" );
		goto cleanup;
	}
	if( libfsntfs_volume_open_file_io_handle( fsntfsVolume, bfioHandle, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		outError = QStringLiteral( "打开 BitLocker 卷的 NTFS 内容失败" );
		goto cleanup;
	}
	bfioHandle = nullptr;

	if( undelete_scanner_scan_volume( fsntfsVolume, &result, scan_progress_trampoline, this, &error ) != 1 )
	{
		outError = QStringLiteral( "扫描过程中发生错误" );
		goto cleanup;
	}

	for( int i = 0; i < result.count; i++ )
	{
		CandidateItem item;
		char ancestorPath[ 1024 ];
		int pathIsComplete = 0;

		if( path_resolver_reconstruct_ancestor_path(
		     fsntfsVolume, result.candidates[ i ].parent_mft_index,
		     ancestorPath, sizeof( ancestorPath ), &pathIsComplete, &error ) != 1 )
		{
			ancestorPath[ 0 ] = '\0';
		}

		item.fsType = CandidateFsType::Ntfs;
		item.name = QString::fromUtf8( result.candidates[ i ].name );
		item.directoryPath = QString::fromUtf8( ancestorPath );
		/* 2026-11-14（L3）：undelete_candidate_t新增file_size/has_file_size
		 * 字段后，候选项如果成功读到了大小就填进去；has_file_size为0（读取
		 * 失败）时维持hasSize=false，不编造一个可能是错的0，和candidate_item.h
		 * 的既有契约一致。 */
		if( result.candidates[ i ].has_file_size )
		{
			item.hasSize = true;
			item.size = result.candidates[ i ].file_size;
		}
		else
		{
			item.hasSize = false;
		}
		item.mftIndex = result.candidates[ i ].mft_entry_index;
		outCandidates.append( item );
	}
	success = true;

cleanup:
	if( error != nullptr )
	{
		libfsntfs_error_free( &error );
	}
	undelete_scan_result_free( &result );
	if( fsntfsVolume != nullptr )
	{
		libfsntfs_volume_close( fsntfsVolume, nullptr );
		libfsntfs_volume_free( &fsntfsVolume, nullptr );
	}
	if( bfioHandle != nullptr )
	{
		libbfio_handle_free( &bfioHandle, nullptr );
	}
	bad_sector_log_free( &badLog );
	return success;
}

/* G7：释放已解锁会话（两个触发点：下一次scanBitLocker()开始前、
 * ScanWorker析构时），空会话调用是安全的no-op */
void ScanWorker::releaseBitLockerSession()
{
	if( m_bitlockerVolume != nullptr )
	{
		libbde_volume_t *volume = reinterpret_cast<libbde_volume_t *>( m_bitlockerVolume );

		libbde_volume_close( volume, nullptr );
		libbde_volume_free( &volume, nullptr );
		m_bitlockerVolume = nullptr;
	}
	if( m_bitlockerUnlockBfioHandle != nullptr )
	{
		libbfio_handle_t *handle = reinterpret_cast<libbfio_handle_t *>( m_bitlockerUnlockBfioHandle );

		libbfio_handle_free( &handle, nullptr );
		m_bitlockerUnlockBfioHandle = nullptr;
	}
	m_bitlockerSessionActive = false;
	m_bitlockerSessionDevicePath.clear();
	m_bitlockerSessionBaseOffset = 0;
}

bool ScanWorker::scanNtfs( const QString &devicePath, QVector<CandidateItem> &outCandidates, QString &outError )
{
	QByteArray pathUtf8 = devicePath.toUtf8();
	const char *path = pathUtf8.constData();
	libbfio_handle_t *bfioHandle = nullptr;
	libfsntfs_volume_t *volume = nullptr;
	libfsntfs_error_t *error = nullptr;
	undelete_scan_result_t result;
	bad_sector_log_t badLog;
	bool success = false;

	memset( &result, 0, sizeof( result ) );
	memset( &badLog, 0, sizeof( badLog ) );

	if( ntfs_bfio_adapter_initialize( path, 0, kNtfsMaxReadRetries, &badLog, &bfioHandle, &error ) != 1 )
	{
		outError = QStringLiteral( "无法构造自定义 I/O 句柄" );
		goto cleanup;
	}
	if( libfsntfs_volume_initialize( &volume, &error ) != 1 )
	{
		outError = QStringLiteral( "无法初始化 volume 对象" );
		goto cleanup;
	}
	if( libfsntfs_volume_open_file_io_handle( volume, bfioHandle, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		outError = QStringLiteral( "打开 NTFS 卷失败" );
		goto cleanup;
	}
	/* bfioHandle的生命周期从这里起交给volume管理（和cli/main.c同样的
	 * 约定），下面的清理不再单独释放它，避免重复释放 */
	bfioHandle = nullptr;

	if( undelete_scanner_scan_volume( volume, &result, scan_progress_trampoline, this, &error ) != 1 )
	{
		outError = QStringLiteral( "扫描过程中发生错误" );
		goto cleanup;
	}

	for( int i = 0; i < result.count; i++ )
	{
		CandidateItem item;
		char ancestorPath[ 1024 ];
		int pathIsComplete = 0;

		if( path_resolver_reconstruct_ancestor_path(
		     volume, result.candidates[ i ].parent_mft_index,
		     ancestorPath, sizeof( ancestorPath ), &pathIsComplete, &error ) != 1 )
		{
			ancestorPath[ 0 ] = '\0';
		}

		item.fsType = CandidateFsType::Ntfs;
		item.name = QString::fromUtf8( result.candidates[ i ].name );
		item.directoryPath = QString::fromUtf8( ancestorPath );
		/* 2026-11-14（L3）：undelete_candidate_t新增file_size/has_file_size
		 * 字段后，候选项如果成功读到了大小就填进去；has_file_size为0（读取
		 * 失败）时维持hasSize=false，不编造一个可能是错的0，和candidate_item.h
		 * 的既有契约一致。 */
		if( result.candidates[ i ].has_file_size )
		{
			item.hasSize = true;
			item.size = result.candidates[ i ].file_size;
		}
		else
		{
			item.hasSize = false;
		}
		item.mftIndex = result.candidates[ i ].mft_entry_index;
		outCandidates.append( item );
	}
	success = true;

cleanup:
	if( error != nullptr )
	{
		libfsntfs_error_free( &error );
	}
	undelete_scan_result_free( &result );
	if( volume != nullptr )
	{
		libfsntfs_volume_close( volume, nullptr );
		libfsntfs_volume_free( &volume, nullptr );
	}
	if( bfioHandle != nullptr )
	{
		libbfio_handle_free( &bfioHandle, nullptr );
	}
	bad_sector_log_free( &badLog );
	return success;
}

bool ScanWorker::scanFat32( const QString &devicePath, QVector<CandidateItem> &outCandidates, QString &outError )
{
	QByteArray pathUtf8 = devicePath.toUtf8();
	fat_reader_t reader;
	fat_undelete_scan_result_t result;
	bad_sector_log_t badLog;
	bool success = false;

	memset( &result, 0, sizeof( result ) );
	memset( &badLog, 0, sizeof( badLog ) );

	if( fat_reader_open( pathUtf8.constData(), &reader ) != 1 )
	{
		outError = QStringLiteral( "无法打开卷" );
		return false;
	}
	if( fat_reader_scan_directory_tree( &reader, &badLog, scan_progress_trampoline, this, &result ) != 1 )
	{
		outError = QStringLiteral( "扫描过程中发生错误" );
		goto cleanup;
	}
	for( int i = 0; i < result.count; i++ )
	{
		CandidateItem item;

		item.fsType = CandidateFsType::Fat32;
		item.name = QString::fromUtf8( result.candidates[ i ].name );
		item.directoryPath = QString::fromUtf8( result.candidates[ i ].directory_path );
		item.hasSize = true;
		item.size = result.candidates[ i ].file_size;
		item.firstCluster = result.candidates[ i ].first_cluster;
		outCandidates.append( item );
	}
	success = true;

cleanup:
	fat_undelete_scan_result_free( &result );
	fat_reader_close( &reader );
	bad_sector_log_free( &badLog );
	return success;
}

bool ScanWorker::scanExFat( const QString &devicePath, QVector<CandidateItem> &outCandidates, QString &outError )
{
	QByteArray pathUtf8 = devicePath.toUtf8();
	exfat_reader_t reader;
	exfat_undelete_scan_result_t result;
	bad_sector_log_t badLog;
	bool success = false;

	memset( &result, 0, sizeof( result ) );
	memset( &badLog, 0, sizeof( badLog ) );

	if( exfat_reader_open( pathUtf8.constData(), &reader ) != 1 )
	{
		outError = QStringLiteral( "无法打开卷" );
		return false;
	}
	if( exfat_reader_scan_directory_tree( &reader, &badLog, scan_progress_trampoline, this, &result ) != 1 )
	{
		outError = QStringLiteral( "扫描过程中发生错误" );
		goto cleanup;
	}
	for( int i = 0; i < result.count; i++ )
	{
		CandidateItem item;

		item.fsType = CandidateFsType::ExFat;
		item.name = QString::fromUtf8( result.candidates[ i ].name );
		item.directoryPath = QString::fromUtf8( result.candidates[ i ].directory_path );
		item.hasSize = true;
		item.size = result.candidates[ i ].file_size;
		item.firstCluster = result.candidates[ i ].first_cluster;
		item.noFatChain = ( result.candidates[ i ].no_fat_chain != 0 );
		outCandidates.append( item );
	}
	success = true;

cleanup:
	exfat_undelete_scan_result_free( &result );
	exfat_reader_close( &reader );
	bad_sector_log_free( &badLog );
	return success;
}

/* G6（VSS接入GUI，2026-11-06小树裁决）：按source.type分发，和scan()
 * 同样的分发原则。 */
void ScanWorker::recover( ScanSource source, CandidateItem item, const QString &outputPath )
{
	QString errorMessage;
	bool ok;
	int badSectorCount = 0;
	quint64 badSectorBytes = 0;

	switch( source.type )
	{
		case ScanSourceType::DirectDevice:
			ok = recoverDirectDevice( source.devicePath, item, outputPath, errorMessage, badSectorCount, badSectorBytes );
			break;

		case ScanSourceType::VssOffline:
			ok = recoverVssOffline( source.devicePath, source.storeIndex, item, outputPath, errorMessage, badSectorCount, badSectorBytes );
			break;

		case ScanSourceType::BitLocker:
			/* 不需要凭据——复用scanBitLocker()里缓存的已解锁会话
			 * （2026-11-06小树裁决C），会话不存在/不匹配会在
			 * recoverBitLocker()内部报错，不在这里现场问凭据 */
			ok = recoverBitLocker( source, item, outputPath, errorMessage, badSectorCount, badSectorBytes );
			break;

		default:
			ok = false;
			errorMessage = QStringLiteral( "未知的扫描来源类型" );
			break;
	}
	emit recoverFinished( ok, errorMessage, badSectorCount, badSectorBytes );
}

/* H2（批量恢复，2026-11-06小树裁决）：直接复用recoverDirectDevice/
 * recoverVssOffline/recoverBitLocker三个既有私有函数，顺序处理每一项，
 * 每完成一项发一次recoverBatchItemFinished，全部处理完发一次
 * recoverBatchFinished汇总。BitLocker这条路径天然复用scanBitLocker()
 * 阶段缓存下来的已解锁会话（m_bitlockerVolume），不需要在这里重新问
 * 一次凭据——和单个recover()的BitLocker分支是同一个会话。 */
void ScanWorker::recoverBatch( ScanSource source, QVector<CandidateItem> items, QString outputDir )
{
	int successCount = 0;
	QStringList failedNames;
	int totalBadSectorCount = 0;
	quint64 totalBadSectorBytes = 0;

	for( int i = 0; i < items.count(); i++ )
	{
		const CandidateItem &item = items.at( i );
		QString outputPath = uniqueOutputPath( outputDir, item.name );
		QString errorMessage;
		bool ok;
		int badSectorCount = 0;
		quint64 badSectorBytes = 0;

		switch( source.type )
		{
			case ScanSourceType::DirectDevice:
				ok = recoverDirectDevice( source.devicePath, item, outputPath, errorMessage, badSectorCount, badSectorBytes );
				break;

			case ScanSourceType::VssOffline:
				ok = recoverVssOffline( source.devicePath, source.storeIndex, item, outputPath, errorMessage, badSectorCount, badSectorBytes );
				break;

			case ScanSourceType::BitLocker:
				ok = recoverBitLocker( source, item, outputPath, errorMessage, badSectorCount, badSectorBytes );
				break;

			default:
				ok = false;
				errorMessage = QStringLiteral( "未知的扫描来源类型" );
				break;
		}

		if( ok )
		{
			successCount++;
		}
		else
		{
			failedNames.append( item.name.isEmpty() ? QStringLiteral( "(未命名)" ) : item.name );
		}
		totalBadSectorCount += badSectorCount;
		totalBadSectorBytes += badSectorBytes;
		emit recoverBatchItemFinished( i, ok, errorMessage, outputPath, badSectorCount, badSectorBytes );
	}
	emit recoverBatchFinished( successCount, items.count(), failedNames, totalBadSectorCount, totalBadSectorBytes );
}

/* G3既有分发逻辑，原样保留，只是改名以区分scanDirectDevice/
 * scanVssOffline那条新增的路径。 */
bool ScanWorker::recoverDirectDevice( const QString &devicePath, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes )
{
	switch( item.fsType )
	{
		case CandidateFsType::Ntfs:
			return recoverNtfs( devicePath, item, outputPath, outError, outBadSectorCount, outBadSectorBytes );

		case CandidateFsType::Fat32:
			return recoverFat32( devicePath, item, outputPath, outError, outBadSectorCount, outBadSectorBytes );

		case CandidateFsType::ExFat:
			return recoverExFat( devicePath, item, outputPath, outError, outBadSectorCount, outBadSectorBytes );
	}
	return false;
}

/* G6新增：离线VSS恢复——和scanVssOffline()共用同一套bfio构造路径
 * （libvshadow_volume_open→get_store→vshadow_bfio_adapter_initialize→
 * libfsntfs_volume_open_file_io_handle），定位和写出这两步照搬
 * recoverNtfs()最后那段（按mftIndex取file_entry、
 * file_writer_write_entry_content写出），VSS候选项固定是NTFS，不需要
 * 按fsType分发。 */
bool ScanWorker::recoverVssOffline( const QString &imagePath, int storeIndex, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes )
{
	QByteArray pathUtf8 = imagePath.toUtf8();
	QByteArray outputUtf8 = outputPath.toUtf8();
	libvshadow_volume_t *vshadowVolume = nullptr;
	libvshadow_store_t *store = nullptr;
	libvshadow_error_t *vshadowError = nullptr;
	libbfio_handle_t *bfioHandle = nullptr;
	libfsntfs_volume_t *volume = nullptr;
	libfsntfs_file_entry_t *fileEntry = nullptr;
	libfsntfs_error_t *error = nullptr;
	bad_sector_log_t badLog;
	bool success = false;

	memset( &badLog, 0, sizeof( badLog ) );

	if( libvshadow_volume_initialize( &vshadowVolume, &vshadowError ) != 1 )
	{
		outError = QStringLiteral( "无法初始化 vshadow volume 对象" );
		goto cleanup;
	}
	if( libvshadow_volume_open( vshadowVolume, pathUtf8.constData(), LIBVSHADOW_OPEN_READ, &vshadowError ) != 1 )
	{
		outError = QStringLiteral( "无法把这个镜像当作包含VSS数据的卷来解析" );
		goto cleanup;
	}
	if( libvshadow_volume_get_store( vshadowVolume, storeIndex, &store, &vshadowError ) != 1 )
	{
		outError = QStringLiteral( "找不到 store 序号 %1" ).arg( storeIndex );
		goto cleanup;
	}
	if( vshadow_bfio_adapter_initialize( store, kNtfsMaxReadRetries, &badLog, &bfioHandle, &error ) != 1 )
	{
		outError = QStringLiteral( "无法构造自定义 I/O 句柄" );
		goto cleanup;
	}
	if( libfsntfs_volume_initialize( &volume, &error ) != 1 )
	{
		outError = QStringLiteral( "无法初始化 volume 对象" );
		goto cleanup;
	}
	if( libfsntfs_volume_open_file_io_handle( volume, bfioHandle, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		outError = QStringLiteral( "打开卷影副本的 NTFS 内容失败" );
		goto cleanup;
	}
	bfioHandle = nullptr;

	if( libfsntfs_volume_get_file_entry_by_index( volume, item.mftIndex, &fileEntry, &error ) != 1 )
	{
		outError = QStringLiteral( "找不到 MFT 索引 %1" ).arg( item.mftIndex );
		goto cleanup;
	}
	if( file_writer_write_entry_content( fileEntry, nullptr, outputUtf8.constData(), &error ) != 1 )
	{
		outError = QStringLiteral( "写出内容失败" );
		goto cleanup;
	}
	success = true;

cleanup:
	if( error != nullptr )
	{
		libfsntfs_error_free( &error );
	}
	if( vshadowError != nullptr )
	{
		libvshadow_error_free( &vshadowError );
	}
	if( fileEntry != nullptr )
	{
		libfsntfs_file_entry_free( &fileEntry, nullptr );
	}
	if( volume != nullptr )
	{
		libfsntfs_volume_close( volume, nullptr );
		libfsntfs_volume_free( &volume, nullptr );
	}
	if( bfioHandle != nullptr )
	{
		libbfio_handle_free( &bfioHandle, nullptr );
	}
	if( store != nullptr )
	{
		libvshadow_store_free( &store, nullptr );
	}
	if( vshadowVolume != nullptr )
	{
		libvshadow_volume_close( vshadowVolume, nullptr );
		libvshadow_volume_free( &vshadowVolume, nullptr );
	}
	outBadSectorCount = badLog.count;
	outBadSectorBytes = bad_sector_log_total_bytes_skipped( &badLog );
	bad_sector_log_free( &badLog );
	return success;
}

/* G7：不需要凭据——复用scanBitLocker()里缓存的m_bitlockerVolume，和
 * scanBitLockerWithSession()同样的理由，每次调用用缓存的volume构造
 * 一个*新的*bfio_handle，定位和写出这两步照搬recoverNtfs()最后那段
 * （按mftIndex取file_entry、file_writer_write_entry_content写出）。 */
bool ScanWorker::recoverBitLocker( const ScanSource &source, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes )
{
	QByteArray outputUtf8 = outputPath.toUtf8();
	libbde_volume_t *volume = reinterpret_cast<libbde_volume_t *>( m_bitlockerVolume );
	libbfio_handle_t *bfioHandle = nullptr;
	libfsntfs_volume_t *fsntfsVolume = nullptr;
	libfsntfs_file_entry_t *fileEntry = nullptr;
	libfsntfs_error_t *error = nullptr;
	bad_sector_log_t badLog;
	bool success = false;

	memset( &badLog, 0, sizeof( badLog ) );

	if( !m_bitlockerSessionActive || volume == nullptr
	 || m_bitlockerSessionDevicePath != source.devicePath || m_bitlockerSessionBaseOffset != source.baseOffset )
	{
		outError = QStringLiteral( "没有已解锁的BitLocker会话，请先重新使用BitLocker向导扫描" );
		return false;
	}
	if( bitlocker_bfio_adapter_initialize( volume, kNtfsMaxReadRetries, &badLog, &bfioHandle, &error ) != 1 )
	{
		outError = QStringLiteral( "无法构造自定义 I/O 句柄" );
		goto cleanup;
	}
	if( libfsntfs_volume_initialize( &fsntfsVolume, &error ) != 1 )
	{
		outError = QStringLiteral( "无法初始化 volume 对象" );
		goto cleanup;
	}
	if( libfsntfs_volume_open_file_io_handle( fsntfsVolume, bfioHandle, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		outError = QStringLiteral( "打开 BitLocker 卷的 NTFS 内容失败" );
		goto cleanup;
	}
	bfioHandle = nullptr;

	if( libfsntfs_volume_get_file_entry_by_index( fsntfsVolume, item.mftIndex, &fileEntry, &error ) != 1 )
	{
		outError = QStringLiteral( "找不到 MFT 索引 %1" ).arg( item.mftIndex );
		goto cleanup;
	}
	if( file_writer_write_entry_content( fileEntry, nullptr, outputUtf8.constData(), &error ) != 1 )
	{
		outError = QStringLiteral( "写出内容失败" );
		goto cleanup;
	}
	success = true;

cleanup:
	if( error != nullptr )
	{
		libfsntfs_error_free( &error );
	}
	if( fileEntry != nullptr )
	{
		libfsntfs_file_entry_free( &fileEntry, nullptr );
	}
	if( fsntfsVolume != nullptr )
	{
		libfsntfs_volume_close( fsntfsVolume, nullptr );
		libfsntfs_volume_free( &fsntfsVolume, nullptr );
	}
	if( bfioHandle != nullptr )
	{
		libbfio_handle_free( &bfioHandle, nullptr );
	}
	outBadSectorCount = badLog.count;
	outBadSectorBytes = bad_sector_log_total_bytes_skipped( &badLog );
	bad_sector_log_free( &badLog );
	return success;
}

/* G4b（2026-11-06小树裁决②③）：UI层前置筛选（扩展名是否在
 * QImageReader::supportedImageFormats()范围内）由调用方（MainWindow）
 * 做——那一层不涉及任何真实I/O，判断"要不要发这个请求"本来就该在
 * 请求发出之前完成，不需要在worker里重复一遍。这里收到请求之后
 * 直接尝试"恢复到临时文件→QImageReader解码"，解码失败就是
 * "decode-failed"（内容已损坏，数据恢复场景下的正常情况，不是bug），
 * 不再反过来猜测是不是UI层筛选判断错了。 */
void ScanWorker::generateThumbnail( int row, ScanSource source, CandidateItem item )
{
	QString recoverError;
	bool recovered;
	/* 缩略图生成不需要把坏道信息传出去——预览区本来就有独立的"decode-
	 * failed"失败态覆盖"内容已损坏"这个场景，不需要额外叠加一份坏道
	 * 计数的展示逻辑，这两个变量只是满足recoverXxx()系列函数现在统一
	 * 要求的出参，原地丢弃 */
	int unusedBadSectorCount = 0;
	quint64 unusedBadSectorBytes = 0;

	QTemporaryFile tempFile;
	tempFile.setAutoRemove( false );
	if( !tempFile.open() )
	{
		emit thumbnailReady( row, false, QStringLiteral( "decode-failed" ), QPixmap() );
		return;
	}
	QString tempPath = tempFile.fileName();
	tempFile.close();

	switch( source.type )
	{
		case ScanSourceType::DirectDevice:
			recovered = recoverDirectDevice( source.devicePath, item, tempPath, recoverError, unusedBadSectorCount, unusedBadSectorBytes );
			break;

		case ScanSourceType::VssOffline:
			recovered = recoverVssOffline( source.devicePath, source.storeIndex, item, tempPath, recoverError, unusedBadSectorCount, unusedBadSectorBytes );
			break;

		case ScanSourceType::BitLocker:
			recovered = recoverBitLocker( source, item, tempPath, recoverError, unusedBadSectorCount, unusedBadSectorBytes );
			break;

		default:
			recovered = false;
			break;
	}

	QPixmap pixmap;
	bool decoded = false;
	if( recovered )
	{
		QImageReader reader( tempPath );
		QImage image = reader.read();
		if( !image.isNull() )
		{
			pixmap = QPixmap::fromImage( image );
			decoded = true;
		}
	}

	QFile::remove( tempPath );

	if( decoded )
	{
		emit thumbnailReady( row, true, QString(), pixmap );
	}
	else
	{
		emit thumbnailReady( row, false, QStringLiteral( "decode-failed" ), QPixmap() );
	}
}

bool ScanWorker::recoverNtfs( const QString &devicePath, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes )
{
	QByteArray pathUtf8 = devicePath.toUtf8();
	QByteArray outputUtf8 = outputPath.toUtf8();
	libbfio_handle_t *bfioHandle = nullptr;
	libfsntfs_volume_t *volume = nullptr;
	libfsntfs_file_entry_t *fileEntry = nullptr;
	libfsntfs_error_t *error = nullptr;
	bad_sector_log_t badLog;
	bool success = false;

	memset( &badLog, 0, sizeof( badLog ) );

	if( ntfs_bfio_adapter_initialize( pathUtf8.constData(), 0, kNtfsMaxReadRetries, &badLog, &bfioHandle, &error ) != 1 )
	{
		outError = QStringLiteral( "无法构造自定义 I/O 句柄" );
		goto cleanup;
	}
	if( libfsntfs_volume_initialize( &volume, &error ) != 1 )
	{
		outError = QStringLiteral( "无法初始化 volume 对象" );
		goto cleanup;
	}
	if( libfsntfs_volume_open_file_io_handle( volume, bfioHandle, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		outError = QStringLiteral( "打开 NTFS 卷失败" );
		goto cleanup;
	}
	bfioHandle = nullptr;

	if( libfsntfs_volume_get_file_entry_by_index( volume, item.mftIndex, &fileEntry, &error ) != 1 )
	{
		outError = QStringLiteral( "找不到 MFT 索引 %1" ).arg( item.mftIndex );
		goto cleanup;
	}
	if( file_writer_write_entry_content( fileEntry, nullptr, outputUtf8.constData(), &error ) != 1 )
	{
		outError = QStringLiteral( "写出内容失败" );
		goto cleanup;
	}
	success = true;

cleanup:
	if( error != nullptr )
	{
		libfsntfs_error_free( &error );
	}
	if( fileEntry != nullptr )
	{
		libfsntfs_file_entry_free( &fileEntry, nullptr );
	}
	if( volume != nullptr )
	{
		libfsntfs_volume_close( volume, nullptr );
		libfsntfs_volume_free( &volume, nullptr );
	}
	if( bfioHandle != nullptr )
	{
		libbfio_handle_free( &bfioHandle, nullptr );
	}
	outBadSectorCount = badLog.count;
	outBadSectorBytes = bad_sector_log_total_bytes_skipped( &badLog );
	bad_sector_log_free( &badLog );
	return success;
}

bool ScanWorker::recoverFat32( const QString &devicePath, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes )
{
	QByteArray pathUtf8 = devicePath.toUtf8();
	QByteArray outputUtf8 = outputPath.toUtf8();
	fat_reader_t reader;
	bad_sector_log_t badLog;
	bool success;

	memset( &badLog, 0, sizeof( badLog ) );

	if( fat_reader_open( pathUtf8.constData(), &reader ) != 1 )
	{
		outError = QStringLiteral( "无法打开卷" );
		return false;
	}
	success = ( fat_reader_recover_file(
	             &reader, item.firstCluster, (uint32_t) item.size, &badLog, outputUtf8.constData() ) == 1 );
	if( !success )
	{
		outError = QStringLiteral( "写出内容失败" );
	}
	fat_reader_close( &reader );
	outBadSectorCount = badLog.count;
	outBadSectorBytes = bad_sector_log_total_bytes_skipped( &badLog );
	bad_sector_log_free( &badLog );
	return success;
}

bool ScanWorker::recoverExFat( const QString &devicePath, const CandidateItem &item, const QString &outputPath, QString &outError, int &outBadSectorCount, quint64 &outBadSectorBytes )
{
	QByteArray pathUtf8 = devicePath.toUtf8();
	QByteArray outputUtf8 = outputPath.toUtf8();
	exfat_reader_t reader;
	bad_sector_log_t badLog;
	bool success;

	memset( &badLog, 0, sizeof( badLog ) );

	if( exfat_reader_open( pathUtf8.constData(), &reader ) != 1 )
	{
		outError = QStringLiteral( "无法打开卷" );
		return false;
	}
	success = ( exfat_reader_recover_file(
	             &reader, item.firstCluster, item.size, item.noFatChain ? 1 : 0, &badLog, outputUtf8.constData() ) == 1 );
	if( !success )
	{
		outError = QStringLiteral( "写出内容失败" );
	}
	exfat_reader_close( &reader );
	outBadSectorCount = badLog.count;
	outBadSectorBytes = bad_sector_log_total_bytes_skipped( &badLog );
	bad_sector_log_free( &badLog );
	return success;
}
