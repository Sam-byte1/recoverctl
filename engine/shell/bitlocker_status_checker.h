/*
 * I1（BitLocker自动识别，2026-11-06小树裁决）：查询一个盘符是否被
 * BitLocker管理、是否锁定、是否加密——给GUI磁盘卡片画锁形角标、判断
 * 要不要弹"解锁加密磁盘"用。
 *
 * imperative shell：Windows专属（用到PowerShell子进程查WMI），只在
 * Windows构建里编译。
 *
 * 实现方式和查询出的真实字段行为，完全基于I0真机探测的结果（见
 * docs/开发日志.md对应记录），不是照着MSDN文档直接假设写的：
 *   - Win32_EncryptableVolume对完全没加密过的卷也有实例，不是"只有
 *     加密过才出现"。
 *   - 卷被锁定之后，GetProtectionStatus()会变成"未知"（不再是"已
 *     保护"），GetConversionStatus()直接调用失败、返回垃圾值——这两个
 *     方法只应该在GetLockStatus()确认"未锁定"之后才去调用，锁定态下
 *     这两个字段在bitlocker_status_t里统一标成"未知"（-1），不传递
 *     真机验证过的垃圾值出去。
 */
#ifndef ENGINE_SHELL_BITLOCKER_STATUS_CHECKER_H
#define ENGINE_SHELL_BITLOCKER_STATUS_CHECKER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	/* 1 = 这个盘符在Win32_EncryptableVolume里有实例（受FVE管理，
	 *     不代表已经加密）；0 = 没有实例（这个盘从来没碰过BitLocker
	 *     管理，比如网络盘/只读光驱/虚拟机某些特殊卷，见disk_enumerator
	 *     已经过滤掉的Network/CD-ROM类型——这个检查只对Fixed/Removable
	 *     类型的盘有意义） */
	int is_fve_manageable;

	/* 只有is_fve_manageable为1才有意义：1=已锁定（读不出内容，需要
	 * 解锁），0=未锁定（可以正常访问，不管是否加密） */
	int is_locked;

	/* 只有is_fve_manageable为1且is_locked为0才有真实值：1=已启用保护
	 * （已加密），0=未保护（未加密）；is_locked为1时固定是-1（真机
	 * 验证过：锁定态下这个查询本身就不可信，不是"懒得查"） */
	int is_protected;

	/* 同is_protected的可用性条件：0-100表示已加密百分比；-1表示
	 * 不适用/未知（未加密，或者已锁定查不到） */
	int encryption_percentage;
}
bitlocker_status_t;

/* 查询单个盘符的BitLocker状态。
 * 返回1成功（哪怕这个盘根本没加密过，只要PowerShell查询本身正常跑完
 * 就算成功，out里相应字段会标成"未保护/0%"或者"不适用"，不是-1这个
 * "查询失败"的语义）；返回-1表示查询本身失败（PowerShell启动失败、
 * 输出解析不出预期格式等），out内容未定义。
 */
int bitlocker_status_checker_query(
     char drive_letter,
     bitlocker_status_t *out );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SHELL_BITLOCKER_STATUS_CHECKER_H */
