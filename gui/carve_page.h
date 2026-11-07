/*
 * G9第二轮（UI视觉重做，2026-11-06）：深度查找页——CarveDialog的内容
 * 从"非模态悬浮窗口"改造成侧栏常驻的一个页面（设计稿方向：深度查找是
 * 三个一级入口之一）。
 *
 * "运行期间仍可以用其它功能"这条G8裁决①的要求，现在由"它本来就是
 * 侧栏里的一个页面、切到别的页面不影响后台线程继续跑"这个更自然的
 * 页面导航模型满足，不再需要专门的非模态浮窗机制。CarveWorker+专属
 * 线程的用法原样保留（原因见carve_worker.h头注释——不能和ScanWorker
 * 共享线程）。runningChanged信号供MainWindow驱动侧栏"运行中"提示。
 */
#ifndef CARVE_PAGE_H
#define CARVE_PAGE_H

#include <QElapsedTimer>
#include <QThread>
#include <QWidget>

class CarveWorker;
class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;

/* 2026-11-07（世杰真机反馈：查找来源只有"浏览..."文件选择器，选不了
 * 磁盘，也没法真正点进磁盘里的文件夹确认）：PhotoRec深度查找本来就是
 * 对着整块裸设备/镜像文件扫描，不存在"扫描某个文件夹"这回事——`浏览...`
 * 按钮用QFileDialog::getOpenFileName()只能挑一个"文件"（给镜像文件
 * 这个用例用的），从来就选不了磁盘本身，用户想在这个对话框里选中一个
 * 磁盘/点进文件夹"确认"，撞的是原生文件选择器的固有限制，不是某个
 * 具体操作错了。新增"选择磁盘..."按钮，复用主页磁盘卡片同一套
 * disk_enumerator_list_physical_drives()真实枚举出的物理盘列表，
 * 直接出一个能选的磁盘列表，不需要用户知道\\.\PhysicalDriveN这种
 * 语法、也不用在文件对话框里打转。 */

class CarvePage : public QWidget
{
	Q_OBJECT

public:
	explicit CarvePage( QWidget *parent = nullptr );
	~CarvePage() override;

	bool isRunning() const;
	/* MainWindow退出前调用——雕刻运行中时请求取消并等待线程退出，
	 * 和原CarveDialog析构函数的逻辑一致，只是调用时机从"这个窗口被
	 * 析构时"改成"MainWindow::closeEvent接受关闭前" */
	void requestCancelAndWait();

signals:
	void requestStartCarve( QString photorecExePath, QString targetPath, QString outputDirBase );
	void requestStartCarvePartition( QString photorecExePath, QString devicePath, qint64 offset, quint64 size, QString outputDirBase );
	void runningChanged( bool running );

private slots:
	void onBrowsePhotorecPathClicked();
	void onBrowseTargetClicked();
	void onPickDiskClicked();
	void onBrowseOutputDirClicked();
	void onPartitionRangeToggled( bool checked );
	void onAdvancedToggled();
	void onStartClicked();
	void onCancelClicked();
	void onCarveFinished( bool success, bool cancelled, const QString &errorMessage, const QString &recupDirPath, int exitCode, int recoveredFileCount, int badSectorCount );
	void onElapsedTimerTick();

private:
	void setRunning( bool running );

	QPushButton *m_advancedToggleButton;
	QWidget *m_advancedPanel;
	QLineEdit *m_photorecPathEdit;
	QPushButton *m_browsePhotorecPathButton;
	QLineEdit *m_targetPathEdit;
	QPushButton *m_browseTargetButton;
	QPushButton *m_pickDiskButton;
	QLineEdit *m_outputDirEdit;
	QPushButton *m_browseOutputDirButton;
	QCheckBox *m_partitionRangeCheckBox;
	QLineEdit *m_partitionOffsetEdit;
	QLineEdit *m_partitionSizeEdit;
	QPushButton *m_startButton;
	QPushButton *m_cancelButton;
	QProgressBar *m_progressBar;
	QLabel *m_elapsedLabel;
	QLabel *m_resultLabel;
	QPushButton *m_openOutputButton;
	/* 2026-11-06（G9第二轮重构时顺带修复）：原CarveDialog每次完成都用
	 * layout()->addWidget(new QPushButton(...))新建一个"打开输出目录"
	 * 按钮——对话框场景下通常用一次就关掉，这个小累积问题不明显；
	 * 改成页面常驻之后同一个CarvePage实例可能多次开始/完成雕刻，
	 * 不修的话每跑完一次界面上就多一个重复按钮。现在固定一个按钮，
	 * 点击时读取这个成员变量里当前的路径，不在每次完成时新建控件 */
	QString m_lastRecupDirPath;

	QTimer *m_elapsedTimer;
	QElapsedTimer m_stopwatch;

	QThread m_workerThread;
	CarveWorker *m_worker;
	bool m_running;
};

#endif /* CARVE_PAGE_H */
