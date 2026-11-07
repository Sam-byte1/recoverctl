/*
 * G9第二轮（UI视觉重做，2026-11-06）：主窗口结构从"单页面、四个技术
 * 路径按钮并列"改成"常驻侧栏+三个一级入口页面"（磁盘与镜像/历史版本/
 * 深度查找），对应世杰看过HTML设计稿之后认可的方向。BitLocker不再是
 * 侧栏的第四个入口，收进"磁盘与镜像"页面里的一个链接按钮（见.cpp里
 * 对应注释——disk_enumerator现在识别不了哪块盘被加密，不能在磁盘卡片
 * 上画锁形角标假装会自动识别，只能维持"用户自己知道要解锁"这条路）。
 *
 * G3（P6 GUI壳层，2026-11-06小树裁决）：真正的扫描/恢复逻辑都在
 * ScanWorker里，这个类只负责摆控件、转发用户操作、把worker的信号结果
 * 画到界面上，不直接调用任何engine函数——这条边界这次重构没有变。
 */
#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QHash>
#include <QMainWindow>
#include <QPixmap>
#include <QThread>
#include <QVector>

#include "candidate_item.h"
#include "scan_source.h"

class CarvePage;
class DeviceCard;
class QCheckBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTableWidgetItem;
class ScanWorker;
class VssPage;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit MainWindow( QWidget *parent = nullptr );
	~MainWindow() override;

signals:
	void requestScan( ScanSource source );
	void requestScanBitLocker( ScanSource source, BitLockerCredentialInput credential );
	void requestRecover( ScanSource source, CandidateItem item, const QString &outputPath );
	/* H2：多选恢复——参数直接透传给ScanWorker::recoverBatch() */
	void requestRecoverBatch( ScanSource source, QVector<CandidateItem> items, const QString &outputDir );
	void requestThumbnail( int row, ScanSource source, CandidateItem item );

protected:
	/* G9第二轮新增：深度查找运行期间原来由CarveDialog::closeEvent拦截
	 * "关掉那个窗口"，现在深度查找只是侧栏的一个页面、没有自己的窗口，
	 * 这个保护职责上移到MainWindow——关闭整个应用前检查CarvePage是否
	 * 还在跑 */
	void closeEvent( QCloseEvent *event ) override;

private slots:
	void onRefreshDisksClicked();
	void onDeviceCardClicked( DeviceCard *card );
	void onScanClicked();
	void onOpenImageClicked();
	void onUnlockBitLockerClicked();
	void onChangeSourceClicked();
	void onVssScanRequested( ScanSource source );
	void onCarveRunningChanged( bool running );
	void onAboutClicked();
	void onRecoverClicked();
	void onProgress( quint64 current, quint64 total );
	void onScanFinished( bool success, const QString &errorMessage, const QVector<CandidateItem> &candidates );
	void onRecoverFinished( bool success, const QString &errorMessage, int badSectorCount, quint64 badSectorBytes );
	/* H2：批量恢复逐项/汇总完成——用于更新"正在恢复X/Y"状态文案和最终
	 * 弹窗摘要，不单独处理每一项的错误（失败文件名汇总在完成弹窗里）。 */
	void onRecoverBatchItemFinished( int index, bool success, const QString &errorMessage, const QString &outputPath, int badSectorCount, quint64 badSectorBytes );
	void onRecoverBatchFinished( int successCount, int totalCount, const QStringList &failedNames, int totalBadSectorCount, quint64 totalBadSectorBytes );
	void onFilterTextChanged( const QString &text );
	void onTableSelectionChanged();
	void onThumbnailReady( int row, bool success, const QString &reason, const QPixmap &pixmap );
	void onAdvancedToggled();
	/* H1追加（世杰真机反馈：没有全选，只能一个个点）：表格每行名称格前
	 * 的勾选框状态变化——`itemChanged`是QTableWidget通用的数据变化信号，
	 * 不是专门的"勾选框"信号，但这个表格里唯一会被setData()触发的场景
	 * 就是勾选框切换（其它列是只读的NoEditTriggers，不会被用户改写），
	 * 所以不需要额外过滤是不是勾选框那一列 */
	void onTableItemChanged( QTableWidgetItem *item );
	void onSelectAllClicked();
	/* 2026-11-06（世杰追加要求：全选按钮挪到表格标题"名称"旁边，更符合
	 * 常见应用"表头勾选框=全选"的既有习惯）：表头里嵌入的勾选框只在
	 * 列宽变化时需要重新定位——窗口缩放、拖动预览区分隔条都会通过
	 * QHeaderView的stretch机制触发列宽变化，连这一个信号就够，不需要
	 * 分别监听窗口resize事件 */
	void repositionSelectAllHeaderCheckBox();

private:
	void setBusy( bool busy );
	void populateTable( const QVector<CandidateItem> &candidates );
	/* source/sourceTitle: scanWithSource()只认ScanSource；sourceTitle
	 * 是给面包屑/摘要文案展示用的人话描述（比如磁盘标题、镜像文件名、
	 * "历史版本"、"加密磁盘"），不影响扫描逻辑本身 */
	void scanWithSource( const ScanSource &source, const QString &sourceTitle );
	void scanBitLockerWithCredential( const ScanSource &source, const BitLockerCredentialInput &credential, const QString &sourceTitle );
	void applyFilter();
	void updateResultsEmptyState();
	void rebuildDeviceCards();
	void updateSelectionBar();
	void switchToResultsPage( const QString &sourceTitle );
	void switchToPickerPage();
	/* H1追加：勾选框状态变化/全选按钮点击之后，统一走这里更新"全选"
	 * 按钮文案（取决于"当前可见行是不是已经全部勾上了"）、底部"已勾选
	 * N个"摘要、恢复按钮文案——三处UI联动，避免在多个槽函数里各写一遍 */
	void updateCheckedSummary();

	/* ---- 侧栏 ---- */
	QPushButton *m_navDisksButton;
	QPushButton *m_navHistoryButton;
	QPushButton *m_navDeepSearchButton;
	QPushButton *m_navAboutButton;
	QLabel *m_sidebarTaskLabel;
	QStackedWidget *m_contentStack;

	/* ---- 磁盘与镜像：选择来源子页 ---- */
	QWidget *m_homeStack_picker;
	QGridLayout *m_fixedCardGrid;
	QGridLayout *m_removableCardGrid;
	QWidget *m_removableSection;
	QVector<DeviceCard *> m_deviceCards;
	DeviceCard *m_selectedCard;
	QLabel *m_selectionSummaryLabel;
	QPushButton *m_scanButton;
	QPushButton *m_refreshButton;
	QPushButton *m_openImageButton;
	QPushButton *m_unlockBitLockerButton;
	QPushButton *m_advancedToggleButton;
	QWidget *m_advancedPanel;
	QLineEdit *m_devicePathEdit;
	QStackedWidget *m_homeStack;

	/* ---- 磁盘与镜像：扫描结果子页 ---- */
	QWidget *m_homeStack_results;
	QLabel *m_resultsBreadcrumbLabel;
	QLineEdit *m_filterEdit;
	/* 2026-11-06：从筛选框旁边的独立按钮改成嵌进表格标题"名称"列里的
	 * 勾选框（parent是m_table->horizontalHeader()本身，不是普通布局里
	 * 的子控件，位置靠repositionSelectAllHeaderCheckBox()手动摆放，
	 * 这是Qt里"表头嵌入可交互控件"的标准做法——QHeaderView本身不支持
	 * 直接在某个section里放一个真正可点击的勾选框，只能把它当成一个
	 * 独立悬浮在表头视口上的子widget） */
	QCheckBox *m_selectAllHeaderCheckBox;
	QStackedWidget *m_resultsStack;
	QLabel *m_emptyStateLabel;
	QTableWidget *m_table;
	QLabel *m_previewLabel;
	QLabel *m_checkedSummaryLabel;
	QPushButton *m_recoverButton;
	QProgressBar *m_progressBar;
	QLabel *m_statusLabel;

	/* ---- 历史版本 / 深度查找 ---- */
	VssPage *m_vssPage;
	CarvePage *m_carvePage;

	QThread m_workerThread;
	ScanWorker *m_worker;

	QVector<CandidateItem> m_candidates;
	ScanSource m_currentScanSource;
	QHash<int, QPixmap> m_thumbnailCache;
	/* H2：批量恢复进行中的总数，onRecoverBatchItemFinished()只带下标，
	 * 配合这个值才能拼出"正在恢复X/Y"的状态文案 */
	int m_batchTotal;
};

#endif /* MAIN_WINDOW_H */
