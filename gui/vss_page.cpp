#include "vss_page.h"

#include <QDateTime>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QWidget>

extern "C" {
#include <libvshadow.h>
}

namespace
{

/* 和原VssWizardDialog::formatFiletimeAsUtcString同样的换算公式和
 * 展示格式选择理由——这段逻辑原样搬过来，没有改动 */
QString formatFiletimeAsLocalString( quint64 filetime )
{
	qint64 unixSeconds = (qint64) ( ( filetime / 10000000ULL ) - 11644473600ULL );
	QDateTime dt = QDateTime::fromSecsSinceEpoch( unixSeconds, QTimeZone::UTC );
	if( !dt.isValid() )
	{
		return QStringLiteral( "(无法解析时间)" );
	}
	return dt.toLocalTime().toString( QStringLiteral( "yyyy-MM-dd HH:mm" ) );
}

} // namespace

VssPage::VssPage( QWidget *parent )
	: QWidget( parent )
	, m_driveLetterEdit( nullptr )
	, m_queryOnlineButton( nullptr )
	, m_onlineList( nullptr )
	, m_imagePathEdit( nullptr )
	, m_browseImageButton( nullptr )
	, m_listOfflineStoresButton( nullptr )
	, m_offlineList( nullptr )
	, m_tabs( nullptr )
	, m_selectionSummaryLabel( nullptr )
	, m_scanButton( nullptr )
{
	QVBoxLayout *mainLayout = new QVBoxLayout( this );
	mainLayout->setContentsMargins( 32, 28, 32, 32 );
	mainLayout->setSpacing( 20 );

	QLabel *title = new QLabel( QStringLiteral( "从历史版本找回文件" ), this );
	title->setObjectName( QStringLiteral( "pageTitle" ) );
	QLabel *subtitle = new QLabel( QStringLiteral( "查找 Windows 自动保留的历史快照，找回更早时间的文件版本。" ), this );
	subtitle->setObjectName( QStringLiteral( "pageSubtitle" ) );
	mainLayout->addWidget( title );
	mainLayout->addWidget( subtitle );

	m_tabs = new QTabWidget( this );

	QWidget *onlinePage = new QWidget( m_tabs );
	QVBoxLayout *onlineLayout = new QVBoxLayout( onlinePage );
	onlineLayout->setContentsMargins( 16, 16, 16, 16 );
	QHBoxLayout *onlineQueryRow = new QHBoxLayout();
	m_driveLetterEdit = new QLineEdit( onlinePage );
	m_driveLetterEdit->setPlaceholderText( QStringLiteral( "盘符，比如 C" ) );
	m_driveLetterEdit->setMaxLength( 1 );
	m_queryOnlineButton = new QPushButton( QStringLiteral( "查询" ), onlinePage );
	m_queryOnlineButton->setProperty( "role", "ghost" );
	onlineQueryRow->addWidget( new QLabel( QStringLiteral( "盘符:" ), onlinePage ) );
	onlineQueryRow->addWidget( m_driveLetterEdit );
	onlineQueryRow->addWidget( m_queryOnlineButton );
	onlineQueryRow->addStretch( 1 );
	onlineLayout->addLayout( onlineQueryRow );
	m_onlineList = new QListWidget( onlinePage );
	onlineLayout->addWidget( m_onlineList, 1 );
	m_tabs->addTab( onlinePage, QStringLiteral( "本机历史版本" ) );

	QWidget *offlinePage = new QWidget( m_tabs );
	QVBoxLayout *offlineLayout = new QVBoxLayout( offlinePage );
	offlineLayout->setContentsMargins( 16, 16, 16, 16 );
	QHBoxLayout *offlinePathRow = new QHBoxLayout();
	m_imagePathEdit = new QLineEdit( offlinePage );
	m_imagePathEdit->setPlaceholderText( QStringLiteral( "镜像文件路径" ) );
	m_browseImageButton = new QPushButton( QStringLiteral( "浏览..." ), offlinePage );
	m_browseImageButton->setProperty( "role", "ghost" );
	offlinePathRow->addWidget( new QLabel( QStringLiteral( "镜像文件:" ), offlinePage ) );
	offlinePathRow->addWidget( m_imagePathEdit, 1 );
	offlinePathRow->addWidget( m_browseImageButton );
	offlineLayout->addLayout( offlinePathRow );
	m_listOfflineStoresButton = new QPushButton( QStringLiteral( "列出历史版本" ), offlinePage );
	m_listOfflineStoresButton->setProperty( "role", "ghost" );
	offlineLayout->addWidget( m_listOfflineStoresButton, 0, Qt::AlignLeft );
	m_offlineList = new QListWidget( offlinePage );
	offlineLayout->addWidget( m_offlineList, 1 );
	m_tabs->addTab( offlinePage, QStringLiteral( "从磁盘镜像读取" ) );

	mainLayout->addWidget( m_tabs, 1 );

	QHBoxLayout *selectionBar = new QHBoxLayout();
	m_selectionSummaryLabel = new QLabel( QStringLiteral( "请先查询并选中一个历史版本" ), this );
	m_selectionSummaryLabel->setProperty( "role", "secondary" );
	m_scanButton = new QPushButton( QStringLiteral( "扫描此版本" ), this );
	m_scanButton->setProperty( "role", "primary" );
	m_scanButton->setEnabled( false );
	selectionBar->addWidget( m_selectionSummaryLabel );
	selectionBar->addStretch( 1 );
	selectionBar->addWidget( m_scanButton );
	mainLayout->addLayout( selectionBar );

	connect( m_queryOnlineButton, &QPushButton::clicked, this, &VssPage::onQueryOnlineClicked );
	connect( m_browseImageButton, &QPushButton::clicked, this, &VssPage::onBrowseImageClicked );
	connect( m_listOfflineStoresButton, &QPushButton::clicked, this, &VssPage::onListOfflineStoresClicked );
	connect( m_scanButton, &QPushButton::clicked, this, &VssPage::onScanClicked );
	connect( m_onlineList, &QListWidget::currentItemChanged, this, &VssPage::onSelectionChanged );
	connect( m_offlineList, &QListWidget::currentItemChanged, this, &VssPage::onSelectionChanged );
	connect( m_tabs, &QTabWidget::currentChanged, this, &VssPage::onSelectionChanged );
}

void VssPage::onQueryOnlineClicked()
{
	QString driveLetter = m_driveLetterEdit->text().trimmed().toUpper();
	if( driveLetter.isEmpty() )
	{
		QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先填写盘符" ) );
		return;
	}

	m_onlineList->clear();

	QString command = QStringLiteral(
	     "$v = (Get-Volume -DriveLetter %1).Path; "
	     "Get-CimInstance Win32_ShadowCopy | Where-Object { $_.VolumeName -eq $v } | "
	     "ForEach-Object { $_.ID + '|' + $_.DeviceObject + '|' + $_.InstallDate }" )
	     .arg( driveLetter );

	QProcess process;
	process.start( QStringLiteral( "powershell" ), { QStringLiteral( "-NoProfile" ), QStringLiteral( "-Command" ), command } );
	if( !process.waitForFinished( 15000 ) )
	{
		QMessageBox::critical( this, QStringLiteral( "查询失败" ), QStringLiteral( "PowerShell 查询超时或无法启动" ) );
		return;
	}

	QString output = QString::fromLocal8Bit( process.readAllStandardOutput() );
	const QStringList lines = output.split( QLatin1Char( '\n' ), Qt::SkipEmptyParts );

	for( const QString &rawLine : lines )
	{
		QString line = rawLine.trimmed();
		if( line.isEmpty() )
		{
			continue;
		}
		QStringList parts = line.split( QLatin1Char( '|' ) );
		if( parts.count() < 3 )
		{
			continue;
		}
		QString devicePath = parts.at( 1 );
		QString installDate = parts.at( 2 );

		QListWidgetItem *listItem = new QListWidgetItem( installDate, m_onlineList );
		listItem->setToolTip( devicePath );
		listItem->setData( Qt::UserRole, devicePath );
	}

	if( m_onlineList->count() == 0 )
	{
		QMessageBox::information( this, QStringLiteral( "没有找到" ),
		     QStringLiteral( "没有检测到可用的历史快照——这个盘可能没有开启\"系统保护\"/卷影复制服务，"
		                      "或者历史快照已经过期被系统自动清理，这不是本工具的问题。" ) );
	}
	onSelectionChanged();
}

void VssPage::onBrowseImageClicked()
{
	QString path = QFileDialog::getOpenFileName( this, QStringLiteral( "选择镜像文件" ) );
	if( !path.isEmpty() )
	{
		m_imagePathEdit->setText( path );
	}
}

void VssPage::onListOfflineStoresClicked()
{
	QString imagePath = m_imagePathEdit->text().trimmed();
	if( imagePath.isEmpty() )
	{
		QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先选择镜像文件" ) );
		return;
	}

	m_offlineList->clear();

	QByteArray pathUtf8 = imagePath.toUtf8();
	libvshadow_volume_t *volume = nullptr;
	libvshadow_error_t *error = nullptr;
	int numberOfStores = 0;

	if( libvshadow_volume_initialize( &volume, &error ) != 1 )
	{
		QMessageBox::critical( this, QStringLiteral( "失败" ), QStringLiteral( "无法初始化 vshadow volume 对象" ) );
		return;
	}
	if( libvshadow_volume_open( volume, pathUtf8.constData(), LIBVSHADOW_OPEN_READ, &error ) != 1 )
	{
		QMessageBox::critical( this, QStringLiteral( "失败" ), QStringLiteral( "无法把这个镜像当作包含VSS数据的卷来解析" ) );
		if( error != nullptr )
		{
			libvshadow_error_free( &error );
		}
		libvshadow_volume_free( &volume, nullptr );
		return;
	}
	if( libvshadow_volume_get_number_of_stores( volume, &numberOfStores, &error ) != 1 )
	{
		QMessageBox::critical( this, QStringLiteral( "失败" ), QStringLiteral( "无法枚举卷影副本数量" ) );
		if( error != nullptr )
		{
			libvshadow_error_free( &error );
		}
		libvshadow_volume_close( volume, nullptr );
		libvshadow_volume_free( &volume, nullptr );
		return;
	}

	for( int i = 0; i < numberOfStores; i++ )
	{
		libvshadow_store_t *store = nullptr;
		quint64 creationFiletime = 0;
		QString timeString = QStringLiteral( "(未知)" );

		if( libvshadow_volume_get_store( volume, i, &store, &error ) != 1 )
		{
			if( error != nullptr )
			{
				libvshadow_error_free( &error );
			}
			continue;
		}
		if( libvshadow_store_get_creation_time( store, &creationFiletime, &error ) == 1 )
		{
			timeString = formatFiletimeAsLocalString( creationFiletime );
		}
		else if( error != nullptr )
		{
			libvshadow_error_free( &error );
		}

		QListWidgetItem *listItem = new QListWidgetItem( timeString, m_offlineList );
		listItem->setToolTip( QStringLiteral( "store 序号 %1" ).arg( i ) );
		listItem->setData( Qt::UserRole, i );
		libvshadow_store_free( &store, nullptr );
	}

	libvshadow_volume_close( volume, nullptr );
	libvshadow_volume_free( &volume, nullptr );

	if( m_offlineList->count() == 0 )
	{
		QMessageBox::information( this, QStringLiteral( "没有找到" ),
		     QStringLiteral( "没有检测到任何卷影副本数据——这个镜像对应的卷可能没有开启过"
		                      "\"系统保护\"/卷影复制服务，这不是本工具的问题。" ) );
	}
	onSelectionChanged();
}

void VssPage::onSelectionChanged()
{
	bool onlineTab = ( m_tabs->currentIndex() == 0 );
	QListWidgetItem *item = onlineTab ? m_onlineList->currentItem() : m_offlineList->currentItem();

	if( item == nullptr )
	{
		m_scanButton->setEnabled( false );
		m_selectionSummaryLabel->setText( QStringLiteral( "请先查询并选中一个历史版本" ) );
		return;
	}

	m_scanButton->setEnabled( true );
	m_selectionSummaryLabel->setText( QStringLiteral( "已选择：%1" ).arg( item->text() ) );
}

void VssPage::onScanClicked()
{
	ScanSource source;

	if( m_tabs->currentIndex() == 0 )
	{
		QListWidgetItem *item = m_onlineList->currentItem();
		if( item == nullptr )
		{
			QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先查询并选中一个快照" ) );
			return;
		}
		source.type = ScanSourceType::DirectDevice;
		source.devicePath = item->data( Qt::UserRole ).toString();
		source.storeIndex = 0;
	}
	else
	{
		QListWidgetItem *item = m_offlineList->currentItem();
		if( item == nullptr )
		{
			QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先列出并选中一个卷影副本" ) );
			return;
		}
		source.type = ScanSourceType::VssOffline;
		source.devicePath = m_imagePathEdit->text().trimmed();
		source.storeIndex = item->data( Qt::UserRole ).toInt();
	}

	emit scanRequested( source );
}
