/*
 * G3（P6 GUI壳层，2026-11-06小树裁决）：三种文件系统（NTFS/FAT32/exFAT）
 * 已删除候选项的统一Qt端表示。C引擎侧有三个不同的候选结构体
 * （undelete_candidate_t/fat_undelete_candidate_t/exfat_undelete_candidate_t），
 * 字段形状不完全一样——NTFS用MFT索引定位、没有file_size字段；
 * FAT32/exFAT用first_cluster+file_size定位、exFAT还多一个no_fat_chain
 * 标志位。这里不强行假装三者字段一致，而是用一个fsType区分，
 * 恢复时只看对应fsType的那组字段（小树裁决：不提前为还没出现的差异
 * 发明一个"通用"但大多数字段都用不上的结构）。
 *
 * 是纯值类型（没有QObject继承），可以安全地按值跨线程传递，不涉及
 * 共享可变状态——这是小树裁决④里"结果数据现场转换成Qt端值类型"这个
 * 判断的具体落地。
 */
#ifndef CANDIDATE_ITEM_H
#define CANDIDATE_ITEM_H

#include <QMetaType>
#include <QString>
#include <QVector>

enum class CandidateFsType
{
	Ntfs,
	Fat32,
	ExFat
};

struct CandidateItem
{
	CandidateFsType fsType = CandidateFsType::Ntfs;

	QString name;
	/* 所在目录路径，不含文件名本身：NTFS是沿$FILE_NAME父目录链重建出的
	 * 完整相对路径（可能是多级），FAT32/exFAT是扫描时记录的单层目录名
	 * （engine目前的FAT32/exFAT实现只验证一层子目录，这不是GUI这边的
	 * 限制，是engine/shell对应scanner的既有范围边界，见各自头文件）。 */
	QString directoryPath;
	/* NTFS候选项结构体本身不携带文件大小（undelete_candidate_t没有这个
	 * 字段），hasSize=false时UI不应该显示一个假的0，而是显示"未知"。 */
	bool hasSize = false;
	quint64 size = 0;

	/* 恢复用的定位参数——只有和fsType匹配的那组字段有意义：
	 * Ntfs用mftIndex；Fat32/ExFat用firstCluster+size（size字段复用上面
	 * 那个，hasSize在FAT32/exFAT场景下恒为true）；noFatChain只有ExFat
	 * 用（1=连续存储直接顺序读，0=碎片化要跟着FAT链走，见
	 * exfat_reader_recover_file头注释）。 */
	quint64 mftIndex = 0;
	quint32 firstCluster = 0;
	bool noFatChain = true;
};

Q_DECLARE_METATYPE( CandidateItem )
Q_DECLARE_METATYPE( QVector<CandidateItem> )

#endif /* CANDIDATE_ITEM_H */
