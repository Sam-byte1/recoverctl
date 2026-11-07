#include "carve_page.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "carve_worker.h"
#include "../engine/shell/disk_enumerator.h"

namespace
{

/* main_window.cpp里的formatFileSize()是匿名命名空间内部链接，这边用不到，
 * 没必要为了共享十几行格式化代码去新建头文件——三行相似代码比过早抽象好。 */
QString carveFormatDiskSize( quint64 bytes )
{
	static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
	double value = (double) bytes;
	int unitIndex = 0;

	while( value >= 1024.0 && unitIndex < 4 )
	{
		value /= 1024.0;
		unitIndex++;
	}
	if( unitIndex == 0 )
	{
		return QStringLiteral( "%1 B" ).arg( bytes );
	}
	return QStringLiteral( "%1 %2" ).arg( value, 0, 'f', 1 ).arg( units[ unitIndex ] );
}

}

CarvePage::CarvePage( QWidget *parent )
	: QWidget( parent )
	, m_advancedToggleButton( nullptr )
	, m_advancedPanel( nullptr )
	, m_photorecPathEdit( nullptr )
	, m_browsePhotorecPathButton( nullptr )
	, m_targetPathEdit( nullptr )
	, m_browseTargetButton( nullptr )
	, m_pickDiskButton( nullptr )
	, m_outputDirEdit( nullptr )
	, m_browseOutputDirButton( nullptr )
	, m_partitionRangeCheckBox( nullptr )
	, m_partitionOffsetEdit( nullptr )
	, m_partitionSizeEdit( nullptr )
	, m_startButton( nullptr )
	, m_cancelButton( nullptr )
	, m_progressBar( nullptr )
	, m_elapsedLabel( nullptr )
	, m_resultLabel( nullptr )
	, m_openOutputButton( nullptr )
	, m_elapsedTimer( nullptr )
	, m_worker( nullptr )
	, m_running( false )
{
	QVBoxLayout *mainLayout = new QVBoxLayout( this );
	mainLayout->setContentsMargins( 32, 28, 32, 32 );
	mainLayout->setSpacing( 16 );

	QLabel *title = new QLabel( QStringLiteral( "深度查找文件" ), this );
	title->setObjectName( QStringLiteral( "pageTitle" ) );
	QLabel *subtitle = new QLabel(
	     QStringLiteral( "按文件特征逐字节查找，扫描耗时可能较长，找回的文件可能没有原文件名和目录。" ), this );
	subtitle->setObjectName( QStringLiteral( "pageSubtitle" ) );
	mainLayout->addWidget( title );
	mainLayout->addWidget( subtitle );

	QHBoxLayout *targetRow = new QHBoxLayout();
	m_targetPathEdit = new QLineEdit( this );
	m_targetPathEdit->setPlaceholderText( QStringLiteral( "设备路径（比如 \\\\.\\PhysicalDrive0）或镜像文件路径" ) );
	m_browseTargetButton = new QPushButton( QStringLiteral( "浏览镜像文件..." ), this );
	m_browseTargetButton->setProperty( "role", "ghost" );
	m_pickDiskButton = new QPushButton( QStringLiteral( "选择磁盘..." ), this );
	m_pickDiskButton->setProperty( "role", "ghost" );
	targetRow->addWidget( new QLabel( QStringLiteral( "查找来源:" ), this ) );
	targetRow->addWidget( m_targetPathEdit, 1 );
	targetRow->addWidget( m_pickDiskButton );
	targetRow->addWidget( m_browseTargetButton );
	mainLayout->addLayout( targetRow );

	QHBoxLayout *outputRow = new QHBoxLayout();
	m_outputDirEdit = new QLineEdit( this );
	m_outputDirEdit->setPlaceholderText( QStringLiteral( "找回的文件要保存到哪个目录" ) );
	m_browseOutputDirButton = new QPushButton( QStringLiteral( "浏览..." ), this );
	m_browseOutputDirButton->setProperty( "role", "ghost" );
	outputRow->addWidget( new QLabel( QStringLiteral( "保存位置:" ), this ) );
	outputRow->addWidget( m_outputDirEdit, 1 );
	outputRow->addWidget( m_browseOutputDirButton );
	mainLayout->addLayout( outputRow );

	m_advancedToggleButton = new QPushButton( QStringLiteral( "高级设置 ▾" ), this );
	m_advancedToggleButton->setProperty( "role", "ghost" );
	mainLayout->addWidget( m_advancedToggleButton, 0, Qt::AlignLeft );

	m_advancedPanel = new QWidget( this );
	QVBoxLayout *advancedLayout = new QVBoxLayout( m_advancedPanel );
	advancedLayout->setContentsMargins( 0, 4, 0, 0 );
	advancedLayout->setSpacing( 10 );

	QHBoxLayout *photorecRow = new QHBoxLayout();
	m_photorecPathEdit = new QLineEdit( m_advancedPanel );
	m_photorecPathEdit->setText( QApplication::applicationDirPath() + QStringLiteral( "/photorec.exe" ) );
	m_browsePhotorecPathButton = new QPushButton( QStringLiteral( "浏览..." ), m_advancedPanel );
	m_browsePhotorecPathButton->setProperty( "role", "ghost" );
	photorecRow->addWidget( new QLabel( QStringLiteral( "PhotoRec 程序:" ), m_advancedPanel ) );
	photorecRow->addWidget( m_photorecPathEdit, 1 );
	photorecRow->addWidget( m_browsePhotorecPathButton );
	advancedLayout->addLayout( photorecRow );

	m_partitionRangeCheckBox = new QCheckBox(
	     QStringLiteral( "仅对指定分区范围查找（已知分区边界时可以缩小范围、加快速度）" ), m_advancedPanel );
	advancedLayout->addWidget( m_partitionRangeCheckBox );

	QHBoxLayout *partitionRow = new QHBoxLayout();
	m_partitionOffsetEdit = new QLineEdit( m_advancedPanel );
	m_partitionOffsetEdit->setPlaceholderText( QStringLiteral( "分区偏移（字节）" ) );
	m_partitionSizeEdit = new QLineEdit( m_advancedPanel );
	m_partitionSizeEdit->setPlaceholderText( QStringLiteral( "分区大小（字节）" ) );
	partitionRow->addWidget( new QLabel( QStringLiteral( "分区偏移:" ), m_advancedPanel ) );
	partitionRow->addWidget( m_partitionOffsetEdit );
	partitionRow->addWidget( new QLabel( QStringLiteral( "分区大小:" ), m_advancedPanel ) );
	partitionRow->addWidget( m_partitionSizeEdit );
	advancedLayout->addLayout( partitionRow );
	m_partitionOffsetEdit->setEnabled( false );
	m_partitionSizeEdit->setEnabled( false );

	m_advancedPanel->setVisible( false );
	mainLayout->addWidget( m_advancedPanel );

	QHBoxLayout *buttonRow = new QHBoxLayout();
	buttonRow->addStretch( 1 );
	m_startButton = new QPushButton( QStringLiteral( "开始查找并保存" ), this );
	m_startButton->setProperty( "role", "primary" );
	m_cancelButton = new QPushButton( QStringLiteral( "停止" ), this );
	m_cancelButton->setProperty( "role", "danger-ghost" );
	m_cancelButton->setEnabled( false );
	buttonRow->addWidget( m_cancelButton );
	buttonRow->addWidget( m_startButton );
	mainLayout->addLayout( buttonRow );

	m_progressBar = new QProgressBar( this );
	m_progressBar->setRange( 0, 0 );
	m_progressBar->setTextVisible( false );
	m_progressBar->setVisible( false );
	mainLayout->addWidget( m_progressBar );

	m_elapsedLabel = new QLabel( this );
	m_elapsedLabel->setProperty( "role", "secondary" );
	mainLayout->addWidget( m_elapsedLabel );

	m_resultLabel = new QLabel( QStringLiteral( "耗时取决于设备大小和读取速度，可能几分钟到几小时。" ), this );
	m_resultLabel->setWordWrap( true );
	mainLayout->addWidget( m_resultLabel );

	m_openOutputButton = new QPushButton( QStringLiteral( "打开输出文件夹" ), this );
	m_openOutputButton->setVisible( false );
	connect( m_openOutputButton, &QPushButton::clicked, this, [ this ]() {
		QDesktopServices::openUrl( QUrl::fromLocalFile( m_lastRecupDirPath ) );
	} );
	mainLayout->addWidget( m_openOutputButton, 0, Qt::AlignLeft );
	mainLayout->addStretch( 1 );

	m_elapsedTimer = new QTimer( this );
	m_elapsedTimer->setInterval( 1000 );
	connect( m_elapsedTimer, &QTimer::timeout, this, &CarvePage::onElapsedTimerTick );

	m_worker = new CarveWorker();
	m_worker->moveToThread( &m_workerThread );
	connect( &m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater );
	connect( this, &CarvePage::requestStartCarve, m_worker, &CarveWorker::startCarve );
	connect( this, &CarvePage::requestStartCarvePartition, m_worker, &CarveWorker::startCarvePartition );
	connect( m_worker, &CarveWorker::carveFinished, this, &CarvePage::onCarveFinished );
	m_workerThread.start();

	connect( m_browsePhotorecPathButton, &QPushButton::clicked, this, &CarvePage::onBrowsePhotorecPathClicked );
	connect( m_browseTargetButton, &QPushButton::clicked, this, &CarvePage::onBrowseTargetClicked );
	connect( m_pickDiskButton, &QPushButton::clicked, this, &CarvePage::onPickDiskClicked );
	connect( m_browseOutputDirButton, &QPushButton::clicked, this, &CarvePage::onBrowseOutputDirClicked );
	connect( m_partitionRangeCheckBox, &QCheckBox::toggled, this, &CarvePage::onPartitionRangeToggled );
	connect( m_advancedToggleButton, &QPushButton::clicked, this, &CarvePage::onAdvancedToggled );
	connect( m_startButton, &QPushButton::clicked, this, &CarvePage::onStartClicked );
	connect( m_cancelButton, &QPushButton::clicked, this, &CarvePage::onCancelClicked );
}

CarvePage::~CarvePage()
{
	if( m_running )
	{
		m_worker->requestCancel();
	}
	m_workerThread.quit();
	m_workerThread.wait();
}

bool CarvePage::isRunning() const
{
	return m_running;
}

void CarvePage::requestCancelAndWait()
{
	if( m_running )
	{
		m_worker->requestCancel();
	}
}

void CarvePage::onBrowsePhotorecPathClicked()
{
	QString path = QFileDialog::getOpenFileName( this, QStringLiteral( "选择 PhotoRec 可执行文件" ) );
	if( !path.isEmpty() )
	{
		m_photorecPathEdit->setText( path );
	}
}

void CarvePage::onBrowseTargetClicked()
{
	QString path = QFileDialog::getOpenFileName( this, QStringLiteral( "选择镜像文件（取消则改为手动填写设备路径）" ) );
	if( !path.isEmpty() )
	{
		m_targetPathEdit->setText( path );
	}
}

void CarvePage::onPickDiskClicked()
{
	physical_disk_enum_result_t result = {};

	if( disk_enumerator_list_physical_drives( &result ) != 1 || result.count == 0 )
	{
		QMessageBox::warning( this, QStringLiteral( "提示" ),
		     QStringLiteral( "没有找到可用的磁盘——可能需要以管理员身份运行本程序才能看到物理磁盘列表。" ) );
		physical_disk_enum_result_free( &result );
		return;
	}

	QDialog dialog( this );
	dialog.setWindowTitle( QStringLiteral( "选择磁盘" ) );
	QVBoxLayout *dialogLayout = new QVBoxLayout( &dialog );
	QListWidget *list = new QListWidget( &dialog );

	for( int i = 0; i < result.count; i++ )
	{
		const physical_disk_entry_t &entry = result.entries[ i ];
		QString label = QStringLiteral( "磁盘 %1" ).arg( entry.disk_number );
		if( entry.model[ 0 ] != '\0' )
		{
			label += QStringLiteral( "：%1" ).arg( QString::fromLocal8Bit( entry.model ) );
		}
		label += QStringLiteral( "　%1" ).arg( carveFormatDiskSize( entry.size_bytes ) );
		if( entry.is_removable == 1 )
		{
			label += QStringLiteral( "　[可移动]" );
		}
		QListWidgetItem *item = new QListWidgetItem( label, list );
		item->setData( Qt::UserRole, entry.disk_number );
	}
	physical_disk_enum_result_free( &result );

	list->setCurrentRow( 0 );
	dialogLayout->addWidget( new QLabel( QStringLiteral( "深度查找会对整块磁盘逐字节扫描，请选择要查找的磁盘：" ), &dialog ) );
	dialogLayout->addWidget( list );

	QDialogButtonBox *buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
	dialogLayout->addWidget( buttons );
	connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
	connect( list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept );

	if( dialog.exec() == QDialog::Accepted && list->currentItem() != nullptr )
	{
		int diskNumber = list->currentItem()->data( Qt::UserRole ).toInt();
		m_targetPathEdit->setText( QStringLiteral( "\\\\.\\PhysicalDrive%1" ).arg( diskNumber ) );
	}
}

void CarvePage::onBrowseOutputDirClicked()
{
	QString dir = QFileDialog::getExistingDirectory( this, QStringLiteral( "选择输出目录" ) );
	if( !dir.isEmpty() )
	{
		m_outputDirEdit->setText( dir );
	}
}

void CarvePage::onAdvancedToggled()
{
	bool nowVisible = !m_advancedPanel->isVisible();
	m_advancedPanel->setVisible( nowVisible );
	m_advancedToggleButton->setText( nowVisible ? QStringLiteral( "高级设置 ▴" ) : QStringLiteral( "高级设置 ▾" ) );
}

void CarvePage::onPartitionRangeToggled( bool checked )
{
	m_partitionOffsetEdit->setEnabled( checked );
	m_partitionSizeEdit->setEnabled( checked );
	m_targetPathEdit->setPlaceholderText(
	     checked ? QStringLiteral( "物理盘路径（比如 \\\\.\\PhysicalDrive0）或镜像文件路径——先从这里切出下面指定的字节范围" )
	             : QStringLiteral( "设备路径（比如 \\\\.\\PhysicalDrive0）或镜像文件路径" ) );
}

void CarvePage::onStartClicked()
{
	bool partitionRange = m_partitionRangeCheckBox->isChecked();

	if( m_photorecPathEdit->text().trimmed().isEmpty()
	 || m_targetPathEdit->text().trimmed().isEmpty()
	 || m_outputDirEdit->text().trimmed().isEmpty() )
	{
		QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先填写 PhotoRec 可执行文件、查找来源、保存位置" ) );
		return;
	}

	bool offsetOk = false;
	bool sizeOk = false;
	qint64 offset = 0;
	quint64 size = 0;

	if( partitionRange )
	{
		offset = m_partitionOffsetEdit->text().trimmed().toLongLong( &offsetOk );
		size = m_partitionSizeEdit->text().trimmed().toULongLong( &sizeOk );
		if( !offsetOk || !sizeOk || size == 0 )
		{
			QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请填写有效的分区偏移和大小（大小必须大于0）" ) );
			return;
		}
	}

	int confirm = QMessageBox::question( this, QStringLiteral( "确认" ),
	     QStringLiteral( "即将对\n%1\n做深度查找，这个过程可能耗时很久且目前无法预估进度，确定开始吗？" ).arg( m_targetPathEdit->text() ) );
	if( confirm != QMessageBox::Yes )
	{
		return;
	}

	m_openOutputButton->setVisible( false );
	setRunning( true );
	m_resultLabel->setText( partitionRange ? QStringLiteral( "正在准备分区范围（这一步暂不支持取消）…" ) : QStringLiteral( "正在深度查找…" ) );
	m_stopwatch.start();
	m_elapsedTimer->start();
	onElapsedTimerTick();

	QString outputDirBase = m_outputDirEdit->text().trimmed() + QStringLiteral( "/recup_dir" );
	if( partitionRange )
	{
		emit requestStartCarvePartition( m_photorecPathEdit->text().trimmed(), m_targetPathEdit->text().trimmed(), offset, size, outputDirBase );
	}
	else
	{
		emit requestStartCarve( m_photorecPathEdit->text().trimmed(), m_targetPathEdit->text().trimmed(), outputDirBase );
	}
}

void CarvePage::onCancelClicked()
{
	m_cancelButton->setEnabled( false );
	m_resultLabel->setText( QStringLiteral( "正在停止..." ) );
	m_worker->requestCancel();
}

void CarvePage::onCarveFinished( bool success, bool cancelled, const QString &errorMessage, const QString &recupDirPath, int exitCode, int recoveredFileCount, int badSectorCount )
{
	setRunning( false );
	m_elapsedTimer->stop();

	if( cancelled )
	{
		m_resultLabel->setText( QStringLiteral( "已停止，已保存的部分文件仍保留在输出目录中。" ) );
		return;
	}
	if( !success )
	{
		m_resultLabel->setText( QStringLiteral( "失败：%1" ).arg( errorMessage ) );
		return;
	}
	if( recupDirPath.isEmpty() )
	{
		m_resultLabel->setText( QStringLiteral( "PhotoRec 已退出（退出码 %1），但没有找到它生成的输出目录。" ).arg( exitCode ) );
		return;
	}

	QString resultText = QStringLiteral( "深度查找完成\n找回 %1 个文件\n保存到：%2" ).arg( recoveredFileCount ).arg( recupDirPath );
	if( badSectorCount > 0 )
	{
		resultText += QStringLiteral( "\n（准备分区范围时遇到 %1 处坏道，已跳过，不影响其它部分的结果）" ).arg( badSectorCount );
	}
	m_resultLabel->setText( resultText );

	m_lastRecupDirPath = recupDirPath;
	m_openOutputButton->setVisible( true );
}

void CarvePage::onElapsedTimerTick()
{
	qint64 secs = m_stopwatch.elapsed() / 1000;
	m_elapsedLabel->setText( QStringLiteral( "已用时 %1:%2" ).arg( secs / 60, 2, 10, QLatin1Char( '0' ) ).arg( secs % 60, 2, 10, QLatin1Char( '0' ) ) );
}

void CarvePage::setRunning( bool running )
{
	m_running = running;
	m_startButton->setEnabled( !running );
	m_cancelButton->setEnabled( running );
	m_photorecPathEdit->setEnabled( !running );
	m_browsePhotorecPathButton->setEnabled( !running );
	m_targetPathEdit->setEnabled( !running );
	m_browseTargetButton->setEnabled( !running );
	m_pickDiskButton->setEnabled( !running );
	m_outputDirEdit->setEnabled( !running );
	m_browseOutputDirButton->setEnabled( !running );
	m_partitionRangeCheckBox->setEnabled( !running );
	if( !running )
	{
		m_partitionOffsetEdit->setEnabled( m_partitionRangeCheckBox->isChecked() );
		m_partitionSizeEdit->setEnabled( m_partitionRangeCheckBox->isChecked() );
	}
	else
	{
		m_partitionOffsetEdit->setEnabled( false );
		m_partitionSizeEdit->setEnabled( false );
	}
	m_progressBar->setVisible( running );
	emit runningChanged( running );
}
