#include "bitlocker_wizard_dialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace
{

/* 和main_window.cpp的resolveDevicePath同样的"纯数字→物理盘路径"规则
 * 子集——BitLocker扫描本来就要求物理盘路径+字节偏移（B9真机验证过的
 * 既有限制：锁定的BitLocker卷不能走卷盘符设备对象读取），这里不需要
 * resolveDevicePath()里"单个字母/字母:"那部分卷盘符规则，只保留纯
 * 数字这一种简写；不跨文件复用main_window.cpp里那份file-local的
 * 匿名命名空间函数，重复几行判断逻辑的维护成本可以忽略（和
 * main_window.cpp注释里的理由一致）。 */
QString resolvePhysicalDrivePath( const QString &input )
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
	return trimmed;
}

} // namespace

BitLockerWizardDialog::BitLockerWizardDialog( QWidget *parent )
	: QDialog( parent )
	, m_devicePathEdit( nullptr )
	, m_offsetEdit( nullptr )
	, m_passwordRadio( nullptr )
	, m_recoveryKeyRadio( nullptr )
	, m_startupKeyRadio( nullptr )
	, m_fvekRadio( nullptr )
	, m_credentialTypeGroup( nullptr )
	, m_credentialInputStack( nullptr )
	, m_textValueEdit( nullptr )
	, m_togglePasswordButton( nullptr )
	, m_credentialFilePathEdit( nullptr )
	, m_browseCredentialFileButton( nullptr )
{
	setWindowTitle( QStringLiteral( "解锁加密磁盘" ) );
	resize( 480, 360 );

	QVBoxLayout *mainLayout = new QVBoxLayout( this );
	mainLayout->setContentsMargins( 20, 20, 20, 20 );
	mainLayout->setSpacing( 16 );

	QLabel *subtitle = new QLabel(
	     QStringLiteral( "需要有效的密码、恢复密钥或密钥文件才能打开这个加密磁盘" ), this );
	subtitle->setProperty( "role", "secondary" );
	subtitle->setWordWrap( true );
	mainLayout->addWidget( subtitle );

	QHBoxLayout *deviceRow = new QHBoxLayout();
	m_devicePathEdit = new QLineEdit( this );
	m_devicePathEdit->setPlaceholderText( QStringLiteral( "物理盘编号或路径，比如 0 或 \\\\.\\PhysicalDrive0" ) );
	deviceRow->addWidget( new QLabel( QStringLiteral( "物理盘:" ), this ) );
	deviceRow->addWidget( m_devicePathEdit, 1 );
	mainLayout->addLayout( deviceRow );

	QHBoxLayout *offsetRow = new QHBoxLayout();
	m_offsetEdit = new QLineEdit( this );
	m_offsetEdit->setPlaceholderText( QStringLiteral( "分区在物理盘上的字节偏移，比如 105906176" ) );
	offsetRow->addWidget( new QLabel( QStringLiteral( "分区偏移:" ), this ) );
	offsetRow->addWidget( m_offsetEdit, 1 );
	mainLayout->addLayout( offsetRow );

	/* G9（UI视觉重做）：四个纵向单选项代替下拉框——Codex建议"比下拉框
	 * 更容易了解可用方法"，用户不需要先点开下拉框才知道有哪些选项 */
	QLabel *credentialTypeLabel = new QLabel( QStringLiteral( "解锁方式:" ), this );
	mainLayout->addWidget( credentialTypeLabel );

	QVBoxLayout *radioLayout = new QVBoxLayout();
	radioLayout->setSpacing( 6 );
	m_passwordRadio = new QRadioButton( QStringLiteral( "使用密码" ), this );
	m_recoveryKeyRadio = new QRadioButton( QStringLiteral( "使用 48 位恢复密钥" ), this );
	m_startupKeyRadio = new QRadioButton( QStringLiteral( "使用启动密钥文件（.BEK）" ), this );
	m_fvekRadio = new QRadioButton( QStringLiteral( "使用 FVEK 密钥文件" ), this );
	m_passwordRadio->setChecked( true );

	m_credentialTypeGroup = new QButtonGroup( this );
	m_credentialTypeGroup->addButton( m_passwordRadio, 0 );
	m_credentialTypeGroup->addButton( m_recoveryKeyRadio, 1 );
	m_credentialTypeGroup->addButton( m_startupKeyRadio, 2 );
	m_credentialTypeGroup->addButton( m_fvekRadio, 3 );

	radioLayout->addWidget( m_passwordRadio );
	radioLayout->addWidget( m_recoveryKeyRadio );
	radioLayout->addWidget( m_startupKeyRadio );
	radioLayout->addWidget( m_fvekRadio );
	mainLayout->addLayout( radioLayout );

	/* 密码/恢复密钥用QLineEdit密码回显模式（输入内容不回显明文，和
	 * CLI的B11c交互式隐藏输入渠道同样的安全考虑，Qt原生控件自带，不
	 * 需要自己实现），右侧加一个显示/隐藏切换按钮（Codex建议）；
	 * 启动密钥/FVEK用QFileDialog选文件——两种输入形态切换用
	 * QStackedWidget，不是同一个控件来回改placeholder */
	m_credentialInputStack = new QStackedWidget( this );

	QWidget *textPage = new QWidget( m_credentialInputStack );
	QHBoxLayout *textLayout = new QHBoxLayout( textPage );
	textLayout->setContentsMargins( 0, 0, 0, 0 );
	m_textValueEdit = new QLineEdit( textPage );
	m_textValueEdit->setEchoMode( QLineEdit::Password );
	m_togglePasswordButton = new QPushButton( QStringLiteral( "显示" ), textPage );
	m_togglePasswordButton->setProperty( "role", "ghost" );
	m_togglePasswordButton->setCheckable( true );
	textLayout->addWidget( new QLabel( QStringLiteral( "凭据内容:" ), textPage ) );
	textLayout->addWidget( m_textValueEdit, 1 );
	textLayout->addWidget( m_togglePasswordButton );
	m_credentialInputStack->addWidget( textPage );

	QWidget *filePage = new QWidget( m_credentialInputStack );
	QHBoxLayout *fileLayout = new QHBoxLayout( filePage );
	fileLayout->setContentsMargins( 0, 0, 0, 0 );
	m_credentialFilePathEdit = new QLineEdit( filePage );
	m_credentialFilePathEdit->setReadOnly( true );
	m_credentialFilePathEdit->setPlaceholderText( QStringLiteral( "尚未选择文件" ) );
	m_browseCredentialFileButton = new QPushButton( QStringLiteral( "浏览..." ), filePage );
	m_browseCredentialFileButton->setProperty( "role", "ghost" );
	fileLayout->addWidget( new QLabel( QStringLiteral( "凭据文件:" ), filePage ) );
	fileLayout->addWidget( m_credentialFilePathEdit, 1 );
	fileLayout->addWidget( m_browseCredentialFileButton );
	m_credentialInputStack->addWidget( filePage );

	mainLayout->addWidget( m_credentialInputStack );
	mainLayout->addStretch( 1 );

	QDialogButtonBox *buttonBox = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
	buttonBox->button( QDialogButtonBox::Ok )->setText( QStringLiteral( "解锁并扫描" ) );
	buttonBox->button( QDialogButtonBox::Ok )->setProperty( "role", "primary" );
	buttonBox->button( QDialogButtonBox::Cancel )->setText( QStringLiteral( "取消" ) );
	buttonBox->button( QDialogButtonBox::Cancel )->setProperty( "role", "ghost" );
	mainLayout->addWidget( buttonBox );

	connect( m_credentialTypeGroup, &QButtonGroup::idClicked, this, &BitLockerWizardDialog::onCredentialTypeChanged );
	connect( m_togglePasswordButton, &QPushButton::toggled, this, &BitLockerWizardDialog::onTogglePasswordVisibility );
	connect( m_browseCredentialFileButton, &QPushButton::clicked, this, &BitLockerWizardDialog::onBrowseCredentialFileClicked );
	connect( buttonBox, &QDialogButtonBox::accepted, this, &BitLockerWizardDialog::onAccept );
	connect( buttonBox, &QDialogButtonBox::rejected, this, &BitLockerWizardDialog::reject );

	onCredentialTypeChanged();
}

void BitLockerWizardDialog::setInitialDevicePath( const QString &path )
{
	m_devicePathEdit->setText( path );
}

void BitLockerWizardDialog::setInitialOffset( quint64 offset )
{
	m_offsetEdit->setText( QString::number( offset ) );
}

void BitLockerWizardDialog::onCredentialTypeChanged()
{
	/* 密码/恢复密钥走文本页，启动密钥/FVEK走文件页 */
	bool isTextType = m_passwordRadio->isChecked() || m_recoveryKeyRadio->isChecked();
	m_credentialInputStack->setCurrentIndex( isTextType ? 0 : 1 );
}

void BitLockerWizardDialog::onTogglePasswordVisibility()
{
	bool showing = m_togglePasswordButton->isChecked();
	m_textValueEdit->setEchoMode( showing ? QLineEdit::Normal : QLineEdit::Password );
	m_togglePasswordButton->setText( showing ? QStringLiteral( "隐藏" ) : QStringLiteral( "显示" ) );
}

void BitLockerWizardDialog::onBrowseCredentialFileClicked()
{
	QString path = QFileDialog::getOpenFileName( this, QStringLiteral( "选择凭据文件" ) );
	if( !path.isEmpty() )
	{
		m_credentialFilePathEdit->setText( path );
	}
}

void BitLockerWizardDialog::onAccept()
{
	QString devicePath = resolvePhysicalDrivePath( m_devicePathEdit->text() );
	if( devicePath.isEmpty() )
	{
		QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先填写物理盘编号或路径" ) );
		return;
	}

	bool offsetOk = false;
	quint64 offset = m_offsetEdit->text().trimmed().toULongLong( &offsetOk );
	if( !offsetOk )
	{
		QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "分区偏移必须是一个非负整数（字节）" ) );
		return;
	}

	BitLockerCredentialType credentialType;
	bool isTextType;
	if( m_passwordRadio->isChecked() )
	{
		credentialType = BitLockerCredentialType::Password;
		isTextType = true;
	}
	else if( m_recoveryKeyRadio->isChecked() )
	{
		credentialType = BitLockerCredentialType::RecoveryKey;
		isTextType = true;
	}
	else if( m_startupKeyRadio->isChecked() )
	{
		credentialType = BitLockerCredentialType::StartupKey;
		isTextType = false;
	}
	else
	{
		credentialType = BitLockerCredentialType::Fvek;
		isTextType = false;
	}

	m_credentialInput.type = credentialType;
	if( isTextType )
	{
		if( m_textValueEdit->text().isEmpty() )
		{
			QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先填写凭据内容" ) );
			return;
		}
		m_credentialInput.textValue = m_textValueEdit->text();
		m_credentialInput.filePath.clear();
	}
	else
	{
		if( m_credentialFilePathEdit->text().isEmpty() )
		{
			QMessageBox::warning( this, QStringLiteral( "提示" ), QStringLiteral( "请先选择凭据文件" ) );
			return;
		}
		m_credentialInput.filePath = m_credentialFilePathEdit->text();
		m_credentialInput.textValue.clear();
	}

	m_selectedSource.type = ScanSourceType::BitLocker;
	m_selectedSource.devicePath = devicePath;
	m_selectedSource.baseOffset = offset;
	m_selectedSource.credentialType = credentialType;

	/* 对话框即将关闭、m_textValueEdit即将销毁——这里先把界面上的明文
	 * 凭据清掉，不留着等Qt自己的析构顺序去处理（哪怕只是尽力而为，见
	 * 2026-11-06裁决记录里对"GUI环境下清零只能尽力而为"的讨论；这里
	 * 清的是UI控件自己的回显缓冲区，不是ScanWorker那份负责真正清零的
	 * 副本，两者职责不重叠） */
	m_textValueEdit->clear();

	accept();
}

ScanSource BitLockerWizardDialog::selectedSource() const
{
	return m_selectedSource;
}

BitLockerCredentialInput BitLockerWizardDialog::credentialInput() const
{
	return m_credentialInput;
}
