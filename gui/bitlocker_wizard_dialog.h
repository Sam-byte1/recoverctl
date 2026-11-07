/*
 * G7（BitLocker接入GUI，2026-11-06小树裁决）：BitLocker前置向导——用户
 * 点"扫描BitLocker卷..."时弹出的输入框，产出一个ScanSource（不含凭据
 * 内容）+一个BitLockerCredentialInput（一次性凭据，用完即弃，不被
 * MainWindow长期持有，见scan_source.h头注释）。
 *
 * 和cli/main.c的`scan-bitlocker <磁盘/卷路径> <偏移> <凭据类型>
 * <凭据来源> [<凭据内容>]`相比，GUI不需要照搬"凭据来源file/cli/
 * interactive"这三选一分类——那是CLI没有图形控件时被迫的设计，GUI
 * 直接用Qt原生控件表达"渠道"：密码/恢复密钥用QLineEdit密码回显模式，
 * 启动密钥/FVEK用QFileDialog选文件（复用引擎已有的文件渠道解析，不
 * 在GUI侧重新实现格式解析逻辑）。
 */
#ifndef BITLOCKER_WIZARD_DIALOG_H
#define BITLOCKER_WIZARD_DIALOG_H

#include <QButtonGroup>
#include <QDialog>

#include "scan_source.h"

class QLineEdit;
class QPushButton;
class QRadioButton;
class QStackedWidget;

class BitLockerWizardDialog : public QDialog
{
	Q_OBJECT

public:
	explicit BitLockerWizardDialog( QWidget *parent = nullptr );

	/* I4（BitLocker自动识别，2026-11-06小树裁决）：磁盘卡片确认"已锁定
	 * 且单一物理区间"时，用这个方法把自动算出的物理盘路径+偏移预填进
	 * 来——字段仍然可编辑，用户发现不对可以自己改，不是只读锁死 */
	void setInitialDevicePath( const QString &path );
	void setInitialOffset( quint64 offset );

	/* 只有accept()之后（用户点了"确定"）调用才有意义，调用方应该先
	 * 检查exec()的返回值是QDialog::Accepted */
	ScanSource selectedSource() const;
	BitLockerCredentialInput credentialInput() const;

private slots:
	void onCredentialTypeChanged();
	void onBrowseCredentialFileClicked();
	/* G9（UI视觉重做）：密码框默认隐藏内容，按钮切换显示/隐藏——
	 * Codex建议的具体交互点，纯UI层状态，不碰凭据处理逻辑 */
	void onTogglePasswordVisibility();
	void onAccept();

private:
	QLineEdit *m_devicePathEdit;
	QLineEdit *m_offsetEdit;
	/* G9（UI视觉重做）：凭据类型从QComboBox改成四个纵向单选项——
	 * Codex建议"比下拉框更容易了解可用方法"，QButtonGroup管理互斥+
	 * 取当前选中项 */
	QRadioButton *m_passwordRadio;
	QRadioButton *m_recoveryKeyRadio;
	QRadioButton *m_startupKeyRadio;
	QRadioButton *m_fvekRadio;
	QButtonGroup *m_credentialTypeGroup;

	QStackedWidget *m_credentialInputStack;
	QLineEdit *m_textValueEdit;
	QPushButton *m_togglePasswordButton;
	QLineEdit *m_credentialFilePathEdit;
	QPushButton *m_browseCredentialFileButton;

	ScanSource m_selectedSource;
	BitLockerCredentialInput m_credentialInput;
};

#endif /* BITLOCKER_WIZARD_DIALOG_H */
