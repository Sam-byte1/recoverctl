/*
 * G9第二轮（UI视觉重做，2026-11-06）：全部图标用QPainter现场绘制几何
 * 图形，不引入Qt6Svg模块——这个项目一贯的原则是"能不加依赖就不加"
 * （见third_party/MANIFEST.md里Qt只链接Core/Gui/Widgets/Multimedia
 * 这几个核心模块的记录），几个简单图标的量级不值得为此新增一个Qt
 * 模块、多一份DLL要部署、多一条许可证核实记录。
 *
 * 图形设计和gui/这次一并产出的HTML设计稿（Artifact，石墨侧栏+雾白
 * 工作区+深青绿主色方向）用的是同一套坐标和配色，保证真实程序和
 * 设计稿看起来是同一个东西，不是另画了一套。
 */
#ifndef ICON_PAINTER_H
#define ICON_PAINTER_H

#include <QIcon>
#include <QPixmap>

namespace IconPainter
{

enum class NavIcon
{
	Disks,
	History,
	DeepSearch,
	About
};

/* 应用/窗口图标——多分辨率（16/32/64/128/256），给setWindowIcon用 */
QIcon appIcon();

/* 单一尺寸的品牌图形本身（不含QIcon多分辨率包装），侧栏品牌区用这个 */
QPixmap appGlyph( int size );

QPixmap navGlyph( NavIcon kind, int size );

/* removable=true画U盘/SD卡造型，false画机械硬盘造型——disk_enumerator
 * 能分辨的只有"Fixed/Removable"这一层粒度，不区分SSD/HDD，图标也只
 * 按这一层区分，不假装能识别更细的介质类型 */
QPixmap diskGlyph( bool removable, int size );

QPixmap imageGlyph( int size );

QPixmap lockGlyph( int size );

QPixmap refreshGlyph( int size );

/* 磁盘卡片右上角的圆形勾选标记，直接画在调用方已有的QPainter上（卡片
 * 自绘paintEvent里用），不是单独生成一张图片再贴上去 */
void paintSelectionBadge( QPainter &painter, const QRectF &rect, bool selected );

} // namespace IconPainter

#endif /* ICON_PAINTER_H */
