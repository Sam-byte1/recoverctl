/*
 * G9第二轮（UI视觉重做，2026-11-06）：磁盘与镜像首页的卡片控件——
 * 展示一个来自disk_enumerator_list_logical_drives()的真实逻辑磁盘。
 *
 * 只画disk_entry_t实际提供的信息（容量/文件系统/Fixed-or-Removable），
 * 不假装能识别SSD/USB这类更细的介质类型——disk_enumerator分不出这个。
 *
 * I4（BitLocker自动识别，2026-11-06小树裁决）：加密锁形角标现在是
 * 真实数据驱动的——`bitlocker_status_checker_query()`/
 * `volume_disk_extents_query()`（I0真机验证过两者真实行为）确认"已
 * 锁定"且"卷只跨单一物理区间"才会显示角标+把devicePath()切换成
 * 物理盘路径；查询失败或者卷跨多个物理区间（极少见）时不显示角标，
 * 优雅回退到main_window.cpp里"解锁加密磁盘…"这个独立手动入口，不
 * 假装自动识别在这种情况下也能用。
 */
#ifndef DEVICE_CARD_H
#define DEVICE_CARD_H

#include <QFrame>
#include <QString>

class QLabel;

/* 容量条——占用比例的自绘进度条，不用QProgressBar（QProgressBar自带
 * 的文字/百分比语义和这里"纯视觉条"的需求对不上，QSS也不好单独定制
 * 成这么细的一条） */
class CapacityBar : public QWidget
{
public:
	explicit CapacityBar( QWidget *parent = nullptr );
	void setFraction( double fraction );

protected:
	void paintEvent( QPaintEvent *event ) override;

private:
	double m_fraction;
};

class DeviceCard : public QFrame
{
	Q_OBJECT

public:
	explicit DeviceCard( QWidget *parent = nullptr );

	/* totalBytes==0表示这个磁盘取不到容量信息（disk_enumerator对某些
	 * 设备类型可能拿不到），这种情况下不画容量条，只画文件系统/类型 */
	void setDiskInfo( const QString &title, bool removable, const QString &fsLabel, quint64 totalBytes, quint64 freeBytes );
	void setDevicePath( const QString &path );
	QString devicePath() const;

	void setSelected( bool selected );
	bool isSelected() const;

	/* "NTFS · 256 GB"这种摘要文案——底部选择栏拼"已选择：XXX · 摘要"
	 * 用，摘要本身的格式化逻辑只在这里写一份 */
	QString summaryText() const;
	QString title() const;

	/* I4：physicalDrivePath形如"\\.\PhysicalDrive3"，offset是分区在
	 * 物理盘上的字节偏移——只有当bitlocker_status_checker确认"已锁定"
	 * 且volume_disk_extents确认"单一区间"才调用这个方法，调用之后
	 * isLocked()返回true、画锁形角标；不调用则保持未锁定的普通外观 */
	void setLocked( const QString &physicalDrivePath, quint64 offset );
	bool isLocked() const;
	QString physicalDrivePath() const;
	quint64 physicalDriveOffset() const;

signals:
	void cardClicked();

protected:
	void mousePressEvent( QMouseEvent *event ) override;
	void paintEvent( QPaintEvent *event ) override;
	void enterEvent( QEnterEvent *event ) override;
	void leaveEvent( QEvent *event ) override;

private:
	QLabel *m_iconLabel;
	QLabel *m_titleLabel;
	QLabel *m_kindLabel;
	CapacityBar *m_capacityBar;
	QLabel *m_capacityTextLabel;
	QLabel *m_fsBadgeLabel;
	QLabel *m_kindBadgeLabel;

	QString m_devicePath;
	QString m_title;
	QString m_summaryText;
	bool m_selected;

	bool m_locked;
	QString m_physicalDrivePath;
	quint64 m_physicalDriveOffset;
};

#endif /* DEVICE_CARD_H */
