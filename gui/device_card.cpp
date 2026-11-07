#include "device_card.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QVBoxLayout>

#include "icon_painter.h"

namespace
{

/* 和main_window.cpp里的同名函数是同一段逻辑的独立副本——纯展示格式化，
 * 几行算术，不值得为了复用新开一个共享头文件（和这个项目里
 * resolveDevicePath在main_window.cpp/bitlocker_wizard_dialog.cpp
 * 各自保留一份同样的理由） */
QString formatBytes( quint64 bytes )
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

QLabel *makeBadge( const QString &text, QWidget *parent )
{
	QLabel *label = new QLabel( text, parent );
	label->setStyleSheet( QStringLiteral(
	     "background: #EAF0EE; color: #596A70; font-size: 11px; font-weight: 600;"
	     "border-radius: 8px; padding: 2px 8px;" ) );
	return label;
}

} // namespace

CapacityBar::CapacityBar( QWidget *parent )
	: QWidget( parent )
	, m_fraction( 0.0 )
{
	setFixedHeight( 4 );
}

void CapacityBar::setFraction( double fraction )
{
	m_fraction = qBound( 0.0, fraction, 1.0 );
	update();
}

void CapacityBar::paintEvent( QPaintEvent * )
{
	QPainter painter( this );
	painter.setRenderHint( QPainter::Antialiasing );
	painter.setPen( Qt::NoPen );

	painter.setBrush( QColor( 0xD8, 0xE2, 0xDE ) );
	painter.drawRoundedRect( rect(), 2, 2 );

	if( m_fraction > 0.0 )
	{
		QRectF filled( 0, 0, width() * m_fraction, height() );
		painter.setBrush( QColor( 0x7C, 0x94, 0x90 ) );
		painter.drawRoundedRect( filled, 2, 2 );
	}
}

DeviceCard::DeviceCard( QWidget *parent )
	: QFrame( parent )
	, m_iconLabel( nullptr )
	, m_titleLabel( nullptr )
	, m_kindLabel( nullptr )
	, m_capacityBar( nullptr )
	, m_capacityTextLabel( nullptr )
	, m_fsBadgeLabel( nullptr )
	, m_kindBadgeLabel( nullptr )
	, m_selected( false )
	, m_locked( false )
	, m_physicalDriveOffset( 0 )
{
	setProperty( "card", "device" );
	setProperty( "selected", false );
	setAttribute( Qt::WA_Hover, true );
	setCursor( Qt::PointingHandCursor );

	QVBoxLayout *mainLayout = new QVBoxLayout( this );
	mainLayout->setContentsMargins( 16, 14, 16, 14 );
	mainLayout->setSpacing( 10 );

	QHBoxLayout *topRow = new QHBoxLayout();
	topRow->setSpacing( 10 );
	m_iconLabel = new QLabel( this );
	m_iconLabel->setFixedSize( 36, 36 );

	QVBoxLayout *textCol = new QVBoxLayout();
	textCol->setSpacing( 1 );
	m_titleLabel = new QLabel( this );
	m_titleLabel->setStyleSheet( QStringLiteral( "font-size: 14px; font-weight: 600; color: #202C30;" ) );
	m_kindLabel = new QLabel( this );
	m_kindLabel->setStyleSheet( QStringLiteral( "font-size: 11.5px; color: #596A70;" ) );
	textCol->addWidget( m_titleLabel );
	textCol->addWidget( m_kindLabel );

	topRow->addWidget( m_iconLabel );
	topRow->addLayout( textCol, 1 );
	mainLayout->addLayout( topRow );

	m_capacityBar = new CapacityBar( this );
	mainLayout->addWidget( m_capacityBar );

	m_capacityTextLabel = new QLabel( this );
	m_capacityTextLabel->setStyleSheet( QStringLiteral( "font-size: 11.5px; color: #596A70;" ) );
	mainLayout->addWidget( m_capacityTextLabel );

	QHBoxLayout *badgeRow = new QHBoxLayout();
	badgeRow->setSpacing( 6 );
	m_fsBadgeLabel = makeBadge( QString(), this );
	m_kindBadgeLabel = makeBadge( QString(), this );
	badgeRow->addWidget( m_fsBadgeLabel );
	badgeRow->addWidget( m_kindBadgeLabel );
	badgeRow->addStretch( 1 );
	mainLayout->addLayout( badgeRow );
}

void DeviceCard::setDiskInfo( const QString &title, bool removable, const QString &fsLabel, quint64 totalBytes, quint64 freeBytes )
{
	m_title = title;
	m_devicePath.clear(); /* 调用方随后会用setDevicePath()补上，这里只重置展示内容 */

	m_titleLabel->setText( title );
	m_kindLabel->setText( removable ? QStringLiteral( "外接设备" ) : QStringLiteral( "本机磁盘" ) );
	m_iconLabel->setPixmap( IconPainter::diskGlyph( removable, 36 ) );

	m_fsBadgeLabel->setText( fsLabel.isEmpty() ? QStringLiteral( "未知文件系统" ) : fsLabel );
	m_kindBadgeLabel->setVisible( removable );
	m_kindBadgeLabel->setText( QStringLiteral( "可移动" ) );

	if( totalBytes == 0 )
	{
		m_capacityBar->setVisible( false );
		m_capacityTextLabel->setText( QStringLiteral( "容量信息不可用" ) );
		m_summaryText = fsLabel;
	}
	else
	{
		quint64 usedBytes = ( totalBytes >= freeBytes ) ? ( totalBytes - freeBytes ) : 0;
		double fraction = (double) usedBytes / (double) totalBytes;
		m_capacityBar->setVisible( true );
		m_capacityBar->setFraction( fraction );
		m_capacityTextLabel->setText( QStringLiteral( "已用 %1 / 共 %2" ).arg( formatBytes( usedBytes ), formatBytes( totalBytes ) ) );
		m_summaryText = fsLabel.isEmpty() ? formatBytes( totalBytes ) : QStringLiteral( "%1 · %2" ).arg( fsLabel, formatBytes( totalBytes ) );
	}
}

void DeviceCard::setDevicePath( const QString &path )
{
	m_devicePath = path;
}

QString DeviceCard::devicePath() const
{
	return m_devicePath;
}

QString DeviceCard::title() const
{
	return m_title;
}

QString DeviceCard::summaryText() const
{
	return m_summaryText;
}

void DeviceCard::setSelected( bool selected )
{
	m_selected = selected;
	setProperty( "selected", selected );
	style()->unpolish( this );
	style()->polish( this );
	update();
}

bool DeviceCard::isSelected() const
{
	return m_selected;
}

void DeviceCard::setLocked( const QString &physicalDrivePath, quint64 offset )
{
	m_locked = true;
	m_physicalDrivePath = physicalDrivePath;
	m_physicalDriveOffset = offset;
	update();
}

bool DeviceCard::isLocked() const
{
	return m_locked;
}

QString DeviceCard::physicalDrivePath() const
{
	return m_physicalDrivePath;
}

quint64 DeviceCard::physicalDriveOffset() const
{
	return m_physicalDriveOffset;
}

void DeviceCard::mousePressEvent( QMouseEvent *event )
{
	if( event->button() == Qt::LeftButton )
	{
		emit cardClicked();
	}
	QFrame::mousePressEvent( event );
}

void DeviceCard::paintEvent( QPaintEvent *event )
{
	QFrame::paintEvent( event );

	QPainter painter( this );
	QRectF badgeRect( width() - 30, 10, 20, 20 );
	IconPainter::paintSelectionBadge( painter, badgeRect, m_selected );

	if( m_locked )
	{
		/* 锁形角标画在设备图标的右下角——只有bitlocker_status_checker
		 * 真机确认"已锁定"才会调用setLocked()，这是真实状态驱动的
		 * 角标，不是设计稿里的演示数据 */
		QRect iconGeo = m_iconLabel->geometry();
		QPixmap lockPixmap = IconPainter::lockGlyph( 18 );
		QPoint lockPos( iconGeo.right() - 12, iconGeo.bottom() - 12 );
		painter.setPen( Qt::NoPen );
		painter.setBrush( QColor( 0xFF, 0xF3, 0xDE ) );
		painter.drawEllipse( QRectF( lockPos.x(), lockPos.y(), 18, 18 ) );
		painter.drawPixmap( lockPos, lockPixmap );
	}
}

void DeviceCard::enterEvent( QEnterEvent *event )
{
	QFrame::enterEvent( event );
	update();
}

void DeviceCard::leaveEvent( QEvent *event )
{
	QFrame::leaveEvent( event );
	update();
}
