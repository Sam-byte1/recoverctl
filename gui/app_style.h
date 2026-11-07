/*
 * G9第二轮（UI视觉重做，2026-11-06）：全局视觉设计系统，第二版。
 *
 * 第一版（浅灰+白卡片+蓝色强调色）被世杰明确打回——"一点设计感都没有"。
 * 这一版不是继续咨询Codex要"更好看的颜色"，而是先做了真正的竞品视觉
 * 调研（下载并用Read工具实际看了Tenorshare 4DDiG的截图，不是只读文字
 * 描述），带着具体观察再找Codex要一份有态度的设计方向，世杰看过HTML
 * 原型后明确认可，这份QSS是把那份原型落地到真实Qt控件。
 *
 * 方向：石墨色常驻侧栏（深色、不随主题切换）+ 雾白工作区（浅色内容区）
 * +深青绿主操作色，点睛色用薄荷绿（只用在选中态强调，不是到处撒）。
 * 侧栏三个入口对应用户心智模型（磁盘与镜像/历史版本/深度查找），不是
 * 四个技术路径并列按钮——BitLocker不再是独立入口，见main_window.cpp
 * 里"解锁加密磁盘"链接按钮的位置和注释。
 */
#ifndef APP_STYLE_H
#define APP_STYLE_H

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QString>

/* 中文为主用微软雅黑UI，英文/数字搭配系统自带的Segoe UI系列——不随
 * 软件打包字体文件，两种字体在目标Windows版本上都是系统自带的。 */
inline void applyAppFont( QApplication &app )
{
	QFont font;
	QStringList candidates = { QStringLiteral( "Microsoft YaHei UI" ), QStringLiteral( "Microsoft YaHei" ), QStringLiteral( "Segoe UI" ) };
	QStringList available = QFontDatabase::families();

	for( const QString &name : candidates )
	{
		if( available.contains( name ) )
		{
			font.setFamily( name );
			break;
		}
	}
	font.setPointSize( 10 );
	app.setFont( font );
}

inline QString appStyleSheet()
{
	return QStringLiteral( R"(
QWidget {
	color: #202C30;
	background: transparent;
}

QMainWindow, QDialog {
	background: #F3F6F5;
}

/* ============ 侧栏（石墨色，常驻，不随内容区主题变化） ============ */
QWidget#sidebar {
	background: #202B30;
}

QLabel#sidebarBrandName {
	color: #EEF4F3;
	font-size: 12px;
	font-weight: 700;
}

QLabel[role="navGroupTitle"] {
	color: #9BAEB4;
	font-size: 11px;
	font-weight: 600;
}

QPushButton[navItem="true"] {
	background: transparent;
	border: none;
	border-radius: 8px;
	color: #CFD9DC;
	text-align: left;
	padding: 8px 10px;
	font-size: 13px;
}

QPushButton[navItem="true"]:hover {
	background: #26363B;
}

QPushButton[navItem="true"]:checked {
	background: #284A48;
	color: #F1FAF8;
	font-weight: 600;
}

QLabel[role="sidebarTask"] {
	background: #26363B;
	color: #CFD9DC;
	font-size: 11.5px;
	border-radius: 8px;
	padding: 8px;
}

/* ============ 工作区通用文字/卡片 ============ */
QLabel#pageTitle {
	font-size: 20px;
	font-weight: 600;
	color: #202C30;
}

QLabel#pageSubtitle {
	color: #596A70;
	font-size: 13px;
}

QLabel[role="secondary"] {
	color: #596A70;
}

QLabel[role="placeholder"] {
	color: #596A70;
	font-size: 13px;
}

QLabel[role="success"] {
	color: #28704F;
}

QLabel[role="danger"] {
	color: #B34242;
}

QLabel[role="warning"] {
	color: #925A18;
}

QFrame[card="true"] {
	background: #FFFFFF;
	border: 1px solid #D8E2DE;
	border-radius: 10px;
}

/* 磁盘/镜像卡片——比普通卡片边框略粗，选中态用主色描边+浅色底 */
QFrame[card="device"] {
	background: #FFFFFF;
	border: 1.5px solid #D8E2DE;
	border-radius: 12px;
}

QFrame[card="device"]:hover {
	border: 1.5px solid #81958E;
}

QFrame[card="device"][selected="true"] {
	background: #E1F1EB;
	border: 1.5px solid #126A5D;
}

QFrame[notice="true"] {
	background: #EAF0EE;
	border: none;
	border-radius: 8px;
}

QLineEdit, QComboBox {
	background: #FFFFFF;
	border: 1px solid #81958E;
	border-radius: 6px;
	min-height: 22px;
	padding: 5px 10px;
	selection-background-color: #126A5D;
	selection-color: #FFFFFF;
}

QLineEdit:focus, QComboBox:focus {
	border: 1px solid #126A5D;
}

QLineEdit:disabled, QComboBox:disabled {
	background: #EAF0EE;
	color: #596A70;
	border: 1px solid #D8E2DE;
}

QLineEdit[readOnly="true"] {
	background: #EAF0EE;
	color: #596A70;
}

QComboBox::drop-down {
	border: none;
	width: 22px;
}

QPushButton {
	background: #FFFFFF;
	border: 1px solid #81958E;
	border-radius: 6px;
	min-height: 22px;
	padding: 6px 16px;
}

QPushButton:hover {
	background: #EAF0EE;
}

QPushButton:pressed {
	background: #D8E2DE;
}

QPushButton:focus {
	border: 1px solid #126A5D;
}

QPushButton:disabled {
	background: #EAF0EE;
	color: #596A70;
	border: 1px solid #D8E2DE;
}

QPushButton[role="primary"] {
	background: #126A5D;
	color: #FFFFFF;
	border: 1px solid #126A5D;
	font-weight: 600;
	padding: 8px 20px;
}

QPushButton[role="primary"]:hover {
	background: #0D554A;
	border: 1px solid #0D554A;
}

QPushButton[role="primary"]:pressed {
	background: #0A4038;
	border: 1px solid #0A4038;
}

QPushButton[role="primary"]:disabled {
	background: #C7D3D0;
	color: #8A9794;
	border: 1px solid #C7D3D0;
}

QPushButton[role="ghost"] {
	background: transparent;
	border: 1px solid transparent;
	color: #126A5D;
	padding: 6px 10px;
}

QPushButton[role="ghost"]:hover {
	background: #E1F1EB;
	border: 1px solid transparent;
}

QPushButton[role="ghost"]:pressed {
	background: #D8E2DE;
}

QPushButton[role="ghost"]:disabled {
	background: transparent;
	color: #596A70;
	border: 1px solid transparent;
}

QPushButton[role="danger-ghost"] {
	background: transparent;
	border: 1px solid #B34242;
	color: #B34242;
}

QPushButton[role="danger-ghost"]:hover {
	background: #FCEDEC;
}

QPushButton[role="danger-ghost"]:disabled {
	background: transparent;
	color: #596A70;
	border: 1px solid #D8E2DE;
}

QCheckBox, QRadioButton {
	spacing: 8px;
}

QCheckBox::indicator, QRadioButton::indicator {
	width: 16px;
	height: 16px;
	border: 1px solid #81958E;
	background: #FFFFFF;
}

QCheckBox::indicator {
	border-radius: 4px;
}

QRadioButton::indicator {
	border-radius: 8px;
}

QCheckBox::indicator:checked, QRadioButton::indicator:checked {
	background: #126A5D;
	border: 1px solid #126A5D;
}

QTableWidget {
	background: #FFFFFF;
	alternate-background-color: #FAFCFB;
	border: none;
	gridline-color: transparent;
	selection-background-color: #E1F1EB;
	selection-color: #202C30;
}

QTableWidget::item {
	padding: 4px 12px;
	border-bottom: 1px solid #EAF0EE;
}

QTableWidget::item:selected {
	background: #E1F1EB;
	color: #202C30;
}

QHeaderView::section {
	background: #EAF0EE;
	color: #596A70;
	border: none;
	border-bottom: 1px solid #D8E2DE;
	padding: 8px 12px;
	font-weight: 600;
}

QProgressBar {
	background: #EAF0EE;
	border: none;
	border-radius: 6px;
	min-height: 8px;
	max-height: 8px;
	text-align: center;
}

QProgressBar::chunk {
	background: #126A5D;
	border-radius: 6px;
}

QScrollBar:vertical {
	background: transparent;
	width: 10px;
	margin: 0;
}

QScrollBar::handle:vertical {
	background: #C6D2CF;
	border-radius: 5px;
	min-height: 24px;
}

QScrollBar::handle:vertical:hover {
	background: #81958E;
}

QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
	height: 0;
}

QSplitter::handle {
	background: #D8E2DE;
	width: 1px;
}

QTabWidget::pane {
	border: 1px solid #D8E2DE;
	border-radius: 8px;
	background: #FFFFFF;
	top: -1px;
}

QTabBar::tab {
	background: transparent;
	color: #596A70;
	padding: 8px 16px;
	border: none;
	border-bottom: 2px solid transparent;
}

QTabBar::tab:selected {
	color: #126A5D;
	border-bottom: 2px solid #126A5D;
	font-weight: 600;
}

QTabBar::tab:hover:!selected {
	color: #202C30;
}
)" );
}

#endif /* APP_STYLE_H */
