#include "main_window.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSize>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <cstring>

#include "bitlocker_wizard_dialog.h"
#include "carve_page.h"
#include "device_card.h"
#include "icon_painter.h"
#include "scan_worker.h"
#include "vss_page.h"

extern "C" {
#include "../engine/shell/bitlocker_status_checker.h"
#include "../engine/shell/disk_enumerator.h"
#include "../engine/shell/volume_disk_extents.h"
}

namespace
{

const int kOriginalIndexRole = Qt::UserRole + 1;
const int kDeviceCardColumns = 3;

bool looksLikeSupportedImage( const QString &fileName )
{
	int dotIndex = fileName.lastIndexOf( QLatin1Char( '.' ) );
	if( dotIndex < 0 || dotIndex == fileName.length() - 1 )
	{
		return false;
	}
	QByteArray extension = fileName.mid( dotIndex + 1 ).toLower().toLatin1();

	const QList<QByteArray> supported = QImageReader::supportedImageFormats();
	return supported.contains( extension );
}

/* 和cli/main.c的resolve_device_path同样的规则，重复一份见main_window.cpp
 * 历史版本里的同名函数注释——这次重构没有改变这条既有理由 */
QString resolveDevicePath( const QString &input )
{
	QString trimmed = input.trimmed();

	if( trimmed.isEmpty() )
	{
		return trimmed;
	}

	bool allDigits = true;
	for( const QChar &c : trimmed )
	{
		if( !c.isDigit() )
		{
			allDigits = false;
			break;
		}
	}
	if( allDigits )
	{
		return QStringLiteral( "\\\\.\\PhysicalDrive%1" ).arg( trimmed );
	}

	if( ( trimmed.length() == 1 && trimmed.at( 0 ).isLetter() )
	 || ( trimmed.length() == 2 && trimmed.at( 0 ).isLetter() && trimmed.at( 1 ) == QLatin1Char( ':' ) ) )
	{
		return QStringLiteral( "\\\\.\\%1:" ).arg( trimmed.at( 0 ).toUpper() );
	}

	return trimmed;
}

QString fsTypeLabel( CandidateFsType type )
{
	switch( type )
	{
		case CandidateFsType::Ntfs:
			return QStringLiteral( "NTFS" );
		case CandidateFsType::Fat32:
			return QStringLiteral( "FAT32" );
		case CandidateFsType::ExFat:
			return QStringLiteral( "exFAT" );
	}
	return QString();
}

QString formatFileSize( quint64 bytes )
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
	return QStringLiteral( "%1 %2" ).arg( value, 0, 'f', 1 ).arg( QString::fromLatin1( units[ unitIndex ] ) );
}

class NumericSizeItem : public QTableWidgetItem
{
public:
	explicit NumericSizeItem( const QString &text )
		: QTableWidgetItem( text )
	{
	}

	bool operator<( const QTableWidgetItem &other ) const override
	{
		return data( Qt::UserRole + 2 ).toULongLong() < other.data( Qt::UserRole + 2 ).toULongLong();
	}
};

QFrame *makeCardFrame( QWidget *parent )
{
	QFrame *frame = new QFrame( parent );
	frame->setProperty( "card", true );
	return frame;
}

void setButtonRole( QPushButton *button, const char *role )
{
	button->setProperty( "role", role );
}

QPushButton *makeNavButton( const QString &text, const QPixmap &icon, QWidget *parent )
{
	QPushButton *button = new QPushButton( QIcon( icon ), text, parent );
	button->setProperty( "navItem", true );
	button->setIconSize( QSize( 16, 16 ) );
	button->setFlat( true );
	return button;
}

QLabel *makeGroupTitle( const QString &text, QWidget *parent )
{
	QLabel *label = new QLabel( text, parent );
	label->setProperty( "role", "navGroupTitle" );
	label->setContentsMargins( 10, 0, 0, 4 );
	return label;
}

} // namespace

MainWindow::MainWindow( QWidget *parent )
	: QMainWindow( parent )
	, m_navDisksButton( nullptr )
	, m_navHistoryButton( nullptr )
	, m_navDeepSearchButton( nullptr )
	, m_navAboutButton( nullptr )
	, m_sidebarTaskLabel( nullptr )
	, m_contentStack( nullptr )
	, m_homeStack_picker( nullptr )
	, m_fixedCardGrid( nullptr )
	, m_removableCardGrid( nullptr )
	, m_removableSection( nullptr )
	, m_selectedCard( nullptr )
	, m_selectionSummaryLabel( nullptr )
	, m_scanButton( nullptr )
	, m_refreshButton( nullptr )
	, m_openImageButton( nullptr )
	, m_unlockBitLockerButton( nullptr )
	, m_advancedToggleButton( nullptr )
	, m_advancedPanel( nullptr )
	, m_devicePathEdit( nullptr )
	, m_homeStack( nullptr )
	, m_homeStack_results( nullptr )
	, m_resultsBreadcrumbLabel( nullptr )
	, m_filterEdit( nullptr )
	, m_selectAllHeaderCheckBox( nullptr )
	, m_resultsStack( nullptr )
	, m_emptyStateLabel( nullptr )
	, m_table( nullptr )
	, m_previewLabel( nullptr )
	, m_checkedSummaryLabel( nullptr )
	, m_recoverButton( nullptr )
	, m_progressBar( nullptr )
	, m_statusLabel( nullptr )
	, m_elapsedLabel( nullptr )
	, m_elapsedTimer( nullptr )
	, m_busy( false )
	, m_vssPage( nullptr )
	, m_carvePage( nullptr )
	, m_worker( nullptr )
	, m_batchTotal( 0 )
{
	setWindowTitle( QStringLiteral( "DataRecoveryStudio" ) );
	setWindowIcon( IconPainter::appIcon() );
	resize( 1180, 760 );

	QWidget *central = new QWidget( this );
	QHBoxLayout *rootLayout = new QHBoxLayout( central );
	rootLayout->setContentsMargins( 0, 0, 0, 0 );
	rootLayout->setSpacing( 0 );

	/* ============ 侧栏 ============ */
	QWidget *sidebar = new QWidget( central );
	sidebar->setObjectName( QStringLiteral( "sidebar" ) );
	sidebar->setFixedWidth( 208 );
	QVBoxLayout *sidebarLayout = new QVBoxLayout( sidebar );
	sidebarLayout->setContentsMargins( 14, 18, 14, 16 );
	sidebarLayout->setSpacing( 3 );

	QHBoxLayout *brandRow = new QHBoxLayout();
	brandRow->setSpacing( 9 );
	QLabel *brandIconLabel = new QLabel( sidebar );
	brandIconLabel->setPixmap( IconPainter::appGlyph( 26 ) );
	QLabel *brandNameLabel = new QLabel( QStringLiteral( "DataRecoveryStudio" ), sidebar );
	brandNameLabel->setObjectName( QStringLiteral( "sidebarBrandName" ) );
	brandRow->addWidget( brandIconLabel );
	brandRow->addWidget( brandNameLabel, 1 );
	sidebarLayout->addLayout( brandRow );
	sidebarLayout->addSpacing( 18 );

	sidebarLayout->addWidget( makeGroupTitle( QStringLiteral( "文件恢复" ), sidebar ) );
	m_navDisksButton = makeNavButton( QStringLiteral( "磁盘与镜像" ), IconPainter::navGlyph( IconPainter::NavIcon::Disks, 16 ), sidebar );
	m_navDisksButton->setCheckable( true );
	m_navHistoryButton = makeNavButton( QStringLiteral( "历史版本" ), IconPainter::navGlyph( IconPainter::NavIcon::History, 16 ), sidebar );
	m_navHistoryButton->setCheckable( true );
	sidebarLayout->addWidget( m_navDisksButton );
	sidebarLayout->addWidget( m_navHistoryButton );
	sidebarLayout->addSpacing( 14 );

	sidebarLayout->addWidget( makeGroupTitle( QStringLiteral( "深度恢复" ), sidebar ) );
	m_navDeepSearchButton = makeNavButton( QStringLiteral( "深度查找" ), IconPainter::navGlyph( IconPainter::NavIcon::DeepSearch, 16 ), sidebar );
	m_navDeepSearchButton->setCheckable( true );
	sidebarLayout->addWidget( m_navDeepSearchButton );

	QButtonGroup *navGroup = new QButtonGroup( this );
	navGroup->setExclusive( true );
	navGroup->addButton( m_navDisksButton );
	navGroup->addButton( m_navHistoryButton );
	navGroup->addButton( m_navDeepSearchButton );
	m_navDisksButton->setChecked( true );

	sidebarLayout->addStretch( 1 );

	m_sidebarTaskLabel = new QLabel( QStringLiteral( "深度查找运行中…" ), sidebar );
	m_sidebarTaskLabel->setProperty( "role", "sidebarTask" );
	m_sidebarTaskLabel->setWordWrap( true );
	m_sidebarTaskLabel->setVisible( false );
	sidebarLayout->addWidget( m_sidebarTaskLabel );
	sidebarLayout->addSpacing( 6 );

	m_navAboutButton = makeNavButton( QStringLiteral( "关于" ), IconPainter::navGlyph( IconPainter::NavIcon::About, 16 ), sidebar );
	sidebarLayout->addWidget( m_navAboutButton );

	rootLayout->addWidget( sidebar );

	/* ============ 内容区：三个一级页面 ============ */
	m_contentStack = new QStackedWidget( central );

	/* ---- 页面0：磁盘与镜像（内部还有picker/results两级子页） ---- */
	QWidget *homePage = new QWidget( m_contentStack );
	QVBoxLayout *homePageLayout = new QVBoxLayout( homePage );
	homePageLayout->setContentsMargins( 0, 0, 0, 0 );
	/* 2026-11-06（世杰反馈：窗口没法上下调整高度）：QStackedWidget默认
	 * 按"所有子页面里最高的那个"算最小高度（不是只算当前显示的那个），
	 * 磁盘卡片一多、或者窗口本身被拉得比内容还小，这个最小高度会顶到
	 * 主窗口头上，表现就是窗口拖不动/拖不小。套一层QScrollArea（
	 * setWidgetResizable(true)）：窗口够大时内容跟着撑满，窗口被拉小到
	 * 内容放不下时变成内部滚动，而不是顶住窗口不让缩小。 */
	QScrollArea *homeScrollArea = new QScrollArea( homePage );
	homeScrollArea->setWidgetResizable( true );
	homeScrollArea->setFrameShape( QFrame::NoFrame );
	m_homeStack = new QStackedWidget( homeScrollArea );
	homeScrollArea->setWidget( m_homeStack );
	homePageLayout->addWidget( homeScrollArea );

	/* ---- picker子页 ---- */
	QWidget *pickerPage = new QWidget( m_homeStack );
	m_homeStack_picker = pickerPage;
	QVBoxLayout *pickerLayout = new QVBoxLayout( pickerPage );
	pickerLayout->setContentsMargins( 32, 28, 32, 28 );
	pickerLayout->setSpacing( 20 );

	QHBoxLayout *pickerHeaderRow = new QHBoxLayout();
	QVBoxLayout *pickerHeaderTextCol = new QVBoxLayout();
	QLabel *pickerTitle = new QLabel( QStringLiteral( "从哪里找回文件？" ), pickerPage );
	pickerTitle->setObjectName( QStringLiteral( "pageTitle" ) );
	QLabel *pickerSubtitle = new QLabel( QStringLiteral( "选择文件原来所在的磁盘、分区或磁盘镜像。" ), pickerPage );
	pickerSubtitle->setObjectName( QStringLiteral( "pageSubtitle" ) );
	pickerHeaderTextCol->addWidget( pickerTitle );
	pickerHeaderTextCol->addWidget( pickerSubtitle );
	m_refreshButton = new QPushButton( QStringLiteral( "刷新设备" ), pickerPage );
	setButtonRole( m_refreshButton, "ghost" );
	m_refreshButton->setIcon( QIcon( IconPainter::refreshGlyph( 14 ) ) );
	pickerHeaderRow->addLayout( pickerHeaderTextCol, 1 );
	pickerHeaderRow->addWidget( m_refreshButton, 0, Qt::AlignTop );
	pickerLayout->addLayout( pickerHeaderRow );

	QWidget *fixedSection = new QWidget( pickerPage );
	QVBoxLayout *fixedSectionLayout = new QVBoxLayout( fixedSection );
	fixedSectionLayout->setContentsMargins( 0, 0, 0, 0 );
	fixedSectionLayout->setSpacing( 10 );
	QLabel *fixedSectionTitle = new QLabel( QStringLiteral( "本机磁盘" ), fixedSection );
	fixedSectionTitle->setStyleSheet( QStringLiteral( "font-size: 13px; font-weight: 600; color: #596A70;" ) );
	m_fixedCardGrid = new QGridLayout();
	m_fixedCardGrid->setSpacing( 14 );
	fixedSectionLayout->addWidget( fixedSectionTitle );
	fixedSectionLayout->addLayout( m_fixedCardGrid );
	pickerLayout->addWidget( fixedSection );

	m_removableSection = new QWidget( pickerPage );
	QVBoxLayout *removableSectionLayout = new QVBoxLayout( m_removableSection );
	removableSectionLayout->setContentsMargins( 0, 0, 0, 0 );
	removableSectionLayout->setSpacing( 10 );
	QLabel *removableSectionTitle = new QLabel( QStringLiteral( "外接设备" ), m_removableSection );
	removableSectionTitle->setStyleSheet( QStringLiteral( "font-size: 13px; font-weight: 600; color: #596A70;" ) );
	m_removableCardGrid = new QGridLayout();
	m_removableCardGrid->setSpacing( 14 );
	removableSectionLayout->addWidget( removableSectionTitle );
	removableSectionLayout->addLayout( m_removableCardGrid );
	m_removableSection->setVisible( false );
	pickerLayout->addWidget( m_removableSection );

	QFrame *imageEntryFrame = makeCardFrame( pickerPage );
	QHBoxLayout *imageEntryLayout = new QHBoxLayout( imageEntryFrame );
	imageEntryLayout->setContentsMargins( 16, 14, 16, 14 );
	imageEntryLayout->setSpacing( 14 );
	QLabel *imageEntryIcon = new QLabel( imageEntryFrame );
	imageEntryIcon->setPixmap( IconPainter::imageGlyph( 30 ) );
	QVBoxLayout *imageEntryTextCol = new QVBoxLayout();
	imageEntryTextCol->setSpacing( 1 );
	QLabel *imageEntryTitle = new QLabel( QStringLiteral( "打开磁盘镜像" ), imageEntryFrame );
	imageEntryTitle->setStyleSheet( QStringLiteral( "font-size: 14px; font-weight: 600; color: #202C30;" ) );
	QLabel *imageEntrySubtitle = new QLabel( QStringLiteral( "从已经保存好的磁盘镜像文件里找回数据" ), imageEntryFrame );
	imageEntrySubtitle->setStyleSheet( QStringLiteral( "font-size: 12px; color: #596A70;" ) );
	imageEntryTextCol->addWidget( imageEntryTitle );
	imageEntryTextCol->addWidget( imageEntrySubtitle );
	m_openImageButton = new QPushButton( QStringLiteral( "浏览文件…" ), imageEntryFrame );
	setButtonRole( m_openImageButton, "ghost" );
	imageEntryLayout->addWidget( imageEntryIcon );
	imageEntryLayout->addLayout( imageEntryTextCol, 1 );
	imageEntryLayout->addWidget( m_openImageButton );
	pickerLayout->addWidget( imageEntryFrame );

	QHBoxLayout *bitlockerRow = new QHBoxLayout();
	QLabel *bitlockerNote = new QLabel( QStringLiteral( "磁盘被 BitLocker 加密打不开？" ), pickerPage );
	bitlockerNote->setProperty( "role", "secondary" );
	m_unlockBitLockerButton = new QPushButton( QStringLiteral( "解锁加密磁盘…" ), pickerPage );
	setButtonRole( m_unlockBitLockerButton, "ghost" );
	m_unlockBitLockerButton->setIcon( QIcon( IconPainter::lockGlyph( 14 ) ) );
	bitlockerRow->addWidget( bitlockerNote );
	bitlockerRow->addWidget( m_unlockBitLockerButton );
	bitlockerRow->addStretch( 1 );
	pickerLayout->addLayout( bitlockerRow );

	m_advancedToggleButton = new QPushButton( QStringLiteral( "高级设置 ▾" ), pickerPage );
	setButtonRole( m_advancedToggleButton, "ghost" );
	pickerLayout->addWidget( m_advancedToggleButton, 0, Qt::AlignLeft );

	m_advancedPanel = new QWidget( pickerPage );
	QHBoxLayout *advancedLayout = new QHBoxLayout( m_advancedPanel );
	advancedLayout->setContentsMargins( 0, 0, 0, 0 );
	m_devicePathEdit = new QLineEdit( m_advancedPanel );
	m_devicePathEdit->setPlaceholderText( QStringLiteral( "手动填写设备/卷路径（可选），比如 C 或 \\\\.\\C: 或 \\\\.\\PhysicalDrive0" ) );
	advancedLayout->addWidget( new QLabel( QStringLiteral( "设备路径:" ), m_advancedPanel ) );
	advancedLayout->addWidget( m_devicePathEdit, 1 );
	m_advancedPanel->setVisible( false );
	pickerLayout->addWidget( m_advancedPanel );

	pickerLayout->addStretch( 1 );

	QHBoxLayout *selectionBar = new QHBoxLayout();
	m_selectionSummaryLabel = new QLabel( pickerPage );
	m_selectionSummaryLabel->setProperty( "role", "secondary" );
	m_scanButton = new QPushButton( QStringLiteral( "开始扫描" ), pickerPage );
	setButtonRole( m_scanButton, "primary" );
	m_scanButton->setEnabled( false );
	selectionBar->addWidget( m_selectionSummaryLabel, 1 );
	selectionBar->addWidget( m_scanButton );
	pickerLayout->addLayout( selectionBar );

	m_homeStack->addWidget( pickerPage );

	/* ---- results子页 ---- */
	QWidget *resultsPage = new QWidget( m_homeStack );
	m_homeStack_results = resultsPage;
	QVBoxLayout *resultsLayout = new QVBoxLayout( resultsPage );
	resultsLayout->setContentsMargins( 32, 28, 32, 28 );
	resultsLayout->setSpacing( 16 );

	QHBoxLayout *breadcrumbRow = new QHBoxLayout();
	m_resultsBreadcrumbLabel = new QLabel( resultsPage );
	m_resultsBreadcrumbLabel->setProperty( "role", "secondary" );
	QPushButton *changeSourceButton = new QPushButton( QStringLiteral( "更换来源" ), resultsPage );
	setButtonRole( changeSourceButton, "ghost" );
	breadcrumbRow->addWidget( m_resultsBreadcrumbLabel );
	breadcrumbRow->addWidget( changeSourceButton );
	breadcrumbRow->addStretch( 1 );
	resultsLayout->addLayout( breadcrumbRow );

	QHBoxLayout *progressRow = new QHBoxLayout();
	progressRow->setSpacing( 12 );
	m_progressBar = new QProgressBar( resultsPage );
	m_progressBar->setTextVisible( false );
	m_statusLabel = new QLabel( QStringLiteral( "就绪" ), resultsPage );
	m_statusLabel->setProperty( "role", "secondary" );
	progressRow->addWidget( m_progressBar, 1 );
	progressRow->addWidget( m_statusLabel );
	/* 2026-11-07（世杰真机反馈：进度不够明确）：忙碌期间额外显示"已用时"，
	 * 不是真实字节进度，但能持续证明"确实在推进"，照抄CarvePage已验证
	 * 过的做法。默认隐藏，setBusy(true)时才显示。 */
	m_elapsedLabel = new QLabel( resultsPage );
	m_elapsedLabel->setProperty( "role", "secondary" );
	m_elapsedLabel->setVisible( false );
	progressRow->addWidget( m_elapsedLabel );
	resultsLayout->addLayout( progressRow );

	m_elapsedTimer = new QTimer( this );
	m_elapsedTimer->setInterval( 1000 );
	connect( m_elapsedTimer, &QTimer::timeout, this, &MainWindow::onElapsedTimerTick );

	QFrame *noticeFrame = new QFrame( resultsPage );
	noticeFrame->setProperty( "notice", true );
	QHBoxLayout *noticeLayout = new QHBoxLayout( noticeFrame );
	noticeLayout->setContentsMargins( 12, 8, 12, 8 );
	QLabel *noticeLabel = new QLabel( QStringLiteral( "找到记录不代表文件内容完整，原数据可能已被覆盖。" ), noticeFrame );
	noticeLabel->setProperty( "role", "secondary" );
	noticeLayout->addWidget( noticeLabel );
	resultsLayout->addWidget( noticeFrame );

	m_filterEdit = new QLineEdit( resultsPage );
	m_filterEdit->setPlaceholderText( QStringLiteral( "搜索文件名" ) );
	m_filterEdit->setClearButtonEnabled( true );
	resultsLayout->addWidget( m_filterEdit );

	QSplitter *splitter = new QSplitter( Qt::Horizontal, resultsPage );

	QFrame *tableCard = makeCardFrame( splitter );
	QVBoxLayout *tableCardLayout = new QVBoxLayout( tableCard );
	tableCardLayout->setContentsMargins( 0, 0, 0, 0 );
	m_resultsStack = new QStackedWidget( tableCard );

	m_emptyStateLabel = new QLabel( QStringLiteral( "选择上方的恢复来源，然后点击“开始扫描”" ), tableCard );
	m_emptyStateLabel->setAlignment( Qt::AlignCenter );
	m_emptyStateLabel->setWordWrap( true );
	m_emptyStateLabel->setProperty( "role", "placeholder" );

	m_table = new QTableWidget( tableCard );
	m_table->setColumnCount( 4 );
	/* 2026-11-06：列0标题前缀几个全角空格，给嵌进表头的"全选"勾选框
	 * 腾出视觉空间——勾选框是悬浮在表头视口上的独立子widget，不会自动
	 * 挤开标题文字默认的左侧内边距，不留出空当两者会叠在一起 */
	m_table->setHorizontalHeaderLabels(
	     { QStringLiteral( "　　名称" ), QStringLiteral( "原所在目录" ), QStringLiteral( "大小" ), QStringLiteral( "文件系统" ) } );
	m_table->horizontalHeader()->setStretchLastSection( false );
	m_table->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::Stretch );
	m_table->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::Stretch );
	m_table->setSelectionBehavior( QAbstractItemView::SelectRows );
	/* H1（批量恢复，2026-11-06小树裁决）：从单选改成扩展多选——Ctrl/Shift
	 * 多选，和Windows列表控件的既有习惯一致。选中1行时onRecoverClicked()
	 * 走原有的单文件恢复逻辑（零回归），选中多行才走批量恢复。 */
	m_table->setSelectionMode( QAbstractItemView::ExtendedSelection );
	m_table->setEditTriggers( QAbstractItemView::NoEditTriggers );
	m_table->setSortingEnabled( true );
	m_table->setShowGrid( false );
	m_table->verticalHeader()->setVisible( false );
	m_table->setAlternatingRowColors( false );
	m_table->verticalHeader()->setDefaultSectionSize( 36 );

	/* 2026-11-06（世杰追加要求：全选挪到表格标题"名称"旁边）：嵌一个
	 * 勾选框到表头视口上，parent是header本身（不是普通布局子项）——
	 * QHeaderView原生不支持在某个section里放真正可交互的控件，这是
	 * Qt里这类需求的标准做法：悬浮一个独立widget、手动跟着列宽变化
	 * 重新摆放位置。三态（全选/全不选/部分选中）只用来被动显示，不
	 * 让用户点出"部分选中"这个中间态——点击时统一交给onSelectAllClicked()
	 * той同一套"有没有可见行没勾上"的判断逻辑，和原来按钮的行为完全
	 * 一致，只是触发方式从按钮点击变成表头勾选框点击。 */
	m_selectAllHeaderCheckBox = new QCheckBox( m_table->horizontalHeader() );
	m_selectAllHeaderCheckBox->setTristate( true );
	m_selectAllHeaderCheckBox->setToolTip( QStringLiteral( "全选" ) );
	m_selectAllHeaderCheckBox->resize( m_selectAllHeaderCheckBox->sizeHint() );
	connect( m_table->horizontalHeader(), &QHeaderView::sectionResized, this, &MainWindow::repositionSelectAllHeaderCheckBox );
	connect( m_selectAllHeaderCheckBox, &QCheckBox::clicked, this, &MainWindow::onSelectAllClicked );

	m_resultsStack->addWidget( m_emptyStateLabel );
	m_resultsStack->addWidget( m_table );
	m_resultsStack->setCurrentWidget( m_emptyStateLabel );
	tableCardLayout->addWidget( m_resultsStack );
	splitter->addWidget( tableCard );

	QFrame *previewCard = makeCardFrame( splitter );
	QVBoxLayout *previewLayout = new QVBoxLayout( previewCard );
	previewLayout->setContentsMargins( 20, 16, 20, 16 );
	m_previewLabel = new QLabel( previewCard );
	m_previewLabel->setAlignment( Qt::AlignCenter );
	m_previewLabel->setWordWrap( true );
	m_previewLabel->setText( QStringLiteral( "选择一个文件查看预览" ) );
	m_previewLabel->setProperty( "role", "placeholder" );
	m_previewLabel->setMinimumWidth( 220 );
	previewLayout->addWidget( m_previewLabel, 1 );
	splitter->addWidget( previewCard );

	splitter->setStretchFactor( 0, 3 );
	splitter->setStretchFactor( 1, 1 );
	resultsLayout->addWidget( splitter, 1 );

	QHBoxLayout *footerRow = new QHBoxLayout();
	m_checkedSummaryLabel = new QLabel( resultsPage );
	m_checkedSummaryLabel->setProperty( "role", "secondary" );
	m_checkedSummaryLabel->setVisible( false );
	footerRow->addWidget( m_checkedSummaryLabel );
	footerRow->addStretch( 1 );
	m_recoverButton = new QPushButton( QStringLiteral( "恢复文件到…" ), resultsPage );
	setButtonRole( m_recoverButton, "primary" );
	footerRow->addWidget( m_recoverButton );
	resultsLayout->addLayout( footerRow );

	m_homeStack->addWidget( resultsPage );
	m_homeStack->setCurrentWidget( pickerPage );

	m_contentStack->addWidget( homePage );

	/* ---- 页面1：历史版本 ---- */
	m_vssPage = new VssPage( m_contentStack );
	m_contentStack->addWidget( m_vssPage );

	/* ---- 页面2：深度查找 ---- */
	m_carvePage = new CarvePage( m_contentStack );
	m_contentStack->addWidget( m_carvePage );

	m_contentStack->setCurrentWidget( homePage );

	/* 2026-11-06（世杰真机反馈：加了homePage内部的QScrollArea之后，
	 * 窗口仍然拖不动高度——用offscreen QPA平台构造真实MainWindow实测
	 * minimumSizeHint才找到真正的根因）：QStackedWidget的
	 * minimumSizeHint/sizeHint是"所有子页面里最大的那个"，不是只看
	 * 当前显示的那一页——哪怕正显示着已经包了QScrollArea、
	 * minimumSizeHint只有34x56的homePage，只要m_contentStack里还有
	 * VssPage（实测minimumSizeHint高度415px）、CarvePage（实测394px）
	 * 这两个没包QScrollArea的页面，窗口整体的最小高度就会被这两个
	 * "当前根本没在显示"的页面顶住，用户不管切到哪一页都拖不小于
	 * 这个高度。这是QStackedWidget一个广为人知的陷阱，修法是把
	 * m_contentStack整个包进一层QScrollArea（不是分别给VssPage/
	 * CarvePage各包一层——包外层这一层，任何以后新增的页面都自动受益，
	 * 不需要每加一个新页面都重新踩一次这个坑）。homePage内部那层
	 * QScrollArea继续保留，两层不冲突：外层解决"切到别的页面时别的
	 * 页面的尺寸还在顶着窗口"，内层单独处理"磁盘卡片很多时home页面
	 * 自己的内容需要能滚动"。 */
	QScrollArea *contentScrollArea = new QScrollArea( central );
	contentScrollArea->setWidgetResizable( true );
	contentScrollArea->setFrameShape( QFrame::NoFrame );
	contentScrollArea->setWidget( m_contentStack );
	rootLayout->addWidget( contentScrollArea, 1 );

	setCentralWidget( central );

	m_worker = new ScanWorker();
	m_worker->moveToThread( &m_workerThread );
	connect( &m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater );
	connect( this, &MainWindow::requestScan, m_worker, &ScanWorker::scan );
	connect( this, &MainWindow::requestScanBitLocker, m_worker, &ScanWorker::scanBitLocker );
	connect( this, &MainWindow::requestRecover, m_worker, &ScanWorker::recover );
	connect( this, &MainWindow::requestRecoverBatch, m_worker, &ScanWorker::recoverBatch );
	connect( this, &MainWindow::requestThumbnail, m_worker, &ScanWorker::generateThumbnail );
	connect( m_worker, &ScanWorker::progress, this, &MainWindow::onProgress );
	connect( m_worker, &ScanWorker::scanFinished, this, &MainWindow::onScanFinished );
	connect( m_worker, &ScanWorker::recoverFinished, this, &MainWindow::onRecoverFinished );
	connect( m_worker, &ScanWorker::recoverBatchItemFinished, this, &MainWindow::onRecoverBatchItemFinished );
	connect( m_worker, &ScanWorker::recoverBatchFinished, this, &MainWindow::onRecoverBatchFinished );
	connect( m_worker, &ScanWorker::thumbnailReady, this, &MainWindow::onThumbnailReady );
	m_workerThread.start();

	connect( m_navDisksButton, &QPushButton::clicked, this, [ this ]() { m_contentStack->setCurrentIndex( 0 ); } );
	connect( m_navHistoryButton, &QPushButton::clicked, this, [ this ]() { m_contentStack->setCurrentIndex( 1 ); } );
	connect( m_navDeepSearchButton, &QPushButton::clicked, this, [ this ]() { m_contentStack->setCurrentIndex( 2 ); } );
	connect( m_navAboutButton, &QPushButton::clicked, this, &MainWindow::onAboutClicked );

	connect( m_refreshButton, &QPushButton::clicked, this, &MainWindow::onRefreshDisksClicked );
	connect( m_scanButton, &QPushButton::clicked, this, &MainWindow::onScanClicked );
	connect( m_openImageButton, &QPushButton::clicked, this, &MainWindow::onOpenImageClicked );
	connect( m_unlockBitLockerButton, &QPushButton::clicked, this, &MainWindow::onUnlockBitLockerClicked );
	connect( m_advancedToggleButton, &QPushButton::clicked, this, &MainWindow::onAdvancedToggled );
	connect( changeSourceButton, &QPushButton::clicked, this, &MainWindow::onChangeSourceClicked );
	connect( m_recoverButton, &QPushButton::clicked, this, &MainWindow::onRecoverClicked );
	connect( m_filterEdit, &QLineEdit::textChanged, this, &MainWindow::onFilterTextChanged );
	connect( m_table, &QTableWidget::itemSelectionChanged, this, &MainWindow::onTableSelectionChanged );
	connect( m_table, &QTableWidget::itemChanged, this, &MainWindow::onTableItemChanged );

	connect( m_vssPage, &VssPage::scanRequested, this, &MainWindow::onVssScanRequested );
	connect( m_carvePage, &CarvePage::runningChanged, this, &MainWindow::onCarveRunningChanged );

	rebuildDeviceCards();

	/* 表头勾选框的初始位置——构造函数这个时刻列宽还没有真正按stretch
	 * 模式算出最终值（要等第一次真正显示/布局才会算），之后列宽变化
	 * 会触发sectionResized自动跟着重新摆放，但首次定位等不到那个信号，
	 * 排到事件循环处理完第一轮布局之后再摆一次位置 */
	QTimer::singleShot( 0, this, &MainWindow::repositionSelectAllHeaderCheckBox );
}

MainWindow::~MainWindow()
{
	m_workerThread.quit();
	m_workerThread.wait();
}

void MainWindow::closeEvent( QCloseEvent *event )
{
	if( m_carvePage->isRunning() )
	{
		QMessageBox::information( this, QStringLiteral( "提示" ),
		     QStringLiteral( "深度查找正在进行中，请先切到“深度查找”页面点击“停止”，再关闭程序。" ) );
		event->ignore();
		return;
	}
	event->accept();
}

void MainWindow::rebuildDeviceCards()
{
	for( DeviceCard *card : m_deviceCards )
	{
		delete card;
	}
	m_deviceCards.clear();
	m_selectedCard = nullptr;

	disk_enum_result_t result;
	memset( &result, 0, sizeof( result ) );

	int fixedCount = 0;
	int removableCount = 0;

	if( disk_enumerator_list_logical_drives( &result ) == 1 )
	{
		for( int i = 0; i < result.count; i++ )
		{
			const disk_entry_t &entry = result.entries[ i ];

			/* 2026-11-06（世杰反馈后发现）：disk_enumerator把"Fixed"以外
			 * 的全部类型（Removable/Network/CD-ROM/RAM/Unknown）都当成
			 * "外接设备"塞进卡片列表——网络映射盘、虚拟光驱不是这个工具
			 * 要处理的恢复来源（网络盘不是本机块设备，直接按设备路径读
			 * 根本读不通；光驱是只读介质，没有"已删除文件"这个概念），
			 * 混进列表里只会让用户困惑、也会在开发机上把Parallels共享
			 * 文件夹的网络映射盘（真实开发环境里就撞上过）当成一张磁盘
			 * 卡片画出来。只保留Fixed/Removable两类真正可能需要恢复的
			 * 本地存储。 */
			bool isNetworkOrOptical = ( strcmp( entry.drive_type_name, "Network" ) == 0 )
			     || ( strcmp( entry.drive_type_name, "CD-ROM" ) == 0 )
			     || ( strcmp( entry.drive_type_name, "RAM" ) == 0 )
			     || ( strcmp( entry.drive_type_name, "Unknown" ) == 0 );
			if( isNetworkOrOptical )
			{
				continue;
			}
			bool removable = ( strcmp( entry.drive_type_name, "Fixed" ) != 0 );

			QString driveLetterString( QChar( entry.drive_letter ) );
			QString volumeLabel = QString::fromUtf8( entry.volume_label );
			QString title = volumeLabel.isEmpty()
			     ? QStringLiteral( "本地磁盘（%1:）" ).arg( driveLetterString )
			     : QStringLiteral( "%1（%2:）" ).arg( volumeLabel, driveLetterString );
			QString fsLabel = QString::fromUtf8( entry.file_system );

			DeviceCard *card = new DeviceCard( removable ? m_removableSection : m_homeStack_picker );
			card->setDiskInfo( title, removable, fsLabel, entry.total_bytes, entry.free_bytes );
			card->setDevicePath( resolveDevicePath( driveLetterString ) );

			/* I4（BitLocker自动识别，2026-11-06小树裁决）：查询这个盘符
			 * 是否被BitLocker锁定——同步调用（PowerShell子进程查WMI，
			 * 单次几百毫秒级），和VssWizardDialog::onQueryOnlineClicked()
			 * 同样的既有先例（一次性诊断性查询，不是高频操作，不需要为
			 * 这个新开一套异步基础设施）。只在"确认已锁定+确认单一物理
			 * 区间"这两个条件都满足时才画锁形角标——任何一个条件不满足
			 * 都优雅回退成普通卡片，用户仍然可以走"解锁加密磁盘…"这个
			 * 独立手动入口（见I0/I1/I2头注释，这两个查询真机验证过
			 * 在锁定态下依然可靠）。 */
			bitlocker_status_t bitlockerStatus;
			if( bitlocker_status_checker_query( driveLetterString.at( 0 ).toLatin1(), &bitlockerStatus ) == 1
			 && bitlockerStatus.is_fve_manageable && bitlockerStatus.is_locked )
			{
				volume_disk_extents_t extents;
				if( volume_disk_extents_query( driveLetterString.at( 0 ).toLatin1(), &extents ) == 1 && extents.is_single_extent )
				{
					QString physicalDrivePath = QStringLiteral( "\\\\.\\PhysicalDrive%1" ).arg( extents.disk_number );
					card->setLocked( physicalDrivePath, extents.starting_offset );
				}
			}

			connect( card, &DeviceCard::cardClicked, this, [ this, card ]() { onDeviceCardClicked( card ); } );

			QGridLayout *targetGrid = removable ? m_removableCardGrid : m_fixedCardGrid;
			int &counter = removable ? removableCount : fixedCount;
			targetGrid->addWidget( card, counter / kDeviceCardColumns, counter % kDeviceCardColumns );
			counter++;

			m_deviceCards.append( card );
		}
	}
	disk_enum_result_free( &result );

	m_removableSection->setVisible( removableCount > 0 );
	updateSelectionBar();
}

void MainWindow::onRefreshDisksClicked()
{
	rebuildDeviceCards();
}

void MainWindow::onDeviceCardClicked( DeviceCard *card )
{
	for( DeviceCard *other : m_deviceCards )
	{
		other->setSelected( other == card );
	}
	m_selectedCard = card;
	updateSelectionBar();
}

void MainWindow::updateSelectionBar()
{
	if( m_selectedCard == nullptr )
	{
		m_selectionSummaryLabel->setText( QStringLiteral( "请选择一个磁盘，或者打开一个磁盘镜像文件" ) );
		m_scanButton->setText( QStringLiteral( "开始扫描" ) );
		m_scanButton->setEnabled( false );
	}
	else
	{
		QString summary = m_selectedCard->summaryText();
		QString suffix = m_selectedCard->isLocked() ? QStringLiteral( " · 已加密，需先解锁" ) : QString();
		m_selectionSummaryLabel->setText(
		     ( summary.isEmpty() ? QStringLiteral( "已选择：%1" ).arg( m_selectedCard->title() )
		                        : QStringLiteral( "已选择：%1 · %2" ).arg( m_selectedCard->title(), summary ) )
		     + suffix );
		m_scanButton->setText( m_selectedCard->isLocked() ? QStringLiteral( "解锁并扫描" ) : QStringLiteral( "开始扫描" ) );
		m_scanButton->setEnabled( true );
	}
}

void MainWindow::onAdvancedToggled()
{
	bool nowVisible = !m_advancedPanel->isVisible();
	m_advancedPanel->setVisible( nowVisible );
	m_advancedToggleButton->setText( nowVisible ? QStringLiteral( "高级设置 ▴" ) : QStringLiteral( "高级设置 ▾" ) );
}

void MainWindow::onScanClicked()
{
	QString manualText = m_devicePathEdit->text().trimmed();

	/* I4（BitLocker自动识别，2026-11-06小树裁决）：选中的是一张确认
	 * "已锁定"的卡片（手动设备路径覆盖时不走这条分支——手动填路径说明
	 * 用户明确想绕开卡片自动识别的结果，交给普通扫描逻辑去尝试，失败
	 * 了也有清楚的错误提示） */
	if( manualText.isEmpty() && m_selectedCard != nullptr && m_selectedCard->isLocked() )
	{
		BitLockerWizardDialog dialog( this );
		dialog.setInitialDevicePath( m_selectedCard->physicalDrivePath() );
		dialog.setInitialOffset( m_selectedCard->physicalDriveOffset() );
		if( dialog.exec() != QDialog::Accepted )
		{
			return;
		}
		scanBitLockerWithCredential( dialog.selectedSource(), dialog.credentialInput(), m_selectedCard->title() );
		switchToResultsPage( m_selectedCard->title() );
		return;
	}

	QString devicePath;
	QString title;

	if( !manualText.isEmpty() )
	{
		devicePath = resolveDevicePath( manualText );
		title = manualText;
	}
	else if( m_selectedCard != nullptr )
	{
		devicePath = m_selectedCard->devicePath();
		title = m_selectedCard->title();
	}

	if( devicePath.isEmpty() )
	{
		QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先选择一个磁盘，或在高级设置里填写设备路径" ) );
		return;
	}

	ScanSource source;
	source.type = ScanSourceType::DirectDevice;
	source.devicePath = devicePath;
	scanWithSource( source, title );
	switchToResultsPage( title );
}

void MainWindow::onOpenImageClicked()
{
	QString path = QFileDialog::getOpenFileName( this, QStringLiteral( "选择磁盘镜像文件" ) );
	if( path.isEmpty() )
	{
		return;
	}

	ScanSource source;
	source.type = ScanSourceType::DirectDevice;
	source.devicePath = path;
	QString title = QFileInfo( path ).fileName();
	scanWithSource( source, title );
	switchToResultsPage( title );
}

void MainWindow::onUnlockBitLockerClicked()
{
	BitLockerWizardDialog dialog( this );
	if( dialog.exec() != QDialog::Accepted )
	{
		return;
	}
	scanBitLockerWithCredential( dialog.selectedSource(), dialog.credentialInput(), QStringLiteral( "加密磁盘" ) );
	switchToResultsPage( QStringLiteral( "加密磁盘" ) );
}

void MainWindow::onVssScanRequested( ScanSource source )
{
	scanWithSource( source, QStringLiteral( "历史版本" ) );
	switchToResultsPage( QStringLiteral( "历史版本" ) );
}

void MainWindow::onChangeSourceClicked()
{
	switchToPickerPage();
}

void MainWindow::switchToResultsPage( const QString &sourceTitle )
{
	m_resultsBreadcrumbLabel->setText( QStringLiteral( "磁盘与镜像 / %1" ).arg( sourceTitle ) );
	m_homeStack->setCurrentWidget( m_homeStack_results );
	m_contentStack->setCurrentIndex( 0 );
	m_navDisksButton->setChecked( true );
}

void MainWindow::switchToPickerPage()
{
	m_homeStack->setCurrentWidget( m_homeStack_picker );
}

void MainWindow::onCarveRunningChanged( bool running )
{
	m_sidebarTaskLabel->setVisible( running );
}

void MainWindow::onAboutClicked()
{
	/* 2026-11-17 打包发布前合规补充：third_party/THIRD_PARTY_LICENSES.txt
	 * 一直明确记着"Qt的LGPL-3.0'告知用户使用了LGPL组件'这条义务、
	 * FreeType的强制信用声明，都要体现在产品'关于'页面这类用户能看到
	 * 的地方，不能只塞进一个没人会打开的txt文件里"——这次是第一次真
	 * 正对外分发（绿色版+安装包都会装到真实用户机器上），这条待办到
	 * 了必须兑现的时候，不是顺手加的装饰性文案。 */
	QMessageBox::about( this, QStringLiteral( "关于 DataRecoveryStudio" ),
	     QStringLiteral(
	         "DataRecoveryStudio\n版本 V0.2\n\n"
	         "一个磁盘数据恢复工具：直接扫描、历史版本找回、"
	         "BitLocker 加密磁盘解锁、深度查找（PhotoRec 文件雕刻兜底）。\n\n"
	         "本软件直接访问物理磁盘/卷影副本/加密磁盘，需要以管理员身份运行，"
	         "双击启动时会自动弹出 UAC 提权确认。\n\n"
	         "本软件使用了 Qt 6（LGPL-3.0，以动态库形式链接，未修改 Qt 源码，"
	         "用户可自行替换安装目录内的 Qt 动态库）、libfsntfs/libvshadow/libbde/"
	         "libvsmbr/libvsgpt（LGPL-3.0）等第三方开源组件，并以独立子进程方式"
	         "调用 PhotoRec/TestDisk（GPL-2.0，未拷贝其源码，仅调用独立编译的可"
	         "执行文件）。\n\n"
	         "Portions of this software are copyright (C) The FreeType Project "
	         "(www.freetype.org). All rights reserved.\n\n"
	         "完整的第三方组件清单与许可证全文，见安装目录下的 "
	         "THIRD_PARTY_LICENSES.txt。" ) );
}

void MainWindow::scanWithSource( const ScanSource &source, const QString &sourceTitle )
{
	Q_UNUSED( sourceTitle );
	m_currentScanSource = source;
	m_candidates.clear();
	m_table->setRowCount( 0 );
	updateCheckedSummary();
	m_thumbnailCache.clear();
	m_previewLabel->setPixmap( QPixmap() );
	m_previewLabel->setText( QStringLiteral( "选择一个文件查看预览" ) );
	setBusy( true );
	m_statusLabel->setText( QStringLiteral( "正在扫描…" ) );
	m_progressBar->setRange( 0, 0 );
	m_emptyStateLabel->setText( QStringLiteral( "正在扫描，请稍候…" ) );
	m_resultsStack->setCurrentWidget( m_emptyStateLabel );

	emit requestScan( source );
}

void MainWindow::scanBitLockerWithCredential( const ScanSource &source, const BitLockerCredentialInput &credential, const QString &sourceTitle )
{
	Q_UNUSED( sourceTitle );
	m_currentScanSource = source;
	m_candidates.clear();
	m_table->setRowCount( 0 );
	updateCheckedSummary();
	m_thumbnailCache.clear();
	m_previewLabel->setPixmap( QPixmap() );
	m_previewLabel->setText( QStringLiteral( "选择一个文件查看预览" ) );
	setBusy( true );
	m_statusLabel->setText( QStringLiteral( "正在解锁并扫描…" ) );
	m_progressBar->setRange( 0, 0 );
	m_emptyStateLabel->setText( QStringLiteral( "正在解锁并扫描，请稍候…" ) );
	m_resultsStack->setCurrentWidget( m_emptyStateLabel );

	emit requestScanBitLocker( source, credential );
}

void MainWindow::onRecoverClicked()
{
	/* H1追加（世杰真机反馈：没有全选，只能一个个点）：恢复目标现在以
	 * 名称格的勾选框状态为准，不再看表格行选中状态（行选中/高亮是给
	 * "点一行看预览"这个独立用途的，和"要恢复哪些文件"这件事解耦——
	 * 点一行只是看看这是什么文件，不代表用户想恢复它）。一个都没勾
	 * 时退回到"点哪行就恢复哪行"这个最简单直接的单文件操作方式，不
	 * 强制用户每次都要先勾选才能恢复单个文件。 */
	QVector<CandidateItem> targets;
	for( int row = 0; row < m_table->rowCount(); row++ )
	{
		QTableWidgetItem *nameItem = m_table->item( row, 0 );
		if( nameItem != nullptr && nameItem->checkState() == Qt::Checked )
		{
			targets.append( nameItem->data( Qt::UserRole ).value<CandidateItem>() );
		}
	}

	if( targets.isEmpty() )
	{
		int row = m_table->currentRow();
		if( row < 0 )
		{
			QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先勾选一个或多个候选项，或者点一行要恢复的文件" ) );
			return;
		}
		targets.append( m_table->item( row, 0 )->data( Qt::UserRole ).value<CandidateItem>() );
	}

	if( targets.count() == 1 )
	{
		const CandidateItem &item = targets.first();
		QString suggestedName = item.name.isEmpty() ? QStringLiteral( "recovered_file" ) : item.name;
		QString outputPath = QFileDialog::getSaveFileName( this, QStringLiteral( "恢复到..." ), suggestedName );

		if( outputPath.isEmpty() )
		{
			return;
		}

		setBusy( true );
		/* 2026-11-07（世杰真机反馈：恢复大文件时进度条"直接就满了，一直
		 * 保持满的状态"）：这里原来漏了设置进度条——单文件恢复走的是
		 * 这个独立分支，不会经过onProgress()，如果不在这里主动重置，
		 * 进度条会停留在上一次操作（通常是扫描完成）留下的满值
		 * （onScanFinished()会setRange(0,1)+setValue(1)），看起来就像
		 * "进度条显示100%但状态写着恢复中"这种自相矛盾的样子。批量恢复
		 * 分支（下面）和扫描开始时都已经在做这个reset，单文件恢复漏了，
		 * 不是设计上刻意的差异。 */
		m_progressBar->setRange( 0, 0 );
		m_statusLabel->setText( QStringLiteral( "恢复中..." ) );
		emit requestRecover( m_currentScanSource, item, outputPath );
		return;
	}

	QString outputDir = QFileDialog::getExistingDirectory( this, QStringLiteral( "恢复所选文件到…" ) );
	if( outputDir.isEmpty() )
	{
		return;
	}

	m_batchTotal = targets.count();
	setBusy( true );
	m_progressBar->setRange( 0, 0 );
	m_statusLabel->setText( QStringLiteral( "正在恢复 0 / %1…" ).arg( m_batchTotal ) );

	emit requestRecoverBatch( m_currentScanSource, targets, outputDir );
}

void MainWindow::onRecoverBatchItemFinished( int index, bool success, const QString &errorMessage, const QString &outputPath, int badSectorCount, quint64 badSectorBytes )
{
	Q_UNUSED( success );
	Q_UNUSED( errorMessage );
	Q_UNUSED( outputPath );
	Q_UNUSED( badSectorCount );
	Q_UNUSED( badSectorBytes );
	m_statusLabel->setText( QStringLiteral( "正在恢复 %1 / %2…" ).arg( index + 1 ).arg( m_batchTotal ) );
}

void MainWindow::onRecoverBatchFinished( int successCount, int totalCount, const QStringList &failedNames, int totalBadSectorCount, quint64 totalBadSectorBytes )
{
	setBusy( false );
	m_progressBar->setRange( 0, 1 );
	m_progressBar->setValue( 1 );

	/* 2026-11-06（小树裁决：坏道计数上报补齐）：`success==true`只代表
	 * "恢复流程走完了"，不代表文件内容完整——`ScanWorker`里几条恢复
	 * 路径早就在追踪"读取失败重试耗尽后0填充跳过"这件事，只是一直没有
	 * 传出来给用户看。这里不区分是哪几个文件受影响（想知道具体是哪个
	 * 文件，世杰需要自己打开确认；这个提示只负责回答"要不要怀疑是
	 * bug"——有坏道就不是bug，是已知的数据覆盖/介质问题） */
	QString badSectorSuffix;
	if( totalBadSectorCount > 0 )
	{
		badSectorSuffix = QStringLiteral( "\n\n这批恢复里共有 %1 处数据读取失败已跳过（共 %2），受影响的文件内容可能不完整——这通常意味着原始数据已经被覆盖或者存储介质本身有问题，不是软件的bug。" )
		     .arg( totalBadSectorCount ).arg( formatFileSize( totalBadSectorBytes ) );
	}

	if( failedNames.isEmpty() )
	{
		m_statusLabel->setText( totalBadSectorCount > 0 ? QStringLiteral( "恢复完成（有数据读取失败）" ) : QStringLiteral( "恢复完成" ) );
		QMessageBox::information( this, QStringLiteral( "完成" ),
		     QStringLiteral( "已恢复 %1 个文件到你选择的目录。" ).arg( successCount ) + badSectorSuffix );
	}
	else
	{
		m_statusLabel->setText( QStringLiteral( "部分恢复失败" ) );
		QMessageBox::warning( this, QStringLiteral( "部分完成" ),
		     QStringLiteral( "成功恢复 %1 / %2 个文件，以下文件恢复失败：\n%3" )
		          .arg( successCount ).arg( totalCount ).arg( failedNames.join( QStringLiteral( "\n" ) ) ) + badSectorSuffix );
	}
}

void MainWindow::onProgress( quint64 current, quint64 total )
{
	if( total == 0 )
	{
		m_progressBar->setRange( 0, 0 );
		m_statusLabel->setText( QStringLiteral( "已处理 %1 项" ).arg( current ) );
	}
	else
	{
		m_progressBar->setRange( 0, (int) total );
		m_progressBar->setValue( (int) current );
		m_statusLabel->setText( QStringLiteral( "%1 / %2" ).arg( current ).arg( total ) );
	}
}

void MainWindow::onScanFinished( bool success, const QString &errorMessage, const QVector<CandidateItem> &candidates )
{
	setBusy( false );
	m_progressBar->setRange( 0, 1 );
	m_progressBar->setValue( success ? 1 : 0 );

	if( !success )
	{
		m_statusLabel->setText( QStringLiteral( "扫描失败" ) );
		m_emptyStateLabel->setText( QStringLiteral( "扫描未能完成，可以重新选择来源再试一次" ) );
		m_resultsStack->setCurrentWidget( m_emptyStateLabel );
		QMessageBox::critical( this, QStringLiteral( "扫描失败" ), errorMessage );
		return;
	}

	m_candidates = candidates;
	populateTable( candidates );
	m_statusLabel->setText(
	     candidates.isEmpty() ? QStringLiteral( "未找到可恢复的文件" ) : QStringLiteral( "找到 %1 个可恢复的文件" ).arg( candidates.count() ) );
}

void MainWindow::onRecoverFinished( bool success, const QString &errorMessage, int badSectorCount, quint64 badSectorBytes )
{
	setBusy( false );
	m_progressBar->setRange( 0, 1 );
	m_progressBar->setValue( success ? 1 : 0 );

	if( success )
	{
		/* 2026-11-06（小树裁决：坏道计数上报补齐，世杰真机反馈部分恢复出
		 * 的图片打不开/是黑的——排查确认引擎早就在追踪这个信息，只是
		 * 没有传到GUI这一层）：`success==true`只代表恢复流程本身走完了
		 * （定位到文件、写出了和原始大小一致的内容），不代表内容完整——
		 * 有坏道时额外提示，帮用户当场判断"这是不是bug"，不需要先打开
		 * 文件发现损坏了才回来问 */
		if( badSectorCount > 0 )
		{
			m_statusLabel->setText( QStringLiteral( "恢复完成（有数据读取失败）" ) );
			QMessageBox::information( this, QStringLiteral( "完成" ),
			     QStringLiteral( "文件已恢复到你选择的位置，但恢复过程中有 %1 处数据读取失败已跳过（共 %2），"
			                      "文件内容可能不完整——这通常意味着原始数据已经被覆盖或者存储介质本身有问题，不是软件的bug。" )
			          .arg( badSectorCount ).arg( formatFileSize( badSectorBytes ) ) );
		}
		else
		{
			m_statusLabel->setText( QStringLiteral( "恢复完成" ) );
			QMessageBox::information( this, QStringLiteral( "完成" ), QStringLiteral( "文件已恢复到你选择的位置。" ) );
		}
	}
	else
	{
		m_statusLabel->setText( QStringLiteral( "恢复失败" ) );
		QMessageBox::critical( this, QStringLiteral( "恢复失败" ), errorMessage );
	}
}

void MainWindow::setBusy( bool busy )
{
	m_busy = busy;
	m_scanButton->setEnabled( !busy && m_selectedCard != nullptr );
	m_openImageButton->setEnabled( !busy );
	m_unlockBitLockerButton->setEnabled( !busy );
	m_recoverButton->setEnabled( !busy );
	m_refreshButton->setEnabled( !busy );

	if( busy )
	{
		m_stopwatch.start();
		m_elapsedLabel->setText( QStringLiteral( "已用时 0:00" ) );
		m_elapsedLabel->setVisible( true );
		m_elapsedTimer->start();
	}
	else
	{
		m_elapsedTimer->stop();
		m_elapsedLabel->setVisible( false );
		/* 2026-11-07：忙碌状态刚结束——如果用户在忙碌期间选中过某一行、
		 * 当时因为m_busy==true被onTableSelectionChanged()拦下没有真正
		 * emit预览请求，这里补一次，不需要用户重新点一下那一行才能看到
		 * 预览。重新走一遍onTableSelectionChanged()，它自己会判断当前
		 * 选中行/缓存状态，不需要额外参数。 */
		onTableSelectionChanged();
	}
}

void MainWindow::onElapsedTimerTick()
{
	qint64 secs = m_stopwatch.elapsed() / 1000;
	m_elapsedLabel->setText( QStringLiteral( "已用时 %1:%2" ).arg( secs / 60, 2, 10, QLatin1Char( '0' ) ).arg( secs % 60, 2, 10, QLatin1Char( '0' ) ) );
}

void MainWindow::populateTable( const QVector<CandidateItem> &candidates )
{
	m_table->setSortingEnabled( false );
	m_table->setRowCount( candidates.count() );

	for( int i = 0; i < candidates.count(); i++ )
	{
		const CandidateItem &item = candidates.at( i );

		QTableWidgetItem *nameItem = new QTableWidgetItem( item.name );
		nameItem->setData( Qt::UserRole, QVariant::fromValue( item ) );
		nameItem->setData( kOriginalIndexRole, i );
		/* H1追加（世杰真机反馈：没有全选，只能一个个点）：名称格本身带
		 * 勾选框，不新增一列——避免改动其它所有硬编码列号引用的地方，
		 * 勾选框视觉上紧贴文件名前面，和HTML设计稿里"名称格=勾选框+
		 * 图标+文件名"的呈现方式是同一个思路 */
		nameItem->setFlags( nameItem->flags() | Qt::ItemIsUserCheckable );
		nameItem->setCheckState( Qt::Unchecked );
		m_table->setItem( i, 0, nameItem );
		m_table->setItem( i, 1, new QTableWidgetItem( item.directoryPath ) );

		NumericSizeItem *sizeItem = new NumericSizeItem(
		     item.hasSize ? formatFileSize( item.size ) : QStringLiteral( "未知" ) );
		sizeItem->setData( Qt::UserRole + 2, item.hasSize ? item.size : 0 );
		sizeItem->setTextAlignment( Qt::AlignRight | Qt::AlignVCenter );
		m_table->setItem( i, 2, sizeItem );

		m_table->setItem( i, 3, new QTableWidgetItem( fsTypeLabel( item.fsType ) ) );
	}
	m_table->setSortingEnabled( true );
	updateResultsEmptyState();
	updateCheckedSummary();
}

void MainWindow::updateResultsEmptyState()
{
	if( m_table->rowCount() == 0 )
	{
		m_emptyStateLabel->setText( QStringLiteral( "未找到可恢复的文件，可以试试“历史版本”或“深度查找”" ) );
		m_resultsStack->setCurrentWidget( m_emptyStateLabel );
	}
	else
	{
		m_resultsStack->setCurrentWidget( m_table );
	}
}

void MainWindow::onFilterTextChanged( const QString &text )
{
	applyFilter();
	Q_UNUSED( text );
}

void MainWindow::applyFilter()
{
	QString needle = m_filterEdit->text();

	for( int row = 0; row < m_table->rowCount(); row++ )
	{
		QTableWidgetItem *nameItem = m_table->item( row, 0 );
		bool matches = needle.isEmpty() || ( nameItem != nullptr && nameItem->text().contains( needle, Qt::CaseInsensitive ) );
		m_table->setRowHidden( row, !matches );
	}
	/* "全选"按钮的文案/可用性取决于"当前筛选后可见的行"，筛选条件一变
	 * 这个判断就要重新算一遍；已勾选的文件数量本身不受筛选影响（筛选
	 * 只是换了一下视图，不应该连带着把用户已经勾好的选择也改掉） */
	updateCheckedSummary();
}

void MainWindow::onTableItemChanged( QTableWidgetItem *item )
{
	Q_UNUSED( item );
	updateCheckedSummary();
}

void MainWindow::repositionSelectAllHeaderCheckBox()
{
	QHeaderView *header = m_table->horizontalHeader();
	int x = header->sectionViewportPosition( 0 ) + 8;
	int y = ( header->height() - m_selectAllHeaderCheckBox->height() ) / 2;
	m_selectAllHeaderCheckBox->move( x, y );
}

void MainWindow::onSelectAllClicked()
{
	bool anyUncheckedVisible = false;
	for( int row = 0; row < m_table->rowCount(); row++ )
	{
		if( m_table->isRowHidden( row ) )
		{
			continue;
		}
		QTableWidgetItem *nameItem = m_table->item( row, 0 );
		if( nameItem != nullptr && nameItem->checkState() != Qt::Checked )
		{
			anyUncheckedVisible = true;
			break;
		}
	}

	/* 还有没勾上的可见行——"全选"；可见行已经全部勾上了——再点一次变成
	 * "取消全选"，这是最常见应用里"全选"按钮的行为约定，不单独加一个
	 * "取消全选"按钮占地方 */
	Qt::CheckState targetState = anyUncheckedVisible ? Qt::Checked : Qt::Unchecked;
	for( int row = 0; row < m_table->rowCount(); row++ )
	{
		if( m_table->isRowHidden( row ) )
		{
			continue;
		}
		QTableWidgetItem *nameItem = m_table->item( row, 0 );
		if( nameItem != nullptr )
		{
			nameItem->setCheckState( targetState );
		}
	}
	updateCheckedSummary();
}

void MainWindow::updateCheckedSummary()
{
	int checkedCount = 0;
	int visibleCount = 0;
	int uncheckedVisibleCount = 0;

	for( int row = 0; row < m_table->rowCount(); row++ )
	{
		QTableWidgetItem *nameItem = m_table->item( row, 0 );
		if( nameItem == nullptr )
		{
			continue;
		}
		bool checked = ( nameItem->checkState() == Qt::Checked );
		if( checked )
		{
			checkedCount++;
		}
		if( !m_table->isRowHidden( row ) )
		{
			visibleCount++;
			if( !checked )
			{
				uncheckedVisibleCount++;
			}
		}
	}

	/* 表头勾选框三态只用来被动展示当前状态，不依赖用户真的能点出"部分
	 * 选中"这个中间态——用户点击一律走onSelectAllClicked()的"有没有
	 * 可见行没勾上"二选一判断，这里只是把判断结果（可能是三种展示
	 * 状态之一）同步画到勾选框上 */
	if( visibleCount == 0 )
	{
		m_selectAllHeaderCheckBox->setCheckState( Qt::Unchecked );
		m_selectAllHeaderCheckBox->setEnabled( false );
	}
	else if( uncheckedVisibleCount == 0 )
	{
		m_selectAllHeaderCheckBox->setCheckState( Qt::Checked );
		m_selectAllHeaderCheckBox->setEnabled( true );
	}
	else if( uncheckedVisibleCount == visibleCount )
	{
		m_selectAllHeaderCheckBox->setCheckState( Qt::Unchecked );
		m_selectAllHeaderCheckBox->setEnabled( true );
	}
	else
	{
		m_selectAllHeaderCheckBox->setCheckState( Qt::PartiallyChecked );
		m_selectAllHeaderCheckBox->setEnabled( true );
	}

	if( checkedCount > 0 )
	{
		m_checkedSummaryLabel->setText( QStringLiteral( "已勾选 %1 个文件" ).arg( checkedCount ) );
		m_checkedSummaryLabel->setVisible( true );
		m_recoverButton->setText( QStringLiteral( "恢复所选的 %1 个文件…" ).arg( checkedCount ) );
	}
	else
	{
		m_checkedSummaryLabel->setVisible( false );
		m_recoverButton->setText( QStringLiteral( "恢复文件到…" ) );
	}
}

void MainWindow::onTableSelectionChanged()
{
	/* H1追加：点一行只负责切换预览，不再联动恢复按钮文案——"要恢复
	 * 哪些文件"现在完全由勾选框状态决定（见updateCheckedSummary()），
	 * 这里只剩预览逻辑，和勾选是两件独立的事 */
	int row = m_table->currentRow();

	if( row < 0 )
	{
		m_previewLabel->setPixmap( QPixmap() );
		m_previewLabel->setText( QStringLiteral( "选择一个文件查看预览" ) );
		return;
	}

	QTableWidgetItem *firstCell = m_table->item( row, 0 );
	CandidateItem item = firstCell->data( Qt::UserRole ).value<CandidateItem>();
	int originalIndex = firstCell->data( kOriginalIndexRole ).toInt();

	if( m_thumbnailCache.contains( originalIndex ) )
	{
		m_previewLabel->setPixmap(
		     m_thumbnailCache.value( originalIndex ).scaled( m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation ) );
		return;
	}

	if( !looksLikeSupportedImage( item.name ) )
	{
		m_previewLabel->setPixmap( QPixmap() );
		m_previewLabel->setText( QStringLiteral( "此类型暂不支持预览，仍可尝试恢复" ) );
		return;
	}

	/* 2026-11-07（世杰真机反馈：恢复大文件时点预览一直显示"正在加载预览"）：
	 * ScanWorker的scan/recover/recoverBatch/generateThumbnail全部排在
	 * 同一个worker线程的队列上（见scan_worker.h头注释），忙碌期间新发出
	 * 的预览请求只会排队等着，不是真的卡死，但用户看不出区别。这里不再
	 * 无条件emit，忙碌时直接提示"进行中、请稍候"，等setBusy(false)时会
	 * 自动重新触发一次（见setBusy()实现），用户不需要自己再点一次。 */
	if( m_busy )
	{
		m_previewLabel->setPixmap( QPixmap() );
		m_previewLabel->setText( QStringLiteral( "扫描/恢复进行中，请稍候再查看预览…" ) );
		return;
	}

	m_previewLabel->setPixmap( QPixmap() );
	m_previewLabel->setText( QStringLiteral( "正在加载预览…" ) );
	emit requestThumbnail( originalIndex, m_currentScanSource, item );
}

void MainWindow::onThumbnailReady( int row, bool success, const QString &reason, const QPixmap &pixmap )
{
	int currentRow = m_table->currentRow();
	if( currentRow < 0 )
	{
		return;
	}
	QTableWidgetItem *firstCell = m_table->item( currentRow, 0 );
	int currentOriginalIndex = firstCell->data( kOriginalIndexRole ).toInt();
	if( currentOriginalIndex != row )
	{
		if( success )
		{
			m_thumbnailCache.insert( row, pixmap );
		}
		return;
	}

	if( success )
	{
		m_thumbnailCache.insert( row, pixmap );
		m_previewLabel->setPixmap( pixmap.scaled( m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation ) );
	}
	else if( reason == QLatin1String( "unsupported" ) )
	{
		m_previewLabel->setPixmap( QPixmap() );
		m_previewLabel->setText( QStringLiteral( "此类型暂不支持预览，仍可尝试恢复" ) );
	}
	else
	{
		m_previewLabel->setPixmap( QPixmap() );
		m_previewLabel->setText( QStringLiteral( "无法生成预览，文件可能不完整或格式无法识别" ) );
	}
}
