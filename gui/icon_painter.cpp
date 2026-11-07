#include "icon_painter.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

namespace IconPainter
{

namespace
{

/* 配色和app_style.h/HTML设计稿共用同一套命名，这里不去读QSS反查颜色
 * （QPainter绘制和QSS是两条独立的渲染路径），直接写字面量——和
 * app_style.h里的QSS颜色表需要保持同步，改配色时两边一起改 */
const QColor kGraphite( 0x15, 0x1d, 0x21 );
const QColor kMint( 0x9d, 0xe3, 0xcf );
const QColor kTeal( 0x12, 0x6a, 0x5d );
const QColor kTealLight( 0x58, 0xb7, 0x9b );
const QColor kMist( 0xf3, 0xf6, 0xf5 );
const QColor kIconOutline( 0x36, 0x53, 0x4f );
const QColor kIconBody( 0xdc, 0xe9, 0xe4 );
const QColor kIconMetal( 0xb8, 0xc6, 0xc4 );
const QColor kWarnText( 0x92, 0x5a, 0x18 );
const QColor kSidebarText( 0xcf, 0xd9, 0xdc );

QPixmap makeTransparentPixmap( int size )
{
	QPixmap pixmap( size, size );
	pixmap.fill( Qt::transparent );
	return pixmap;
}

/* 把"恢复箭头环+圆盘"这个品牌核心图形画进一个边长256的逻辑坐标系里
 * （和HTML设计稿的图标SVG viewBox 0 0 256 256完全一致），调用方负责
 * 在外面scale到目标像素尺寸、决定要不要画背景底板 */
void paintRecoveryGlyph( QPainter &painter )
{
	QPainterPath arcPath;
	const QRectF arcRect( 58, 58, 140, 140 ); /* 圆心(128,128)半径70 */
	arcPath.arcMoveTo( arcRect, -45 );
	arcPath.arcTo( arcRect, -45, -270 );

	QLinearGradient arcGradient( 58, 58, 198, 198 );
	arcGradient.setColorAt( 0, kTealLight );
	arcGradient.setColorAt( 1, kMint );

	QPen arcPen( QBrush( arcGradient ), 22 );
	arcPen.setCapStyle( Qt::RoundCap );
	painter.setPen( arcPen );
	painter.setBrush( Qt::NoBrush );
	painter.drawPath( arcPath );

	QPolygonF arrowHead;
	arrowHead << QPointF( 89.8, 67.2 ) << QPointF( 80.7, 89.1 ) << QPointF( 67.9, 76.3 );
	painter.setPen( Qt::NoPen );
	painter.setBrush( kMint );
	painter.drawPolygon( arrowHead );

	painter.setBrush( kMist );
	painter.drawEllipse( QPointF( 128, 128 ), 34, 34 );

	painter.setBrush( kTeal );
	painter.drawRoundedRect( QRectF( 104, 124, 48, 8 ), 4, 4 );
}

} // namespace

QPixmap appGlyph( int size )
{
	QPixmap pixmap = makeTransparentPixmap( size );
	QPainter painter( &pixmap );
	painter.setRenderHint( QPainter::Antialiasing );
	painter.scale( size / 256.0, size / 256.0 );

	painter.setPen( Qt::NoPen );
	painter.setBrush( kGraphite );
	painter.drawRoundedRect( QRectF( 0, 0, 256, 256 ), 56, 56 );

	paintRecoveryGlyph( painter );
	return pixmap;
}

QIcon appIcon()
{
	QIcon icon;
	const int sizes[] = { 16, 32, 48, 64, 128, 256 };
	for( int size : sizes )
	{
		icon.addPixmap( appGlyph( size ) );
	}
	return icon;
}

QPixmap navGlyph( NavIcon kind, int size )
{
	QPixmap pixmap = makeTransparentPixmap( size );
	QPainter painter( &pixmap );
	painter.setRenderHint( QPainter::Antialiasing );
	painter.scale( size / 16.0, size / 16.0 );

	QPen pen( kSidebarText, 1.3 );
	pen.setCapStyle( Qt::RoundCap );
	painter.setPen( pen );
	painter.setBrush( Qt::NoBrush );

	switch( kind )
	{
		case NavIcon::Disks:
			painter.drawRoundedRect( QRectF( 2, 3, 12, 10 ), 2, 2 );
			painter.drawLine( QPointF( 2, 7 ), QPointF( 14, 7 ) );
			break;

		case NavIcon::History:
			painter.drawEllipse( QPointF( 8, 8 ), 6, 6 );
			painter.drawLine( QPointF( 8, 5 ), QPointF( 8, 8 ) );
			painter.drawLine( QPointF( 8, 8 ), QPointF( 10, 9.5 ) );
			break;

		case NavIcon::DeepSearch:
			painter.drawEllipse( QPointF( 7, 7 ), 4.2, 4.2 );
			painter.drawLine( QPointF( 10.2, 10.2 ), QPointF( 13.5, 13.5 ) );
			break;

		case NavIcon::About:
			/* "i"字形——顶部一个圆点（零长度线+圆形线帽画成实心点），
			 * 下方一条竖线做字身 */
			painter.drawEllipse( QPointF( 8, 8 ), 6, 6 );
			painter.drawLine( QPointF( 8, 5.3 ), QPointF( 8, 5.3 ) );
			painter.drawLine( QPointF( 8, 7.3 ), QPointF( 8, 10.5 ) );
			break;
	}
	return pixmap;
}

QPixmap diskGlyph( bool removable, int size )
{
	QPixmap pixmap = makeTransparentPixmap( size );
	QPainter painter( &pixmap );
	painter.setRenderHint( QPainter::Antialiasing );
	painter.scale( size / 48.0, size / 48.0 );

	if( !removable )
	{
		QPen outline( kIconOutline, 1.6 );
		painter.setPen( outline );
		painter.setBrush( kIconBody );
		painter.drawRoundedRect( QRectF( 6, 10, 36, 28 ), 6, 6 );

		painter.setPen( QPen( kIconOutline, 1.2 ) );
		painter.setBrush( Qt::white );
		painter.drawRoundedRect( QRectF( 11, 15, 26, 9 ), 2, 2 );

		painter.setPen( QPen( kIconOutline, 1.4 ) );
		painter.setBrush( Qt::NoBrush );
		painter.drawEllipse( QPointF( 24, 30 ), 5.4, 5.4 );
		painter.setPen( QPen( kTealLight, 1.6, Qt::SolidLine, Qt::RoundCap ) );
		painter.drawLine( QPointF( 24, 30 ), QPointF( 27, 28.4 ) );

		painter.setPen( Qt::NoPen );
		painter.setBrush( kIconMetal );
		painter.drawRoundedRect( QRectF( 14, 35.5, 4, 2 ), 1, 1 );
		painter.drawRoundedRect( QRectF( 30, 35.5, 4, 2 ), 1, 1 );
	}
	else
	{
		painter.setPen( QPen( kIconOutline, 1.4 ) );
		painter.setBrush( kIconMetal );
		painter.drawRoundedRect( QRectF( 16, 6, 16, 10 ), 2, 2 );
		painter.setPen( Qt::NoPen );
		painter.setBrush( kIconOutline );
		painter.drawRect( QRectF( 19, 9, 3, 4 ) );
		painter.drawRect( QRectF( 26, 9, 3, 4 ) );

		painter.setPen( QPen( kIconOutline, 1.6 ) );
		painter.setBrush( kIconBody );
		painter.drawRoundedRect( QRectF( 9, 16, 30, 24 ), 6, 6 );

		painter.setPen( Qt::NoPen );
		painter.setBrush( kTealLight );
		painter.drawEllipse( QPointF( 24, 29 ), 3, 3 );
	}
	return pixmap;
}

QPixmap imageGlyph( int size )
{
	QPixmap pixmap = makeTransparentPixmap( size );
	QPainter painter( &pixmap );
	painter.setRenderHint( QPainter::Antialiasing );
	painter.scale( size / 48.0, size / 48.0 );

	painter.setPen( QPen( kIconOutline, 1.5 ) );
	painter.setBrush( kIconBody );
	painter.drawRoundedRect( QRectF( 8, 10, 24, 26 ), 3, 3 );

	painter.setBrush( Qt::white );
	painter.drawRoundedRect( QRectF( 14, 16, 24, 26 ), 3, 3 );

	painter.setPen( QPen( kTealLight, 1.6 ) );
	painter.setBrush( Qt::NoBrush );
	painter.drawEllipse( QPointF( 26, 29 ), 5, 5 );
	return pixmap;
}

QPixmap lockGlyph( int size )
{
	QPixmap pixmap = makeTransparentPixmap( size );
	QPainter painter( &pixmap );
	painter.setRenderHint( QPainter::Antialiasing );
	painter.scale( size / 16.0, size / 16.0 );

	QPen pen( kWarnText, 1.3 );
	pen.setCapStyle( Qt::RoundCap );
	painter.setPen( pen );
	painter.setBrush( Qt::NoBrush );
	painter.drawRoundedRect( QRectF( 4, 7, 8, 6 ), 1.2, 1.2 );

	/* 和HTML设计稿同一个锁形：弧心(8,5.2)半径2.5的半圆，弧的起止点正好
	 * 落在前面lineTo画到的(5.5,5.2)/(10.5,5.2)上，接续处不会有折角 */
	QPainterPath shackle;
	shackle.moveTo( 5.5, 7 );
	shackle.lineTo( 5.5, 5.2 );
	shackle.arcTo( QRectF( 5.5, 2.7, 5, 5 ), 180, -180 );
	shackle.lineTo( 10.5, 7 );
	painter.drawPath( shackle );
	return pixmap;
}

QPixmap refreshGlyph( int size )
{
	QPixmap pixmap = makeTransparentPixmap( size );
	QPainter painter( &pixmap );
	painter.setRenderHint( QPainter::Antialiasing );
	painter.scale( size / 16.0, size / 16.0 );

	QPainterPath arcPath;
	const QRectF arcRect( 3, 3, 10, 10 );
	arcPath.arcMoveTo( arcRect, -20 );
	arcPath.arcTo( arcRect, -20, -280 );

	QPen pen( kTeal, 1.4 );
	pen.setCapStyle( Qt::RoundCap );
	painter.setPen( pen );
	painter.setBrush( Qt::NoBrush );
	painter.drawPath( arcPath );

	QPolygonF arrowHead;
	arrowHead << QPointF( 13.4, 4.6 ) << QPointF( 10.8, 4.2 ) << QPointF( 12, 2 );
	painter.setPen( Qt::NoPen );
	painter.setBrush( kTeal );
	painter.drawPolygon( arrowHead );
	return pixmap;
}

void paintSelectionBadge( QPainter &painter, const QRectF &rect, bool selected )
{
	painter.save();
	painter.setRenderHint( QPainter::Antialiasing );

	if( selected )
	{
		painter.setPen( QPen( kTeal, 1.5 ) );
		painter.setBrush( kTeal );
	}
	else
	{
		painter.setPen( QPen( QColor( 0x81, 0x95, 0x8e ), 1.5 ) );
		painter.setBrush( Qt::white );
	}
	painter.drawEllipse( rect );

	if( selected )
	{
		QPainterPath check;
		double w = rect.width();
		double h = rect.height();
		check.moveTo( rect.left() + w * 0.27, rect.top() + h * 0.52 );
		check.lineTo( rect.left() + w * 0.44, rect.top() + h * 0.70 );
		check.lineTo( rect.left() + w * 0.75, rect.top() + h * 0.32 );

		QPen checkPen( Qt::white, qMax( 1.4, w * 0.14 ) );
		checkPen.setCapStyle( Qt::RoundCap );
		checkPen.setJoinStyle( Qt::RoundJoin );
		painter.setPen( checkPen );
		painter.setBrush( Qt::NoBrush );
		painter.drawPath( check );
	}
	painter.restore();
}

} // namespace IconPainter
