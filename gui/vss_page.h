/*
 * G9第二轮（UI视觉重做，2026-11-06）：历史版本页——VssWizardDialog的
 * 内容从"弹出的向导对话框"改造成侧栏常驻的一个页面（设计稿方向：
 * 历史版本是三个一级入口之一，不是扫描前的一个临时向导步骤）。
 *
 * 功能逻辑和原VssWizardDialog完全一致（在线查询当前盘的卷影副本/
 * 离线解析镜像文件里的store列表），只是交互收尾方式变了：原来是
 * exec()+accept()把ScanSource通过selectedSource()交回调用方，现在
 * 用户点"扫描此版本"直接emit scanRequested信号，MainWindow连到既有的
 * scanWithSource()——两条入口（直接扫描/历史版本）最终收敛到同一段
 * 收尾逻辑，这一点和重构前保持不变。
 */
#ifndef VSS_PAGE_H
#define VSS_PAGE_H

#include <QWidget>

#include "scan_source.h"

class QLineEdit;
class QListWidget;
class QPushButton;
class QTabWidget;
class QLabel;

class VssPage : public QWidget
{
	Q_OBJECT

public:
	explicit VssPage( QWidget *parent = nullptr );

signals:
	void scanRequested( ScanSource source );

private slots:
	void onQueryOnlineClicked();
	void onBrowseImageClicked();
	void onListOfflineStoresClicked();
	void onScanClicked();
	void onSelectionChanged();

private:
	QLineEdit *m_driveLetterEdit;
	QPushButton *m_queryOnlineButton;
	QListWidget *m_onlineList;

	QLineEdit *m_imagePathEdit;
	QPushButton *m_browseImageButton;
	QPushButton *m_listOfflineStoresButton;
	QListWidget *m_offlineList;

	QTabWidget *m_tabs;
	QLabel *m_selectionSummaryLabel;
	QPushButton *m_scanButton;
};

#endif /* VSS_PAGE_H */
