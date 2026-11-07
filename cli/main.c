/*
 * imperative shell：命令行入口。
 * 负责所有 I/O（打开卷、打印结果），具体判断逻辑全部委托给 engine/core。
 * 这是 Walking Skeleton 的"界面"，以后会被 Windows 原生 UI 替换，
 * 替换时 engine/core 和 engine/shell 完全不用动。
 *
 * 用法：recoverctl scan <ntfs 镜像/卷路径>
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libfsntfs.h>
#include <libvshadow.h>

#include "../engine/core/undelete_scanner.h"
#include "../engine/shell/aligned_device_reader.h"
#include "../engine/shell/exfat_reader.h"
#include "../engine/shell/fat_reader.h"
#include "../engine/shell/file_writer.h"
#include "../engine/shell/ntfs_bfio_adapter.h"
#include "../engine/shell/partition_extractor.h"
#include "../engine/shell/partition_reader.h"
#include "../engine/shell/path_resolver.h"
#include "../engine/shell/photorec_invoker.h"
#include "../engine/shell/vshadow_bfio_adapter.h"
#include "../engine/shell/bitlocker_volume_opener.h"
#include "../engine/shell/bitlocker_credential_source.h"
#include "../engine/shell/bitlocker_unlock.h"
#include "../engine/shell/bitlocker_bfio_adapter.h"
#include "../engine/shell/secure_memory.h"
#include "../engine/core/photorec_report_parser.h"
#include "../engine/core/bitlocker_key_protector_type_name.h"

#ifdef _WIN32
#include <windows.h>
#include "../engine/shell/disk_enumerator.h"
#include "../engine/shell/bitlocker_status_checker.h"
#include "../engine/shell/volume_disk_extents.h"
#endif

/* 前向声明：NTFS 分区偏移扫描（拷贝到临时文件再处理那条路径）要调用
 * 这两个函数，但它们的正式定义在文件后面 */
static int command_scan_ntfs( const char *volume_path );
static int command_recover_ntfs( const char *volume_path, const char *mft_index_str, const char *output_path );
static int command_scan_partition_exfat( const char *device_path, const char *offset_str, long offset );
static int command_recover_partition_exfat( const char *device_path, long offset, const char *index_str, const char *output_path );

/* FAT32/exFAT 的候选项如果是在子目录里找到的（directory_path 非空），
 * 打印成"路径=SUBDIR\xxx"，和 NTFS 路径重建那边的展示风格保持一致；
 * 根目录下的候选项（directory_path 为空）保持原来的"文件名=xxx"，不变。 */
static void print_fat_style_candidate_name( const char *directory_path, const char *name )
{
	if( directory_path[ 0 ] != '\0' )
	{
		printf( "路径=%s\\%s", directory_path, name );
	}
	else
	{
		printf( "文件名=%s", name );
	}
}

/* P5 坏道保护（2026-11-05 小树裁决第17步）：恢复完成后如果有跳过的坏道，
 * 打印摘要给用户看，而不是让"跳过了多少、跳过了哪里"这些信息随着
 * bad_sector_log_t 在函数退出时一起被 free 掉、用户完全不知情。 */
static void print_bad_sector_summary( const bad_sector_log_t *log )
{
	int i;

	if( log == NULL || log->count == 0 )
	{
		return;
	}

	printf( "警告：恢复过程中跳过了 %d 处读取失败的区域（坏道），共 %llu 字节，"
	        "这些位置在输出文件里已用 0 填充。建议对该磁盘做物理健康检测（如 SMART 自检），"
	        "避免问题恶化：\n",
	        log->count, (unsigned long long) bad_sector_log_total_bytes_skipped( log ) );
	for( i = 0; i < log->count; i++ )
	{
		printf( "    偏移=%llu  长度=%llu字节\n",
		        (unsigned long long) log->records[ i ].offset,
		        (unsigned long long) log->records[ i ].length );
	}
}

/* 2026-11-05 第23步小树裁决：受控真机实验（在 E 盘故意制造"MFT 记录槽位
 * 被新文件复用"的场景）证实 is_allocated() 判断本身可靠——槽位真被复用时，
 * 候选项会正确从扫描结果里消失，不是 bug。但即使槽位没被复用、候选项
 * 合法出现在列表里，它记录的数据簇也可能早就被别的、完全无关的文件
 * 复用过了——NTFS/FAT32/exFAT 删除文件时都不会清空簇号这类"位置"信息
 * （本项目反复验证过的通用规律：元数据比数据活得久），但簇里现在装的
 * 是不是还是原来那个文件的内容，没有低成本的办法判断（真要判断需要
 * 对照卷的簇分配位图，卷影副本场景还要对照"拍快照那一刻"的位图状态，
 * 复杂度明显更高，收益也只是"部分情况下提前预警"，不是"能解决"）。
 * 和 FAT32 碎片化恢复那条先例一样的处理方式：如实告知这是结构性限制，
 * 不伪造检测不出来的能力。这条提示是通用的，不分文件系统、不分直接
 * 扫活卷还是扫卷影副本——"元数据先死数据后死"这条规律在所有这些路径
 * 上都成立。
 *
 * 只在扫描结果汇总处打印一次，不挂在每个候选项后面刷屏——噪音盖过
 * 有用信息会导致用户审美疲劳直接忽略。 */
static void print_data_freshness_disclaimer( void )
{
	printf( "\n提示：本工具依据文件系统元数据判断文件是否可恢复，无法检测目标数据\n"
	        "所在的磁盘位置是否已被新数据覆盖；删除时间越久、期间磁盘写入越多，\n"
	        "恢复内容仍然完整的可能性越低，建议优先处理删除时间较近的候选项。\n" );
}

/* D4（P4 PhotoRec兜底雕刻引擎，2026-11-05 第28步排期）：和
 * print_data_freshness_disclaimer 不是同一个问题——那个提示讲的是"文件
 * 系统元数据判断出来的候选项，数据本身可能被覆盖"；这里讲的是信号雕刻
 * 这种方式本身固有的局限（不依赖、也拿不到文件系统元数据），两者原因
 * 不同，不能合并成一条提示。 */
static void print_carve_disclaimer( void )
{
	printf( "\n提示：信号雕刻是在文件系统结构已经解析失败时的兜底手段，只靠文件内容\n"
	        "本身的特征字节判断，天然拿不到原始文件名/目录路径（文件名是雕刻工具按\n"
	        "顺序编号生成的），也可能因为内容边界判断不准确导致文件不完整/误报；\n"
	        "能用 scan/recover 正常解析出文件名和路径的场景，优先用那条路径。\n" );
}

static void print_usage( const char *program_name )
{
	fprintf( stderr, "用法: %s scan <ntfs/fat32 镜像/卷路径>\n", program_name );
	fprintf( stderr, "      %s recover <镜像/卷路径> <索引> <输出路径>\n", program_name );
	fprintf( stderr, "      %s partitions <磁盘镜像/设备路径>   (绕过系统卷管理，直接解析分区表)\n", program_name );
	fprintf( stderr, "      %s scan-partition <磁盘镜像/设备路径> <分区偏移> [分区大小]   (对某个分区直接扫描；FAT32/exFAT 不需要分区大小，NTFS 需要，因为要先拷出来)\n", program_name );
	fprintf( stderr, "      %s recover-partition <磁盘镜像/设备路径> <分区偏移> <分区大小(FAT32可填0)> <候选项序号或MFT索引> <输出路径>\n", program_name );
#ifdef _WIN32
	fprintf( stderr, "      %s list   (列出本机的磁盘/分区)\n", program_name );
	fprintf( stderr, "      %s list-shadows <盘符>   (列出这个盘现有的历史卷影副本，比如系统还原点自动创建的快照)\n", program_name );
	fprintf( stderr, "      %s disk-encryption-status <盘符>   (诊断：查询这个盘符是否受BitLocker管理/是否锁定/加密进度，以及它在物理盘上的编号+字节偏移)\n", program_name );
	fprintf( stderr, "      %s scan-shadow <卷影副本设备路径>   (像扫描普通NTFS卷一样扫描某个历史快照，设备路径从 list-shadows 的输出里拿)\n", program_name );
	fprintf( stderr, "      %s recover-shadow <卷影副本设备路径> <候选项序号> <输出路径>\n", program_name );
#endif
	fprintf( stderr, "      %s list-shadows-offline <磁盘/卷镜像路径>   (离线解析一个镜像文件里记录的历史卷影副本，不需要目标是当前活系统，跨平台)\n", program_name );
	fprintf( stderr, "      %s scan-shadow-offline <磁盘/卷镜像路径> <store序号>   (扫描镜像里某个历史卷影副本，store序号从 list-shadows-offline 的输出里拿)\n", program_name );
	fprintf( stderr, "      %s recover-shadow-offline <磁盘/卷镜像路径> <store序号> <候选项序号> <输出路径>\n", program_name );
	fprintf( stderr, "      %s recover-carve <PhotoRec可执行文件路径> <设备/镜像路径> <输出目录>   (信号雕刻兜底手段，整个目标当一块盘处理，文件系统结构损坏到scan/recover解析不出来时用)\n", program_name );
	fprintf( stderr, "      %s recover-carve-partition <PhotoRec可执行文件路径> <设备路径> <分区偏移> <分区大小字节数> <输出目录>   (先按我们自己解析出的分区边界切出这一段，再做信号雕刻，不让PhotoRec自己重新探测分区表)\n", program_name );
	fprintf( stderr, "      %s list-bitlocker-key-protectors <磁盘/卷路径> <偏移>   (诊断：列出这个卷上有哪些BitLocker密钥保护器，不碰任何凭据)\n", program_name );
	fprintf( stderr, "      %s scan-bitlocker <磁盘/卷路径> <偏移> <凭据类型:password|recovery-key|startup-key|fvek> <凭据来源:file|cli|interactive> [<凭据文件路径或凭据内容本身>]\n", program_name );
	fprintf( stderr, "      %s recover-bitlocker <磁盘/卷路径> <偏移> <凭据类型> <凭据来源:file|cli|interactive> [<凭据文件路径或凭据内容本身>] <MFT索引> <输出路径>\n", program_name );
	fprintf( stderr, "          （凭据来源=interactive时不要传凭据内容参数——会现场从终端不回显地提示输入，"
	                 "不支持startup-key类型；来源=cli时凭据会短暂出现在shell历史/进程列表里，"
	                 "不敏感场景外不建议使用）\n" );
	fprintf( stderr, "          （锁定的BitLocker卷不能用卷盘符路径直接读取，必须传物理盘路径（如\\\\.\\PhysicalDrive3）"
	                 "+该分区在物理盘上的字节偏移（用 `%s partitions <磁盘>` 查）；未锁定/未加密卷可以继续传卷路径+偏移0）\n", program_name );
	fprintf( stderr, "          凭据来源=file时上一个参数是文件路径；=cli时是凭据内容本身（注意：这种方式凭据会短暂出现在shell历史/进程列表里，本工具用完会尽快原地覆写，但无法消除“曾经出现过”这个事实；startup-key类型只支持file来源）\n" );
}

static int command_partitions_gpt( const char *device_path )
{
	gpt_partition_scan_result_t result;
	int i, j;

	memset( &result, 0, sizeof( result ) );

	if( partition_reader_scan_gpt( device_path, &result ) != 1 )
	{
		return EXIT_FAILURE;
	}

	printf( "分区表类型: GPT\n" );
	if( result.count == 0 )
	{
		printf( "没有找到任何分区。\n" );
	}
	else
	{
		printf( "找到 %d 个分区（直接解析分区表得到，不依赖操作系统是否已挂载）：\n", result.count );
		for( i = 0; i < result.count; i++ )
		{
			double size_mb = (double) result.partitions[ i ].size / ( 1024.0 * 1024.0 );
			printf( "  [%d] 偏移=%llu  大小=%.1fMB  类型GUID=",
			        i,
			        (unsigned long long) result.partitions[ i ].offset,
			        size_mb );
			for( j = 0; j < 16; j++ )
			{
				printf( "%02x", result.partitions[ i ].type_guid[ j ] );
			}
			printf( "\n" );
		}
	}

	gpt_partition_scan_result_free( &result );
	return EXIT_SUCCESS;
}

static int command_partitions_mbr( const char *device_path )
{
	mbr_partition_scan_result_t result;
	int i;

	memset( &result, 0, sizeof( result ) );

	if( partition_reader_scan_mbr( device_path, &result ) != 1 )
	{
		return EXIT_FAILURE;
	}

	printf( "分区表类型: MBR\n" );
	if( result.count == 0 )
	{
		printf( "没有找到任何分区。\n" );
	}
	else
	{
		printf( "找到 %d 个分区（直接解析分区表得到，不依赖操作系统是否已挂载）：\n", result.count );
		for( i = 0; i < result.count; i++ )
		{
			double size_mb = (double) result.partitions[ i ].size / ( 1024.0 * 1024.0 );
			printf( "  [%d] 偏移=%llu  大小=%.1fMB  类型代码=0x%02x\n",
			        i,
			        (unsigned long long) result.partitions[ i ].offset,
			        size_mb,
			        result.partitions[ i ].type );
		}
	}

	mbr_partition_scan_result_free( &result );
	return EXIT_SUCCESS;
}

/* 把设备/镜像里从 offset 开始、size 字节的内容拷到一个临时文件，
 * 返回 1 成功、-1 失败。调用方用完要自己删掉这个临时文件。
 *
 * 这是 NTFS 分区偏移扫描的权宜之计：libfsntfs 走"从任意偏移直接读"
 * 这条路需要 libbfio 的 file range 接口，但那是 libfsntfs 内部私有实现，
 * 没有对外公开头文件，要用上它得重新编译整个 libfsntfs 去链接一个独立
 * 版本的 libbfio——这会改动我们已经用真实 Windows 设备验证过的 NTFS
 * 核心依赖，风险明显高于今晚其他改动（之前全部是纯增量，没碰过已验证
 * 稳定的东西）。所以先用"拷一份出来再用现成的 NTFS 读取逻辑"这个更
 * 保守的办法，代价是大分区会多花时间/磁盘空间去拷贝——这个限制值得
 * 以后专门花时间换成真正的 range 读取，不是本次没注意到。
 *
 * 2026-11-06小树裁决：实际的"打开+定位+循环拷贝"实现和"系统临时文件
 * 路径"两个函数已经抽到`engine/shell/partition_extractor.{h,c}`
 * （`partition_extractor_extract_to_temp_file`/
 * `partition_extractor_get_temp_file_path`），供`recover-carve-partition`
 * 这个CLI命令和GUI的`CarveDialog`"按分区范围雕刻"分支共享同一份实现，
 * 不是各自维护一份。
 */

static int command_scan_partition_ntfs_via_copy( const char *device_path, long offset, uint64_t size )
{
	int return_code;
	char temp_path[ 1024 ];
	bad_sector_log_t bad_log;

	memset( &bad_log, 0, sizeof( bad_log ) );

	if( size == 0 )
	{
		fprintf( stderr, "不是 FAT32，也没有提供分区大小，无法尝试按 NTFS 处理（NTFS 偏移扫描需要知道分区大小才能先拷出来）\n" );
		fprintf( stderr, "用法: recoverctl scan-partition <设备> <偏移> <分区大小字节数>\n" );
		return EXIT_FAILURE;
	}

	if( partition_extractor_get_temp_file_path( temp_path, sizeof( temp_path ) ) != 1 )
	{
		fprintf( stderr, "无法确定临时文件路径\n" );
		return EXIT_FAILURE;
	}

	printf( "不是 FAT32，按 NTFS 尝试（先把这段分区拷到临时文件 %s，再用现成的 NTFS 逻辑处理，大分区会慢一些）...\n",
	        temp_path );

	if( partition_extractor_extract_to_temp_file( device_path, offset, size, &bad_log, temp_path ) != 1 )
	{
		fprintf( stderr, "拷贝分区内容失败\n" );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}
	print_bad_sector_summary( &bad_log );
	bad_sector_log_free( &bad_log );

	return_code = command_scan_ntfs( temp_path );
	remove( temp_path );

	/* command_scan_ntfs 自己打印的"用 recoverctl recover <临时文件> ..."提示是
	 * 误导性的（临时文件这时候已经被删掉了），这里补一条指向正确命令的提示 */
	if( return_code == EXIT_SUCCESS )
	{
		printf( "\n（上面提示里的临时文件名不用管，已经删掉了；要恢复某个文件，"
		        "用 `recoverctl recover-partition %s %ld %llu <MFT 索引> <输出路径>`）\n",
		        device_path, offset, (unsigned long long) size );
	}
	return return_code;
}

static int command_scan_partition( const char *device_path, const char *offset_str, const char *size_str )
{
	fat_reader_t reader;
	fat_undelete_scan_result_t result;
	bad_sector_log_t bad_log;
	long offset;
	uint64_t size = 0;
	char *end_pointer = NULL;
	int i;

	memset( &bad_log, 0, sizeof( bad_log ) );

	offset = strtol( offset_str, &end_pointer, 10 );
	if( end_pointer == offset_str || *end_pointer != '\0' )
	{
		fprintf( stderr, "无效的偏移: %s\n", offset_str );
		return EXIT_FAILURE;
	}
	if( size_str != NULL )
	{
		size = (uint64_t) strtoull( size_str, &end_pointer, 10 );
		if( end_pointer == size_str || *end_pointer != '\0' )
		{
			fprintf( stderr, "无效的分区大小: %s\n", size_str );
			return EXIT_FAILURE;
		}
	}

	if( fat_reader_open_at_offset( device_path, offset, &reader ) != 1 )
	{
		exfat_reader_t exfat_probe_reader;

		if( exfat_reader_open_at_offset( device_path, offset, &exfat_probe_reader ) == 1 )
		{
			exfat_reader_close( &exfat_probe_reader );
			return command_scan_partition_exfat( device_path, offset_str, offset );
		}
		return command_scan_partition_ntfs_via_copy( device_path, offset, size );
	}

	printf( "正在扫描 (FAT32, 偏移=%ld): %s\n", offset, device_path );

	memset( &result, 0, sizeof( result ) );
	if( fat_reader_scan_directory_tree( &reader, &bad_log, NULL, NULL, &result ) != 1 )
	{
		fprintf( stderr, "扫描过程中发生错误\n" );
		fat_reader_close( &reader );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	if( result.count == 0 )
	{
		printf( "没有找到可恢复的已删除文件。\n" );
	}
	else
	{
		printf( "找到 %d 个已删除但可能可以恢复的文件：\n", result.count );
		for( i = 0; i < result.count; i++ )
		{
			printf( "  [%d] 起始簇=%u  大小=%u字节  ",
			        i,
			        result.candidates[ i ].first_cluster,
			        result.candidates[ i ].file_size );
			print_fat_style_candidate_name( result.candidates[ i ].directory_path, result.candidates[ i ].name );
			printf( "\n" );
			if( (uint64_t) result.candidates[ i ].file_size
			  > (uint64_t) reader.boot_sector.sectors_per_cluster * reader.boot_sector.bytes_per_sector )
			{
				/* 2026-11-05 小树裁决第16步：FAT32 删除文件时会把 FAT 链上
				 * 所有簇的表项清零（第15步真机验证确认），删除之后没有任何
				 * 残留信号能判断这个文件原本是连续存储还是碎片化存储——
				 * 不是检测不出来，是信息已经不在了。这条提示对所有跨多簇
				 * 的候选项统一展示，不是针对某个"疑似"的文件，因为软件
				 * 层面做不到"挑出疑似的" */
				printf( "      提示：此文件跨多个簇，如果原本是碎片化（非连续）存储，"
				        "恢复出的内容可能不完整或错误——这是 FAT32 删除机制本身的限制"
				        "（删除会清空 FAT 链信息），软件无法判断具体某个文件是否命中\n" );
			}
		}
		printf( "\n用 `recoverctl recover-partition %s %s <候选项序号> <输出路径>` 恢复某一个文件。\n",
		        device_path, offset_str );
		print_data_freshness_disclaimer();
	}

	print_bad_sector_summary( &bad_log );
	fat_undelete_scan_result_free( &result );
	fat_reader_close( &reader );
	bad_sector_log_free( &bad_log );
	return EXIT_SUCCESS;
}

/* 和 command_scan_partition 里 FAT32 那部分结构完全一致，只是换成 exFAT 的
 * reader——两条路径都是"自己写的读写逻辑，加一个 base_offset 字段就能直接
 * 支持非零偏移"，不需要像 NTFS 那样走"拷贝到临时文件"的保守方案 */
static int command_scan_partition_exfat( const char *device_path, const char *offset_str, long offset )
{
	exfat_reader_t reader;
	exfat_undelete_scan_result_t result;
	bad_sector_log_t bad_log;
	int i;

	memset( &bad_log, 0, sizeof( bad_log ) );

	if( exfat_reader_open_at_offset( device_path, offset, &reader ) != 1 )
	{
		fprintf( stderr, "无法打开 exFAT 卷（偏移=%ld）: %s\n", offset, device_path );
		return EXIT_FAILURE;
	}

	printf( "正在扫描 (exFAT, 偏移=%ld): %s\n", offset, device_path );

	memset( &result, 0, sizeof( result ) );
	if( exfat_reader_scan_directory_tree( &reader, &bad_log, NULL, NULL, &result ) != 1 )
	{
		fprintf( stderr, "扫描过程中发生错误\n" );
		exfat_reader_close( &reader );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	if( result.count == 0 )
	{
		printf( "没有找到可恢复的已删除文件。\n" );
	}
	else
	{
		printf( "找到 %d 个已删除但可能可以恢复的文件：\n", result.count );
		for( i = 0; i < result.count; i++ )
		{
			printf( "  [%d] 起始簇=%u  大小=%llu字节  存储=%s  ",
			        i,
			        result.candidates[ i ].first_cluster,
			        (unsigned long long) result.candidates[ i ].file_size,
			        result.candidates[ i ].no_fat_chain ? "连续" : "碎片化(跟随FAT链恢复)" );
			print_fat_style_candidate_name( result.candidates[ i ].directory_path, result.candidates[ i ].name );
			printf( "\n" );
		}
		printf( "\n用 `recoverctl recover-partition %s %s <候选项序号> <输出路径>` 恢复某一个文件。\n",
		        device_path, offset_str );
		print_data_freshness_disclaimer();
	}

	print_bad_sector_summary( &bad_log );
	exfat_undelete_scan_result_free( &result );
	exfat_reader_close( &reader );
	bad_sector_log_free( &bad_log );
	return EXIT_SUCCESS;
}

static int command_recover_partition_ntfs_via_copy( const char *device_path, long offset, uint64_t size,
                                                      const char *mft_index_str, const char *output_path )
{
	int return_code;
	char temp_path[ 1024 ];
	bad_sector_log_t bad_log;

	memset( &bad_log, 0, sizeof( bad_log ) );

	if( size == 0 )
	{
		fprintf( stderr, "不是 FAT32，也没有提供分区大小，无法尝试按 NTFS 处理\n" );
		return EXIT_FAILURE;
	}

	if( partition_extractor_get_temp_file_path( temp_path, sizeof( temp_path ) ) != 1 )
	{
		fprintf( stderr, "无法确定临时文件路径\n" );
		return EXIT_FAILURE;
	}

	if( partition_extractor_extract_to_temp_file( device_path, offset, size, &bad_log, temp_path ) != 1 )
	{
		fprintf( stderr, "拷贝分区内容失败\n" );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}
	print_bad_sector_summary( &bad_log );
	bad_sector_log_free( &bad_log );

	return_code = command_recover_ntfs( temp_path, mft_index_str, output_path );
	remove( temp_path );
	return return_code;
}

/* index_str 的含义取决于实际识别出来的文件系统：FAT32 是 scan-partition
 * 打印出来的候选项序号（不是起始簇号——真实磁盘上簇号会被反复复用，
 * 按簇号定位可能静默恢复成错误的那条记录，详见 command_recover_fat 的注释），
 * NTFS 是 MFT 索引。两种情况下这个参数在命令行位置上的意义不一样，
 * 但调用方（用户）只需要照着 scan-partition/recover-partition 自己打印出来
 * 的那条提示命令抄就行，不需要关心这个差异。 */
static int command_recover_partition( const char *device_path, const char *offset_str, const char *size_str,
                                       const char *index_str, const char *output_path )
{
	fat_reader_t reader;
	long offset;
	uint64_t size = 0;
	long candidate_index;
	char *end_pointer = NULL;
	fat_undelete_scan_result_t result;
	bad_sector_log_t bad_log;
	int success = 0;
	int scanned_count;

	memset( &bad_log, 0, sizeof( bad_log ) );

	offset = strtol( offset_str, &end_pointer, 10 );
	if( end_pointer == offset_str || *end_pointer != '\0' )
	{
		fprintf( stderr, "无效的偏移: %s\n", offset_str );
		return EXIT_FAILURE;
	}
	size = (uint64_t) strtoull( size_str, &end_pointer, 10 );
	if( end_pointer == size_str || *end_pointer != '\0' )
	{
		fprintf( stderr, "无效的分区大小: %s\n", size_str );
		return EXIT_FAILURE;
	}
	/* 用候选项序号而不是起始簇号定位，原因见 command_recover_fat 里的注释：
	 * 真实磁盘上簇号会被反复复用，按簇号查找可能静默恢复成错误的那一条。 */
	candidate_index = strtol( index_str, &end_pointer, 10 );
	if( end_pointer == index_str || *end_pointer != '\0' || candidate_index < 0 )
	{
		fprintf( stderr, "无效的候选项序号: %s\n", index_str );
		return EXIT_FAILURE;
	}

	if( fat_reader_open_at_offset( device_path, offset, &reader ) != 1 )
	{
		exfat_reader_t exfat_probe_reader;

		if( exfat_reader_open_at_offset( device_path, offset, &exfat_probe_reader ) == 1 )
		{
			exfat_reader_close( &exfat_probe_reader );
			return command_recover_partition_exfat( device_path, offset, index_str, output_path );
		}
		return command_recover_partition_ntfs_via_copy( device_path, offset, size, index_str, output_path );
	}

	memset( &result, 0, sizeof( result ) );
	if( fat_reader_scan_directory_tree( &reader, &bad_log, NULL, NULL, &result ) != 1 )
	{
		fprintf( stderr, "扫描失败\n" );
		fat_reader_close( &reader );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	scanned_count = result.count;
	if( candidate_index < scanned_count )
	{
		if( fat_reader_recover_file( &reader, result.candidates[ candidate_index ].first_cluster,
		                              result.candidates[ candidate_index ].file_size, &bad_log, output_path ) != 1 )
		{
			fprintf( stderr, "写出内容失败: %s\n", output_path );
			fat_undelete_scan_result_free( &result );
			fat_reader_close( &reader );
			bad_sector_log_free( &bad_log );
			return EXIT_FAILURE;
		}
		printf( "已恢复候选项 [%ld]（起始簇=%u）-> %s\n",
		        candidate_index, result.candidates[ candidate_index ].first_cluster, output_path );
		success = 1;
	}

	print_bad_sector_summary( &bad_log );
	fat_undelete_scan_result_free( &result );
	fat_reader_close( &reader );
	bad_sector_log_free( &bad_log );

	if( !success )
	{
		fprintf( stderr, "找不到候选项序号 %ld（当前扫描结果只有 %d 个候选项，重新跑一次 scan-partition 确认编号）\n",
		         candidate_index, scanned_count );
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

/* 和 command_recover_partition 里 FAT32 那部分结构完全一致，换成 exFAT 的 reader */
static int command_recover_partition_exfat( const char *device_path, long offset, const char *index_str, const char *output_path )
{
	exfat_reader_t reader;
	exfat_undelete_scan_result_t result;
	bad_sector_log_t bad_log;
	long candidate_index;
	char *end_pointer = NULL;
	int success = 0;
	int scanned_count;

	memset( &bad_log, 0, sizeof( bad_log ) );

	candidate_index = strtol( index_str, &end_pointer, 10 );
	if( end_pointer == index_str || *end_pointer != '\0' || candidate_index < 0 )
	{
		fprintf( stderr, "无效的候选项序号: %s\n", index_str );
		return EXIT_FAILURE;
	}

	if( exfat_reader_open_at_offset( device_path, offset, &reader ) != 1 )
	{
		fprintf( stderr, "无法打开 exFAT 卷（偏移=%ld）: %s\n", offset, device_path );
		return EXIT_FAILURE;
	}

	memset( &result, 0, sizeof( result ) );
	if( exfat_reader_scan_directory_tree( &reader, &bad_log, NULL, NULL, &result ) != 1 )
	{
		fprintf( stderr, "扫描失败\n" );
		exfat_reader_close( &reader );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	scanned_count = result.count;
	if( candidate_index < scanned_count )
	{
		if( exfat_reader_recover_file( &reader, result.candidates[ candidate_index ].first_cluster,
		                                result.candidates[ candidate_index ].file_size,
		                                result.candidates[ candidate_index ].no_fat_chain, &bad_log, output_path ) != 1 )
		{
			fprintf( stderr, "写出内容失败: %s\n", output_path );
			exfat_undelete_scan_result_free( &result );
			exfat_reader_close( &reader );
			bad_sector_log_free( &bad_log );
			return EXIT_FAILURE;
		}
		printf( "已恢复候选项 [%ld]（起始簇=%u）-> %s\n",
		        candidate_index, result.candidates[ candidate_index ].first_cluster, output_path );
		success = 1;
	}

	print_bad_sector_summary( &bad_log );
	exfat_undelete_scan_result_free( &result );
	exfat_reader_close( &reader );
	bad_sector_log_free( &bad_log );

	if( !success )
	{
		fprintf( stderr, "找不到候选项序号 %ld（当前扫描结果只有 %d 个候选项，重新跑一次 scan-partition 确认编号）\n",
		         candidate_index, scanned_count );
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

/* 先试 GPT 再试 MBR——和 scan 的 NTFS/FAT32 判断是同一个"都试一遍"思路 */
static int command_partitions( const char *device_path )
{
	if( command_partitions_gpt( device_path ) == EXIT_SUCCESS )
	{
		return EXIT_SUCCESS;
	}
	if( command_partitions_mbr( device_path ) == EXIT_SUCCESS )
	{
		return EXIT_SUCCESS;
	}
	fprintf( stderr, "无法识别分区表格式（已尝试 GPT、MBR）: %s\n", device_path );
	return EXIT_FAILURE;
}

#ifdef _WIN32
static int command_list( void )
{
	disk_enum_result_t result;
	physical_disk_enum_result_t physical_result;
	int i;

	memset( &result, 0, sizeof( result ) );
	memset( &physical_result, 0, sizeof( physical_result ) );

	if( disk_enumerator_list_physical_drives( &physical_result ) != 1 )
	{
		fprintf( stderr, "枚举物理磁盘失败\n" );
		return EXIT_FAILURE;
	}

	if( physical_result.count == 0 )
	{
		printf( "没有找到任何物理磁盘。\n" );
	}
	else
	{
		printf( "物理磁盘列表（用 `recoverctl scan <磁盘编号>` 可以直接扫描整块盘）：\n" );
		for( i = 0; i < physical_result.count; i++ )
		{
			const physical_disk_entry_t *entry = &physical_result.entries[ i ];
			double size_gb = (double) entry->size_bytes / ( 1024.0 * 1024.0 * 1024.0 );

			printf( "  磁盘 %d  容量=%.1fGB", entry->disk_number, size_gb );
			if( entry->is_removable == 1 )
			{
				printf( "  [可移动介质]" );
			}
			else if( entry->is_removable == 0 )
			{
				printf( "  [固定磁盘]" );
			}
			if( entry->model[ 0 ] != '\0' )
			{
				printf( "  型号=%s", entry->model );
			}
			printf( "\n" );
		}
	}

	physical_disk_enum_result_free( &physical_result );

	if( disk_enumerator_list_logical_drives( &result ) != 1 )
	{
		fprintf( stderr, "枚举磁盘分区失败\n" );
		return EXIT_FAILURE;
	}

	if( result.count == 0 )
	{
		printf( "\n没有找到任何分区。\n" );
	}
	else
	{
		printf( "\n分区（盘符）列表（用 `recoverctl scan <盘符>` 可以直接扫描某个分区）：\n" );
		for( i = 0; i < result.count; i++ )
		{
			const disk_entry_t *entry = &result.entries[ i ];
			double total_gb = (double) entry->total_bytes / ( 1024.0 * 1024.0 * 1024.0 );
			double free_gb  = (double) entry->free_bytes / ( 1024.0 * 1024.0 * 1024.0 );

			printf( "  %c:\\  [%s]", entry->drive_letter, entry->drive_type_name );
			if( entry->file_system[ 0 ] != '\0' )
			{
				printf( "  文件系统=%s", entry->file_system );
			}
			if( entry->volume_label[ 0 ] != '\0' )
			{
				printf( "  卷标=%s", entry->volume_label );
			}
			if( entry->total_bytes > 0 )
			{
				printf( "  容量=%.1fGB  可用=%.1fGB", total_gb, free_gb );
			}
			printf( "\n" );
		}
		printf( "\n例如：`recoverctl scan E:` 或 `recoverctl scan 1`，效果等同于手打完整设备路径。\n" );
	}

	disk_enum_result_free( &result );
	return EXIT_SUCCESS;
}

/* V2（2026-11-05 第22步，VSS最小版）：列出某个盘符现有的历史卷影副本
 * （Windows"系统保护"/卷影复制服务自动创建的快照，不是本工具创建的——
 * 程序自己临时创建一个新快照，对"文件已经删除才想起来找工具"这个场景
 * 价值有限，小树裁决第22步明确排除这个功能，只做"读取系统已有的快照"）。
 *
 * 实现方式选了跑 PowerShell 子进程（`_popen` + 管道分隔输出），没有直接
 * 写 COM/WMI 客户端代码——直接调 `IWbemServices` 要自己管理
 * BSTR/VARIANT/IEnumWbemClassObject 这一整套生命周期，换不来任何现在
 * 用得上的额外能力（第22步调研阶段已经验证过这条 PowerShell 命令本身
 * 在无人值守环境下完全跑得通，不卡住也不弹窗，直接复用验证过的命令，
 * 不是另起一套没验证过的实现）。 */
static int command_list_shadows( const char *drive_letter_arg )
{
	char drive_letter;
	char command[ 512 ];
	FILE *pipe;
	char line[ 512 ];
	int count = 0;

	if( drive_letter_arg[ 0 ] == '\0' )
	{
		fprintf( stderr, "无效的盘符: %s\n", drive_letter_arg );
		return EXIT_FAILURE;
	}
	drive_letter = (char) toupper( (unsigned char) drive_letter_arg[ 0 ] );

	snprintf( command, sizeof( command ),
	          "powershell -NoProfile -Command "
	          "\"$v = (Get-Volume -DriveLetter %c).Path; "
	          "Get-CimInstance Win32_ShadowCopy | Where-Object { $_.VolumeName -eq $v } | "
	          "ForEach-Object { $_.ID + '|' + $_.DeviceObject + '|' + $_.InstallDate }\"",
	          drive_letter );

	pipe = _popen( command, "r" );
	if( pipe == NULL )
	{
		fprintf( stderr, "无法启动 PowerShell 子进程查询卷影副本\n" );
		return EXIT_FAILURE;
	}

	printf( "%c: 盘上的历史卷影副本（Windows\"系统保护\"自动创建的快照，不是本工具创建的）：\n", drive_letter );

	while( fgets( line, sizeof( line ), pipe ) != NULL )
	{
		char *id_part;
		char *device_part;
		char *time_part;
		char *newline;

		newline = strchr( line, '\n' );
		if( newline != NULL )
		{
			*newline = '\0';
		}
		if( line[ 0 ] == '\0' )
		{
			continue;
		}

		id_part     = line;
		device_part = strchr( id_part, '|' );
		if( device_part == NULL )
		{
			continue;
		}
		*device_part = '\0';
		device_part++;

		time_part = strchr( device_part, '|' );
		if( time_part == NULL )
		{
			continue;
		}
		*time_part = '\0';
		time_part++;

		printf( "  [%d] 创建时间=%s  设备路径=%s\n", count, time_part, device_part );
		count++;
	}

	_pclose( pipe );

	if( count == 0 )
	{
		printf( "  （没有检测到可用的历史快照——这台电脑的这个盘可能没有开启\"系统保护\"/\n"
		        "   卷影复制服务，或者历史快照已经过期被系统自动清理。这不是本工具的问题，\n"
		        "   取决于这台电脑之前的系统设置。）\n" );
	}
	else
	{
		printf( "\n用 `recoverctl scan-shadow <设备路径>` 像扫描普通 NTFS 卷一样扫描某个快照，\n"
		        "`recoverctl recover-shadow <设备路径> <候选项序号> <输出路径>` 恢复里面的文件。\n" );
	}

	return EXIT_SUCCESS;
}

/* 把用户输入的简写（盘符 "E"/"E:"，或物理磁盘编号 "1"）转成完整设备路径。
 * 如果 arg 不属于这两种简写形式，原样拷贝（兼容手打完整路径/本地镜像文件路径的旧用法）。
 */
static void resolve_device_path(
     const char *arg,
     char *out_buf,
     size_t out_buf_size )
{
	size_t length = strlen( arg );
	size_t i;
	int is_all_digits = ( length > 0 );

	for( i = 0; i < length; i++ )
	{
		if( !isdigit( (unsigned char) arg[ i ] ) )
		{
			is_all_digits = 0;
			break;
		}
	}

	if( is_all_digits )
	{
		snprintf( out_buf, out_buf_size, "\\\\.\\PhysicalDrive%s", arg );
		return;
	}

	if( ( length == 1 && isalpha( (unsigned char) arg[ 0 ] ) )
	 || ( length == 2 && isalpha( (unsigned char) arg[ 0 ] ) && arg[ 1 ] == ':' ) )
	{
		snprintf( out_buf, out_buf_size, "\\\\.\\%c:", (char) toupper( (unsigned char) arg[ 0 ] ) );
		return;
	}

	strncpy( out_buf, arg, out_buf_size - 1 );
	out_buf[ out_buf_size - 1 ] = '\0';
}
#endif

/* N5（2026-11-05 小树裁决第20步）：固定重试 3 次，和项目里其它坏道容忍
 * 路径（FAT32/exFAT/NTFS分区拷贝）保持一致的默认值 */
#define NTFS_DIRECT_MAX_READ_RETRIES 3

/* 共享核心（2026-11-05 第25步，接入libvshadow离线解析时抽出来复用）：
 * 给定一个已经构造好的 bfio_handle（可能来自真实设备路径的
 * `ntfs_bfio_adapter`，也可能来自 libvshadow 解析出来的历史卷影副本的
 * `vshadow_bfio_adapter`），扫描并打印已删除候选项——扫描/打印逻辑本身
 * 完全不关心字节是从哪个来源读出来的，原来只有 command_scan_ntfs 一个
 * 调用方，现在卷影副本离线扫描也要用同一套逻辑，不重新写一遍。
 *
 * display_label 用于"正在扫描 (NTFS): %s"这行提示；recover_hint 是完整的
 * "<command> <args...>"字符串（不含开头的 `recoverctl `，调用方自己拼好
 * 命令名和定位参数），用于"用 recoverctl ... <MFT索引> <输出路径>"这行
 * 提示——不同调用方（真实卷/卷影副本在线/卷影副本离线镜像）各自传入
 * 适合自己场景的提示文案。
 *
 * 这个函数拥有 bfio_handle 的生命周期，返回前会释放它；bad_log 由调用方
 * 提供、不释放（调用方自己 bad_sector_log_free）。 */
static int command_scan_ntfs_via_bfio_handle(
     libbfio_handle_t *bfio_handle,
     bad_sector_log_t *bad_log,
     const char *display_label,
     const char *recover_hint )
{
	libfsntfs_volume_t *volume = NULL;
	libfsntfs_error_t *error   = NULL;
	undelete_scan_result_t result;
	int i;
	int return_code = EXIT_SUCCESS;

	memset( &result, 0, sizeof( result ) );

	if( libfsntfs_volume_initialize( &volume, &error ) != 1 )
	{
		fprintf( stderr, "无法初始化 volume 对象\n" );
		goto on_error;
	}

	if( libfsntfs_volume_open_file_io_handle( volume, bfio_handle, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		fprintf( stderr, "打开 NTFS 卷失败: %s\n", display_label );
		if( error != NULL )
		{
			libfsntfs_error_backtrace_fprint( error, stderr );
		}
		goto on_error;
	}

	printf( "正在扫描 (NTFS): %s\n", display_label );

	if( undelete_scanner_scan_volume( volume, &result, NULL, NULL, &error ) != 1 )
	{
		fprintf( stderr, "扫描过程中发生错误\n" );
		goto on_error;
	}

	if( result.count == 0 )
	{
		printf( "没有找到可恢复的已删除文件。\n" );
	}
	else
	{
		printf( "找到 %d 个已删除但可能可以恢复的文件：\n", result.count );
		for( i = 0; i < result.count; i++ )
		{
			char ancestor_path[ 1024 ];
			int path_is_complete = 0;

			/* 重建这个文件所在的目录路径（不含文件自己的名字），只有
			 * NTFS 才有这个能力——父目录索引来自 $FILE_NAME 属性，
			 * FAT32/exFAT 没有等价的结构，也就没有这一步 */
			if( path_resolver_reconstruct_ancestor_path(
			     volume, result.candidates[ i ].parent_mft_index,
			     ancestor_path, sizeof( ancestor_path ), &path_is_complete, &error ) != 1 )
			{
				ancestor_path[ 0 ] = '\0';
				path_is_complete = 0;
			}

			if( ancestor_path[ 0 ] != '\0' )
			{
				printf( "  [%d] MFT 索引=%llu  路径=%s\\%s%s\n",
				        i,
				        (unsigned long long) result.candidates[ i ].mft_entry_index,
				        ancestor_path,
				        result.candidates[ i ].name,
				        path_is_complete ? "" : "（上级目录链中途断开，路径可能不完整）" );
			}
			else
			{
				printf( "  [%d] MFT 索引=%llu  文件名=%s%s\n",
				        i,
				        (unsigned long long) result.candidates[ i ].mft_entry_index,
				        result.candidates[ i ].name,
				        path_is_complete ? "" : "（上级目录链中途断开，路径可能不完整）" );
			}
		}
		printf( "\n用 `recoverctl %s <MFT 索引> <输出路径>` 恢复某一个文件。\n", recover_hint );
		print_data_freshness_disclaimer();
	}

	print_bad_sector_summary( bad_log );
	undelete_scan_result_free( &result );
	libfsntfs_volume_close( volume, &error );
	libfsntfs_volume_free( &volume, &error );
	libbfio_handle_free( &bfio_handle, &error );
	return return_code;

on_error:
	if( error != NULL )
	{
		libfsntfs_error_free( &error );
	}
	undelete_scan_result_free( &result );
	if( volume != NULL )
	{
		libfsntfs_volume_free( &volume, NULL );
	}
	libbfio_handle_free( &bfio_handle, NULL );
	return EXIT_FAILURE;
}

static int command_scan_ntfs( const char *volume_path )
{
	libbfio_handle_t *bfio_handle = NULL;
	libfsntfs_error_t *error      = NULL;
	bad_sector_log_t bad_log;
	char recover_hint[ 1200 ];
	int return_code;

	memset( &bad_log, 0, sizeof( bad_log ) );

	/* P5 坏道保护（2026-11-05 小树裁决第20步，桶③）：不再用最简单的
	 * libfsntfs_volume_open(path)（内部用 libfsntfs 自带的 libbfio_file
	 * 实现读磁盘，插不进任何坏道容忍逻辑），改成自己构造一个自定义
	 * libbfio 句柄（engine/shell/ntfs_bfio_adapter.c），真正的读取走我们
	 * 自己的代码，能接入重试+跳过+坏道日志 */
	if( ntfs_bfio_adapter_initialize( volume_path, 0, NTFS_DIRECT_MAX_READ_RETRIES, &bad_log, &bfio_handle, &error ) != 1 )
	{
		fprintf( stderr, "无法构造自定义 I/O 句柄: %s\n", volume_path );
		if( error != NULL )
		{
			libfsntfs_error_free( &error );
		}
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	snprintf( recover_hint, sizeof( recover_hint ), "recover %s", volume_path );

	return_code = command_scan_ntfs_via_bfio_handle( bfio_handle, &bad_log, volume_path, recover_hint );
	bad_sector_log_free( &bad_log );
	return return_code;
}

static int command_scan_fat( const char *volume_path )
{
	fat_reader_t reader;
	fat_undelete_scan_result_t result;
	bad_sector_log_t bad_log;
	int i;

	memset( &result, 0, sizeof( result ) );
	memset( &bad_log, 0, sizeof( bad_log ) );

	if( fat_reader_open( volume_path, &reader ) != 1 )
	{
		return EXIT_FAILURE;
	}

	printf( "正在扫描 (FAT32): %s\n", volume_path );

	if( fat_reader_scan_directory_tree( &reader, &bad_log, NULL, NULL, &result ) != 1 )
	{
		fprintf( stderr, "扫描过程中发生错误\n" );
		fat_reader_close( &reader );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	if( result.count == 0 )
	{
		printf( "没有找到可恢复的已删除文件。\n" );
		printf( "（当前版本扫根目录+一层子目录、假设数据连续存储，更深层级子目录和碎片化文件暂不支持）\n" );
	}
	else
	{
		printf( "找到 %d 个已删除但可能可以恢复的文件：\n", result.count );
		for( i = 0; i < result.count; i++ )
		{
			printf( "  [%d] 起始簇=%u  大小=%u字节  ",
			        i,
			        result.candidates[ i ].first_cluster,
			        result.candidates[ i ].file_size );
			print_fat_style_candidate_name( result.candidates[ i ].directory_path, result.candidates[ i ].name );
			printf( "\n" );
			if( (uint64_t) result.candidates[ i ].file_size
			  > (uint64_t) reader.boot_sector.sectors_per_cluster * reader.boot_sector.bytes_per_sector )
			{
				/* 2026-11-05 小树裁决第16步：FAT32 删除文件时会把 FAT 链上
				 * 所有簇的表项清零（第15步真机验证确认），删除之后没有任何
				 * 残留信号能判断这个文件原本是连续存储还是碎片化存储——
				 * 不是检测不出来，是信息已经不在了。这条提示对所有跨多簇
				 * 的候选项统一展示，不是针对某个"疑似"的文件，因为软件
				 * 层面做不到"挑出疑似的" */
				printf( "      提示：此文件跨多个簇，如果原本是碎片化（非连续）存储，"
				        "恢复出的内容可能不完整或错误——这是 FAT32 删除机制本身的限制"
				        "（删除会清空 FAT 链信息），软件无法判断具体某个文件是否命中\n" );
			}
		}
		printf( "\n用 `recoverctl recover %s <候选项序号> <输出路径>` 恢复某一个文件。\n", volume_path );
		print_data_freshness_disclaimer();
	}

	print_bad_sector_summary( &bad_log );
	fat_undelete_scan_result_free( &result );
	fat_reader_close( &reader );
	bad_sector_log_free( &bad_log );
	return EXIT_SUCCESS;
}

static int command_scan_exfat( const char *volume_path )
{
	exfat_reader_t reader;
	exfat_undelete_scan_result_t result;
	bad_sector_log_t bad_log;
	int i;

	memset( &result, 0, sizeof( result ) );
	memset( &bad_log, 0, sizeof( bad_log ) );

	if( exfat_reader_open( volume_path, &reader ) != 1 )
	{
		return EXIT_FAILURE;
	}

	printf( "正在扫描 (exFAT): %s\n", volume_path );

	if( exfat_reader_scan_directory_tree( &reader, &bad_log, NULL, NULL, &result ) != 1 )
	{
		fprintf( stderr, "扫描过程中发生错误\n" );
		exfat_reader_close( &reader );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	if( result.count == 0 )
	{
		printf( "没有找到可恢复的已删除文件。\n" );
		printf( "（当前版本扫根目录+一层子目录、假设数据连续存储，更深层级子目录和超长文件名暂不支持）\n" );
	}
	else
	{
		printf( "找到 %d 个已删除但可能可以恢复的文件：\n", result.count );
		for( i = 0; i < result.count; i++ )
		{
			printf( "  [%d] 起始簇=%u  大小=%llu字节  存储=%s  ",
			        i,
			        result.candidates[ i ].first_cluster,
			        (unsigned long long) result.candidates[ i ].file_size,
			        result.candidates[ i ].no_fat_chain ? "连续" : "碎片化(跟随FAT链恢复)" );
			print_fat_style_candidate_name( result.candidates[ i ].directory_path, result.candidates[ i ].name );
			printf( "\n" );
		}
		printf( "\n用 `recoverctl recover %s <候选项序号> <输出路径>` 恢复某一个文件。\n", volume_path );
		print_data_freshness_disclaimer();
	}

	print_bad_sector_summary( &bad_log );
	exfat_undelete_scan_result_free( &result );
	exfat_reader_close( &reader );
	bad_sector_log_free( &bad_log );
	return EXIT_SUCCESS;
}

/* 先试 NTFS，打不开再试 FAT32，再打不开试 exFAT——目前只支持这三种文件
 * 系统，都打不开就是真的不认识这个卷，或者卷本身已经严重损坏 */
static int command_scan( const char *volume_path )
{
	libfsntfs_volume_t *probe_volume = NULL;
	libfsntfs_error_t *probe_error   = NULL;
	int is_ntfs;

	if( libfsntfs_volume_initialize( &probe_volume, &probe_error ) != 1 )
	{
		fprintf( stderr, "无法初始化 volume 对象\n" );
		return EXIT_FAILURE;
	}
	is_ntfs = ( libfsntfs_volume_open( probe_volume, volume_path, LIBFSNTFS_OPEN_READ, &probe_error ) == 1 );
	libfsntfs_volume_free( &probe_volume, NULL );
	if( probe_error != NULL )
	{
		libfsntfs_error_free( &probe_error );
	}

	if( is_ntfs )
	{
		return command_scan_ntfs( volume_path );
	}

	{
		fat_reader_t probe_reader;

		if( fat_reader_open( volume_path, &probe_reader ) == 1 )
		{
			fat_reader_close( &probe_reader );
			return command_scan_fat( volume_path );
		}
	}

	{
		exfat_reader_t probe_reader;

		if( exfat_reader_open( volume_path, &probe_reader ) == 1 )
		{
			exfat_reader_close( &probe_reader );
			return command_scan_exfat( volume_path );
		}
	}

	fprintf( stderr, "无法识别卷的文件系统（已尝试 NTFS、FAT32、exFAT）: %s\n", volume_path );
	return EXIT_FAILURE;
}

/* 共享核心（2026-11-05 第25步，和 command_scan_ntfs_via_bfio_handle 同样
 * 的理由抽出来复用）：给定一个已经构造好的 bfio_handle，按 MFT 索引恢复
 * 一个文件。display_label 只用于错误提示里标注"是哪个卷打不开"，这个
 * 函数拥有 bfio_handle 的生命周期，返回前会释放它；bad_log 由调用方
 * 提供、不释放。 */
static int command_recover_ntfs_via_bfio_handle(
     libbfio_handle_t *bfio_handle,
     bad_sector_log_t *bad_log,
     const char *display_label,
     uint64_t mft_entry_index,
     const char *output_path )
{
	libfsntfs_volume_t *volume         = NULL;
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;

	if( libfsntfs_volume_initialize( &volume, &error ) != 1 )
	{
		fprintf( stderr, "无法初始化 volume 对象\n" );
		goto on_error;
	}

	if( libfsntfs_volume_open_file_io_handle( volume, bfio_handle, LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		fprintf( stderr, "无法打开卷: %s\n", display_label );
		goto on_error;
	}

	if( libfsntfs_volume_get_file_entry_by_index( volume, mft_entry_index, &file_entry, &error ) != 1 )
	{
		fprintf( stderr, "找不到 MFT 索引 %llu\n", (unsigned long long) mft_entry_index );
		goto on_error;
	}

	if( file_writer_write_entry_content( file_entry, output_path, &error ) != 1 )
	{
		fprintf( stderr, "写出内容失败: %s\n", output_path );
		goto on_error;
	}

	printf( "已恢复 MFT 索引 %llu -> %s\n", (unsigned long long) mft_entry_index, output_path );
	print_bad_sector_summary( bad_log );

	libfsntfs_file_entry_free( &file_entry, &error );
	libfsntfs_volume_close( volume, &error );
	libfsntfs_volume_free( &volume, &error );
	libbfio_handle_free( &bfio_handle, &error );
	return EXIT_SUCCESS;

on_error:
	if( error != NULL )
	{
		libfsntfs_error_backtrace_fprint( error, stderr );
		libfsntfs_error_free( &error );
	}
	if( file_entry != NULL )
	{
		libfsntfs_file_entry_free( &file_entry, NULL );
	}
	if( volume != NULL )
	{
		libfsntfs_volume_free( &volume, NULL );
	}
	libbfio_handle_free( &bfio_handle, NULL );
	return EXIT_FAILURE;
}

static int command_recover_ntfs( const char *volume_path, const char *mft_index_str, const char *output_path )
{
	libbfio_handle_t *bfio_handle = NULL;
	libfsntfs_error_t *error      = NULL;
	bad_sector_log_t bad_log;
	uint64_t mft_entry_index = 0;
	char *end_pointer        = NULL;
	int return_code;

	memset( &bad_log, 0, sizeof( bad_log ) );

	mft_entry_index = (uint64_t) strtoull( mft_index_str, &end_pointer, 10 );

	if( end_pointer == mft_index_str || *end_pointer != '\0' )
	{
		fprintf( stderr, "无效的 MFT 索引: %s\n", mft_index_str );
		return EXIT_FAILURE;
	}

	/* 和 command_scan_ntfs 同样的理由：自定义 libbfio 句柄才能接入坏道
	 * 容忍，见 engine/shell/ntfs_bfio_adapter.c */
	if( ntfs_bfio_adapter_initialize( volume_path, 0, NTFS_DIRECT_MAX_READ_RETRIES, &bad_log, &bfio_handle, &error ) != 1 )
	{
		fprintf( stderr, "无法构造自定义 I/O 句柄: %s\n", volume_path );
		if( error != NULL )
		{
			libfsntfs_error_free( &error );
		}
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	return_code = command_recover_ntfs_via_bfio_handle( bfio_handle, &bad_log, volume_path, mft_entry_index, output_path );
	bad_sector_log_free( &bad_log );
	return return_code;
}

/* 2026-11-05 第25步（P3-libvshadow离线解析）：下面三个"离线"命令和
 * list-shadows/scan-shadow/recover-shadow（VSS最小可用版，第22步）
 * 不是同一回事——那三个命令查的是"当前这台正在运行的Windows系统上
 * 还留着的"卷影副本（只能在Windows上跑，走系统VSS在线API）；这三个
 * "离线"命令解析的是一个拿到手的磁盘/卷镜像文件里记录的历史卷影副本
 * 数据，不依赖目标盘是不是当前活系统、甚至不需要是Windows系统盘——
 * 跨平台都能跑（Mac/Windows都只是在读一个文件），命中数据恢复软件
 * "客户拿一块盘/镜像过来离线分析"这个核心场景，命名加 -offline 后缀
 * 和在线版本区分清楚，避免以后接手的人把两种本质不同的场景搞混。
 *
 * 技术路径：libvshadow 只负责解析镜像里的 VSS 控制结构、枚举出
 * store 列表；每个 store 对象本身已经是一套完整的"类文件"读接口
 * （`libvshadow_store_read_buffer_at_offset`/`get_size`），写时复制(COW)
 * 的地址重映射逻辑库内部已经做好了，不需要我们自己解析数据块列表
 * （详见 docs/开发日志.md 第24→25步调研纠偏记录）。`vshadow_bfio_adapter.c`
 * 把这套读接口包装成和 ntfs_bfio_adapter.c 同样形状的 libbfio 句柄，
 * 喂给 libfsntfs，后续扫描/恢复逻辑完整复用 `command_scan_ntfs_via_bfio_handle`/
 * `command_recover_ntfs_via_bfio_handle`，不重新写一遍。 */

#define VSHADOW_OFFLINE_MAX_READ_RETRIES 3

static void print_vshadow_error( libvshadow_error_t **error )
{
	char message[ 1024 ];

	if( *error == NULL )
	{
		return;
	}
	if( libvshadow_error_sprint( *error, message, sizeof( message ) ) != -1 )
	{
		fprintf( stderr, "%s\n", message );
	}
	libvshadow_error_free( error );
}

/* Windows FILETIME（100纳秒为单位，1601-01-01起算）转成人类可读的UTC
 * 时间字符串——标准换算公式：减去1601到1970之间的间隔（11644473600秒），
 * 再把100纳秒单位换算成秒。不依赖任何额外的日期处理库。 */
static void format_filetime_as_utc_string( uint64_t filetime, char *out_buf, size_t out_buf_size )
{
	time_t unix_seconds;
	struct tm *utc_tm;

	unix_seconds = (time_t) ( ( filetime / 10000000ULL ) - 11644473600ULL );
	utc_tm = gmtime( &unix_seconds );
	if( utc_tm == NULL )
	{
		snprintf( out_buf, out_buf_size, "(无法解析时间)" );
		return;
	}
	strftime( out_buf, out_buf_size, "%Y-%m-%d %H:%M:%S UTC", utc_tm );
}

static int command_list_shadows_offline( const char *image_path )
{
	libvshadow_volume_t *volume = NULL;
	libvshadow_error_t *error   = NULL;
	int number_of_stores = 0;
	int i;

	if( libvshadow_volume_initialize( &volume, &error ) != 1 )
	{
		fprintf( stderr, "无法初始化 vshadow volume 对象\n" );
		return EXIT_FAILURE;
	}
	if( libvshadow_volume_open( volume, image_path, LIBVSHADOW_OPEN_READ, &error ) != 1 )
	{
		fprintf( stderr, "无法把这个镜像当作包含VSS数据的卷来解析: %s\n", image_path );
		print_vshadow_error( &error );
		libvshadow_volume_free( &volume, NULL );
		return EXIT_FAILURE;
	}
	if( libvshadow_volume_get_number_of_stores( volume, &number_of_stores, &error ) != 1 )
	{
		fprintf( stderr, "无法枚举卷影副本数量\n" );
		print_vshadow_error( &error );
		libvshadow_volume_close( volume, NULL );
		libvshadow_volume_free( &volume, NULL );
		return EXIT_FAILURE;
	}

	printf( "%s 里记录的历史卷影副本（store）：\n", image_path );
	if( number_of_stores == 0 )
	{
		printf( "  （没有检测到任何卷影副本数据——这个镜像对应的卷可能没有开启过\n"
		        "   \"系统保护\"/卷影复制服务，这不是本工具的问题）\n" );
	}
	else
	{
		for( i = 0; i < number_of_stores; i++ )
		{
			libvshadow_store_t *store = NULL;
			uint64_t creation_filetime = 0;
			char time_string[ 64 ];

			if( libvshadow_volume_get_store( volume, i, &store, &error ) != 1 )
			{
				printf( "  [%d] （无法读取这个 store 的信息）\n", i );
				if( error != NULL )
				{
					libvshadow_error_free( &error );
				}
				continue;
			}
			strncpy( time_string, "(未知)", sizeof( time_string ) - 1 );
			time_string[ sizeof( time_string ) - 1 ] = '\0';
			if( libvshadow_store_get_creation_time( store, &creation_filetime, &error ) == 1 )
			{
				format_filetime_as_utc_string( creation_filetime, time_string, sizeof( time_string ) );
			}
			else if( error != NULL )
			{
				libvshadow_error_free( &error );
			}
			printf( "  [%d] 创建时间=%s\n", i, time_string );
			libvshadow_store_free( &store, NULL );
		}
		printf( "\n用 `recoverctl scan-shadow-offline %s <store序号>` 扫描某个历史卷影副本。\n", image_path );
	}

	libvshadow_volume_close( volume, NULL );
	libvshadow_volume_free( &volume, NULL );
	return EXIT_SUCCESS;
}

/* scan-shadow-offline 和 recover-shadow-offline 共用的前置步骤：打开镜像、
 * 定位指定序号的 store、构造 vshadow_bfio_adapter。调用方用完
 * out_bfio_handle 之后负责释放；out_volume/out_store 用完也要调用方
 * 自己 close/free（这个函数不负责，和"谁构造谁清理"的既有约定一致）。
 * 返回 1 成功，-1 失败（已经打印过错误信息）。 */
static int vshadow_offline_open_store_and_bfio(
     const char *image_path,
     const char *store_index_str,
     bad_sector_log_t *bad_log,
     libvshadow_volume_t **out_volume,
     libvshadow_store_t **out_store,
     libbfio_handle_t **out_bfio_handle )
{
	libvshadow_error_t *error = NULL;
	libfsntfs_error_t *fsntfs_error = NULL;
	long store_index;
	char *end_pointer = NULL;

	store_index = strtol( store_index_str, &end_pointer, 10 );
	if( end_pointer == store_index_str || *end_pointer != '\0' || store_index < 0 )
	{
		fprintf( stderr, "无效的 store 序号: %s\n", store_index_str );
		return -1;
	}

	if( libvshadow_volume_initialize( out_volume, &error ) != 1 )
	{
		fprintf( stderr, "无法初始化 vshadow volume 对象\n" );
		return -1;
	}
	if( libvshadow_volume_open( *out_volume, image_path, LIBVSHADOW_OPEN_READ, &error ) != 1 )
	{
		fprintf( stderr, "无法把这个镜像当作包含VSS数据的卷来解析: %s\n", image_path );
		print_vshadow_error( &error );
		libvshadow_volume_free( out_volume, NULL );
		return -1;
	}
	if( libvshadow_volume_get_store( *out_volume, (int) store_index, out_store, &error ) != 1 )
	{
		fprintf( stderr, "找不到 store 序号 %ld\n", store_index );
		print_vshadow_error( &error );
		libvshadow_volume_close( *out_volume, NULL );
		libvshadow_volume_free( out_volume, NULL );
		return -1;
	}
	if( vshadow_bfio_adapter_initialize(
	     *out_store, VSHADOW_OFFLINE_MAX_READ_RETRIES, bad_log, out_bfio_handle, &fsntfs_error ) != 1 )
	{
		fprintf( stderr, "无法构造自定义 I/O 句柄\n" );
		if( fsntfs_error != NULL )
		{
			libfsntfs_error_free( &fsntfs_error );
		}
		libvshadow_store_free( out_store, NULL );
		libvshadow_volume_close( *out_volume, NULL );
		libvshadow_volume_free( out_volume, NULL );
		return -1;
	}

	return 1;
}

static int command_scan_shadow_offline( const char *image_path, const char *store_index_str )
{
	libvshadow_volume_t *volume = NULL;
	libvshadow_store_t *store   = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	bad_sector_log_t bad_log;
	char display_label[ 1200 ];
	char recover_hint[ 1200 ];
	int return_code;

	memset( &bad_log, 0, sizeof( bad_log ) );

	if( vshadow_offline_open_store_and_bfio(
	     image_path, store_index_str, &bad_log, &volume, &store, &bfio_handle ) != 1 )
	{
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	snprintf( display_label, sizeof( display_label ), "%s (store %s, 离线解析)", image_path, store_index_str );
	snprintf( recover_hint, sizeof( recover_hint ), "recover-shadow-offline %s %s", image_path, store_index_str );

	return_code = command_scan_ntfs_via_bfio_handle( bfio_handle, &bad_log, display_label, recover_hint );

	libvshadow_store_free( &store, NULL );
	libvshadow_volume_close( volume, NULL );
	libvshadow_volume_free( &volume, NULL );
	bad_sector_log_free( &bad_log );
	return return_code;
}

static int command_recover_shadow_offline(
     const char *image_path, const char *store_index_str, const char *mft_index_str, const char *output_path )
{
	libvshadow_volume_t *volume = NULL;
	libvshadow_store_t *store   = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	bad_sector_log_t bad_log;
	char display_label[ 1200 ];
	uint64_t mft_entry_index = 0;
	char *end_pointer = NULL;
	int return_code;

	memset( &bad_log, 0, sizeof( bad_log ) );

	mft_entry_index = (uint64_t) strtoull( mft_index_str, &end_pointer, 10 );
	if( end_pointer == mft_index_str || *end_pointer != '\0' )
	{
		fprintf( stderr, "无效的 MFT 索引: %s\n", mft_index_str );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	if( vshadow_offline_open_store_and_bfio(
	     image_path, store_index_str, &bad_log, &volume, &store, &bfio_handle ) != 1 )
	{
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	snprintf( display_label, sizeof( display_label ), "%s (store %s, 离线解析)", image_path, store_index_str );

	return_code = command_recover_ntfs_via_bfio_handle( bfio_handle, &bad_log, display_label, mft_entry_index, output_path );

	libvshadow_store_free( &store, NULL );
	libvshadow_volume_close( volume, NULL );
	libvshadow_volume_free( &volume, NULL );
	bad_sector_log_free( &bad_log );
	return return_code;
}

static int command_recover_fat( const char *volume_path, const char *index_str, const char *output_path )
{
	fat_reader_t reader;
	fat_undelete_scan_result_t result;
	bad_sector_log_t bad_log;
	long candidate_index;
	char *end_pointer = NULL;
	int success = 0;
	int scanned_count;

	memset( &bad_log, 0, sizeof( bad_log ) );

	/* 用"扫描结果里的候选项序号"而不是"起始簇号"来定位要恢复哪一个：
	 * 真实磁盘上一个簇号被反复删除/复用多次是很常见的（这次在虚拟机真实
	 * FAT32 卷上测试就实际撞上了——同一个簇号 5 先后被两个不同的被删文件
	 * 占用过，各自在根目录里都留了一条过期的目录项），按簇号查找会在这种
	 * 情况下静默地恢复成"内容是对的、但用了别的目录项的文件大小"这种半对
	 * 半错的结果，没有任何报错提示，很危险。候选项序号是 scan 当次结果数组
	 * 的下标，天然唯一，不会有这个问题。 */
	candidate_index = strtol( index_str, &end_pointer, 10 );
	if( end_pointer == index_str || *end_pointer != '\0' || candidate_index < 0 )
	{
		fprintf( stderr, "无效的候选项序号: %s\n", index_str );
		return EXIT_FAILURE;
	}

	if( fat_reader_open( volume_path, &reader ) != 1 )
	{
		fprintf( stderr, "无法打开卷: %s\n", volume_path );
		return EXIT_FAILURE;
	}

	memset( &result, 0, sizeof( result ) );
	if( fat_reader_scan_directory_tree( &reader, &bad_log, NULL, NULL, &result ) != 1 )
	{
		fprintf( stderr, "扫描失败\n" );
		fat_reader_close( &reader );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	scanned_count = result.count;
	if( candidate_index < scanned_count )
	{
		if( fat_reader_recover_file( &reader, result.candidates[ candidate_index ].first_cluster,
		                              result.candidates[ candidate_index ].file_size, &bad_log, output_path ) != 1 )
		{
			fprintf( stderr, "写出内容失败: %s\n", output_path );
			fat_undelete_scan_result_free( &result );
			fat_reader_close( &reader );
			bad_sector_log_free( &bad_log );
			return EXIT_FAILURE;
		}
		printf( "已恢复候选项 [%ld]（起始簇=%u）-> %s\n",
		        candidate_index, result.candidates[ candidate_index ].first_cluster, output_path );
		success = 1;
	}

	print_bad_sector_summary( &bad_log );
	fat_undelete_scan_result_free( &result );
	fat_reader_close( &reader );
	bad_sector_log_free( &bad_log );

	if( !success )
	{
		fprintf( stderr, "找不到候选项序号 %ld（当前扫描结果只有 %d 个候选项，重新跑一次 scan 确认编号）\n",
		         candidate_index, scanned_count );
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static int command_recover_exfat( const char *volume_path, const char *index_str, const char *output_path )
{
	exfat_reader_t reader;
	exfat_undelete_scan_result_t result;
	bad_sector_log_t bad_log;
	long candidate_index;
	char *end_pointer = NULL;
	int success = 0;
	int scanned_count;

	memset( &bad_log, 0, sizeof( bad_log ) );

	/* 用候选项序号而不是起始簇号定位，原因和 command_recover_fat 一样：
	 * 真实磁盘上簇号会被反复复用，按簇号查找可能静默恢复成错误的那条记录。 */
	candidate_index = strtol( index_str, &end_pointer, 10 );
	if( end_pointer == index_str || *end_pointer != '\0' || candidate_index < 0 )
	{
		fprintf( stderr, "无效的候选项序号: %s\n", index_str );
		return EXIT_FAILURE;
	}

	if( exfat_reader_open( volume_path, &reader ) != 1 )
	{
		fprintf( stderr, "无法打开卷: %s\n", volume_path );
		return EXIT_FAILURE;
	}

	memset( &result, 0, sizeof( result ) );
	if( exfat_reader_scan_directory_tree( &reader, &bad_log, NULL, NULL, &result ) != 1 )
	{
		fprintf( stderr, "扫描失败\n" );
		exfat_reader_close( &reader );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}

	scanned_count = result.count;
	if( candidate_index < scanned_count )
	{
		if( exfat_reader_recover_file( &reader, result.candidates[ candidate_index ].first_cluster,
		                                result.candidates[ candidate_index ].file_size,
		                                result.candidates[ candidate_index ].no_fat_chain, &bad_log, output_path ) != 1 )
		{
			fprintf( stderr, "写出内容失败: %s\n", output_path );
			exfat_undelete_scan_result_free( &result );
			exfat_reader_close( &reader );
			bad_sector_log_free( &bad_log );
			return EXIT_FAILURE;
		}
		printf( "已恢复候选项 [%ld]（起始簇=%u）-> %s\n",
		        candidate_index, result.candidates[ candidate_index ].first_cluster, output_path );
		success = 1;
	}

	print_bad_sector_summary( &bad_log );
	exfat_undelete_scan_result_free( &result );
	exfat_reader_close( &reader );
	bad_sector_log_free( &bad_log );

	if( !success )
	{
		fprintf( stderr, "找不到候选项序号 %ld（当前扫描结果只有 %d 个候选项，重新跑一次 scan 确认编号）\n",
		         candidate_index, scanned_count );
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

/* 和 command_scan 同样的"先试 NTFS 再试 FAT32 再试 exFAT"判断方式 */
static int command_recover( const char *volume_path, const char *index_str, const char *output_path )
{
	libfsntfs_volume_t *probe_volume = NULL;
	libfsntfs_error_t *probe_error   = NULL;
	int is_ntfs;

	if( libfsntfs_volume_initialize( &probe_volume, &probe_error ) != 1 )
	{
		fprintf( stderr, "无法初始化 volume 对象\n" );
		return EXIT_FAILURE;
	}
	is_ntfs = ( libfsntfs_volume_open( probe_volume, volume_path, LIBFSNTFS_OPEN_READ, &probe_error ) == 1 );
	libfsntfs_volume_free( &probe_volume, NULL );
	if( probe_error != NULL )
	{
		libfsntfs_error_free( &probe_error );
	}

	if( is_ntfs )
	{
		return command_recover_ntfs( volume_path, index_str, output_path );
	}

	{
		fat_reader_t probe_reader;

		if( fat_reader_open( volume_path, &probe_reader ) == 1 )
		{
			fat_reader_close( &probe_reader );
			return command_recover_fat( volume_path, index_str, output_path );
		}
	}

	{
		exfat_reader_t probe_reader;

		if( exfat_reader_open( volume_path, &probe_reader ) == 1 )
		{
			exfat_reader_close( &probe_reader );
			return command_recover_exfat( volume_path, index_str, output_path );
		}
	}

	fprintf( stderr, "无法识别卷的文件系统（已尝试 NTFS、FAT32、exFAT）: %s\n", volume_path );
	return EXIT_FAILURE;
}

/* D4（P4，2026-11-05 第28步排期）：跑一次 PhotoRec 信号雕刻并把结果
 * （report.xml 解析出来的每个雕刻出的文件）打印给用户看。target_path
 * 可以是整块设备/镜像，也可以是 command_recover_carve_partition 先
 * 切出来的分区字节范围临时文件——两种情况命令串里都固定传
 * "partition_none"（第28步小树裁决："B为机制"：不让 PhotoRec 自己重新
 * 探测分区表，范围已经由我们自己决定好了）。
 *
 * 这不是"先scan再recover"两步式的命令（和 scan/recover 不一样）：
 * PhotoRec 的 `/cmd ... search` 本身就是一次把"找+写出来"全做完的原子
 * 操作，拆成两条命令没有实际意义，所以只有一个 recover-carve 命令，
 * 不是 scan-carve/recover-carve 两个——这是拿到第27步实测结果之后才能
 * 确认的产品形状细节，和小树裁决时只谈分工设计、没有细到具体命令
 * 名称不矛盾。 */
static int command_recover_carve(
     const char *photorec_bin_path,
     const char *target_path,
     const char *output_dir_base )
{
	photorec_invoke_result_t invoke_result;
	photorec_report_t report;
	char report_path[ 1600 ];
	FILE *report_file;
	long report_size;
	char *xml_data;
	int i;
	int return_code = EXIT_SUCCESS;

	printf( "正在用 PhotoRec 对 %s 做信号雕刻（文件系统结构化解析失败后的兜底手段，视目标大小可能需要较长时间）...\n",
	        target_path );

	if( photorec_invoker_run( photorec_bin_path, target_path, "partition_none", output_dir_base, &invoke_result ) != 1 )
	{
		fprintf( stderr, "拉起 PhotoRec 子进程失败（确认可执行文件路径是否正确: %s）\n", photorec_bin_path );
		return EXIT_FAILURE;
	}
	if( invoke_result.recup_dir_path[ 0 ] == '\0' )
	{
		fprintf( stderr, "PhotoRec 已退出（退出码 %d），但没有找到它生成的输出目录\n", invoke_result.exit_code );
		return EXIT_FAILURE;
	}
	snprintf( report_path, sizeof( report_path ), "%s/report.xml", invoke_result.recup_dir_path );

	report_file = fopen( report_path, "rb" );
	if( report_file == NULL )
	{
		fprintf( stderr, "找不到 PhotoRec 生成的 report.xml: %s\n", report_path );
		return EXIT_FAILURE;
	}
	if( ( fseek( report_file, 0, SEEK_END ) != 0 )
	 || ( ( report_size = ftell( report_file ) ) < 0 )
	 || ( fseek( report_file, 0, SEEK_SET ) != 0 ) )
	{
		fprintf( stderr, "读取 report.xml 失败: %s\n", report_path );
		fclose( report_file );
		return EXIT_FAILURE;
	}
	xml_data = (char *) malloc( (size_t) report_size + 1 );
	if( xml_data == NULL )
	{
		fclose( report_file );
		return EXIT_FAILURE;
	}
	if( fread( xml_data, 1, (size_t) report_size, report_file ) != (size_t) report_size )
	{
		fprintf( stderr, "读取 report.xml 失败: %s\n", report_path );
		free( xml_data );
		fclose( report_file );
		return EXIT_FAILURE;
	}
	xml_data[ report_size ] = '\0';
	fclose( report_file );

	memset( &report, 0, sizeof( report ) );
	if( photorec_report_parser_parse( xml_data, (size_t) report_size, &report ) != 1 )
	{
		fprintf( stderr, "解析 report.xml 失败: %s\n", report_path );
		free( xml_data );
		return EXIT_FAILURE;
	}
	free( xml_data );

	if( report.count == 0 )
	{
		printf( "没有雕刻出任何文件。PhotoRec 输出目录: %s\n", invoke_result.recup_dir_path );
	}
	else
	{
		printf( "雕刻出 %d 个文件，已保存在: %s\n", report.count, invoke_result.recup_dir_path );
		for( i = 0; i < report.count; i++ )
		{
			printf( "  [%d] %s  大小=%llu字节  原始物理偏移=%llu\n",
			        i,
			        report.entries[ i ].name,
			        (unsigned long long) report.entries[ i ].file_size,
			        (unsigned long long) report.entries[ i ].img_offset );
		}
		print_carve_disclaimer();
	}
	photorec_report_free( &report );
	return return_code;
}

/* 先把分区按我们自己（libvsgpt/libvsmbr）解析出来的字节边界切到一个
 * 临时文件，再对这个临时文件做信号雕刻——复用 cli/main.c 已有的
 * extract_partition_to_temp_file（NTFS 分区偏移扫描那条路径已经在用，
 * 第28步小树裁决明确指出直接复用这个函数，不重新发明）。 */
static int command_recover_carve_partition(
     const char *photorec_bin_path,
     const char *device_path,
     long offset,
     uint64_t size,
     const char *output_dir_base )
{
	char temp_path[ 1024 ];
	bad_sector_log_t bad_log;
	int return_code;

	memset( &bad_log, 0, sizeof( bad_log ) );

	if( size == 0 )
	{
		fprintf( stderr, "信号雕刻需要知道分区大小才能先把这段范围切出来，不能是 0\n" );
		return EXIT_FAILURE;
	}
	if( partition_extractor_get_temp_file_path( temp_path, sizeof( temp_path ) ) != 1 )
	{
		fprintf( stderr, "无法确定临时文件路径\n" );
		return EXIT_FAILURE;
	}
	printf( "先把这段分区范围拷到临时文件 %s 再做信号雕刻...\n", temp_path );

	if( partition_extractor_extract_to_temp_file( device_path, offset, size, &bad_log, temp_path ) != 1 )
	{
		fprintf( stderr, "拷贝分区内容失败\n" );
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}
	print_bad_sector_summary( &bad_log );
	bad_sector_log_free( &bad_log );

	return_code = command_recover_carve( photorec_bin_path, temp_path, output_dir_base );
	remove( temp_path );
	return return_code;
}

static void print_bitlocker_error( libbde_error_t **error )
{
	char message[ 1024 ];

	if( *error == NULL )
	{
		return;
	}
	if( libbde_error_sprint( *error, message, sizeof( message ) ) != -1 )
	{
		fprintf( stderr, "%s\n", message );
	}
	libbde_error_free( error );
}

/* B1（BitLocker，2026-11-05 任务拆解排期）：诊断命令，不碰任何凭据，
 * 只列出这个卷上登记了哪些密钥保护器——libbde打开BDE卷的元数据本身
 * 是明文的，只有卷数据才加密，所以这一步完全不涉及敏感信息处理。 */
static int command_list_bitlocker_key_protectors( const char *device_path, uint64_t base_offset )
{
	bitlocker_key_protector_list_t list;
	libbde_error_t *error = NULL;
	int i;

	memset( &list, 0, sizeof( list ) );

	if( bitlocker_volume_opener_list_key_protectors( device_path, base_offset, &list, &error ) != 1 )
	{
		fprintf( stderr, "无法打开/解析该卷的BitLocker元数据: %s（偏移=%llu）\n", device_path, (unsigned long long) base_offset );
		print_bitlocker_error( &error );
		return EXIT_FAILURE;
	}
	if( list.count == 0 )
	{
		printf( "这个卷没有检测到任何BitLocker密钥保护器（可能不是BitLocker加密卷，或者还没有启用BitLocker）\n" );
	}
	else
	{
		printf( "找到 %d 个密钥保护器：\n", list.count );
		for( i = 0; i < list.count; i++ )
		{
			int j;

			printf( "  [%d] 类型=%s  GUID=", i, bitlocker_key_protector_type_name( list.entries[ i ].type ) );
			for( j = 0; j < 16; j++ )
			{
				printf( "%02x", list.entries[ i ].guid[ j ] );
			}
			printf( "\n" );
		}
	}
	bitlocker_key_protector_list_free( &list );
	return EXIT_SUCCESS;
}

#ifdef _WIN32
/* I3（BitLocker自动识别，2026-11-06小树裁决）：诊断命令，独立于GUI的
 * 验证入口——和I1/I2延续这个项目一贯"CLI先行、GUI复用"的顺序（list-
 * shadows/scan-bitlocker等都是这个模式），不是GUI专属功能。 */
static int command_disk_encryption_status( char drive_letter )
{
	bitlocker_status_t status;
	volume_disk_extents_t extents;

	if( bitlocker_status_checker_query( drive_letter, &status ) != 1 )
	{
		fprintf( stderr, "查询 %c: 的BitLocker状态失败（PowerShell查询没有正常完成）\n", drive_letter );
		return EXIT_FAILURE;
	}

	printf( "盘符: %c:\n", drive_letter );
	if( !status.is_fve_manageable )
	{
		printf( "  不受BitLocker管理（这个盘从来没有和BitLocker打过交道）\n" );
	}
	else if( status.is_locked )
	{
		printf( "  已加密，当前已锁定（需要提供凭据才能读取）\n" );
	}
	else if( status.is_protected )
	{
		printf( "  已加密，当前已解锁（已用 %d%%）\n", status.encryption_percentage );
	}
	else
	{
		printf( "  未加密\n" );
	}

	if( volume_disk_extents_query( drive_letter, &extents ) != 1 )
	{
		fprintf( stderr, "查询 %c: 的物理盘位置失败\n", drive_letter );
		return EXIT_FAILURE;
	}
	if( !extents.is_single_extent )
	{
		printf( "  物理盘位置: 这个卷跨多个物理区间，无法自动定位单一的物理盘+偏移\n" );
	}
	else
	{
		printf( "  物理盘位置: \\\\.\\PhysicalDrive%u，偏移 %llu 字节（大小 %llu 字节）\n",
		     extents.disk_number, (unsigned long long) extents.starting_offset, (unsigned long long) extents.extent_length );
	}
	return EXIT_SUCCESS;
}
#endif

#define BITLOCKER_MAX_READ_RETRIES 3

/* B8（BitLocker，2026-11-05 任务拆解排期）：把credential-type字符串
 * 解析成 bitlocker_credential_type_t。 */
static int parse_bitlocker_credential_type( const char *type_str, bitlocker_credential_type_t *out_type )
{
	if( strcmp( type_str, "password" ) == 0 )
	{
		*out_type = BITLOCKER_CREDENTIAL_TYPE_PASSWORD;
	}
	else if( strcmp( type_str, "recovery-key" ) == 0 )
	{
		*out_type = BITLOCKER_CREDENTIAL_TYPE_RECOVERY_KEY;
	}
	else if( strcmp( type_str, "startup-key" ) == 0 )
	{
		*out_type = BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY;
	}
	else if( strcmp( type_str, "fvek" ) == 0 )
	{
		*out_type = BITLOCKER_CREDENTIAL_TYPE_FVEK;
	}
	else
	{
		fprintf( stderr, "无效的凭据类型: %s（应该是 password / recovery-key / startup-key / fvek 之一）\n", type_str );
		return -1;
	}
	return 1;
}

/* 三种凭据渠道（file/B5、cli/B10、interactive/B11c）全部接入，世杰
 * 拍板要的三种渠道到这里齐了。 */
static int bitlocker_open_volume_and_bfio(
     const char *device_path,
     uint64_t base_offset,
     bitlocker_credential_type_t credential_type,
     const char *credential_source,
     char *credential_value,
     bad_sector_log_t *bad_log,
     libbde_volume_t **out_volume,
     libbfio_handle_t **out_unlock_bfio_handle,
     libbfio_handle_t **out_bfio_handle )
{
	bitlocker_resolved_credential_t credential;
	libbde_error_t *error = NULL;
	libfsntfs_error_t *fsntfs_error = NULL;

	if( strcmp( credential_source, "file" ) == 0 )
	{
		if( bitlocker_credential_source_load_from_file( credential_type, credential_value, &credential ) != 1 )
		{
			return -1;
		}
	}
	else if( strcmp( credential_source, "cli" ) == 0 )
	{
		int load_result = bitlocker_credential_source_load_from_cli_argument( credential_type, credential_value, &credential );

		/* B10：凭据一旦拷进 credential，立刻原地覆写这个argv槽位——
		 * 缩短凭据明文留在进程参数列表里的窗口期，但如世杰拍板时
		 * 已经提醒过的：覆写只能缩短窗口，不能消除"这个进程启动时
		 * 命令行参数/shell历史里曾经出现过明文凭据"这个事实本身，
		 * 这条渠道的这个暴露面是命令行参数机制本身决定的，不是本
		 * 项目能单方面修复的，必须在帮助文案/文档里诚实告知用户。 */
		secure_memory_zero( credential_value, strlen( credential_value ) );
		if( load_result != 1 )
		{
			return -1;
		}
	}
	else if( strcmp( credential_source, "interactive" ) == 0 )
	{
		/* B11c：不经过argv——credential_value这个参数对这条渠道毫无
		 * 意义（CLI层在解析命令行时就没有给这条渠道留credential_value
		 * 位置，见main()里的参数个数判断），内容是现场从终端不回显
		 * 读出来的。 */
		int load_result = bitlocker_credential_source_load_from_interactive( credential_type, &credential );

		if( load_result == 0 )
		{
			fprintf( stderr, "已取消。\n" );
			return -1;
		}
		if( load_result != 1 )
		{
			return -1;
		}
	}
	else
	{
		fprintf( stderr, "无效的凭据来源: %s（应该是 file / cli / interactive 之一）\n", credential_source );
		return -1;
	}
	if( bitlocker_unlock_volume( device_path, base_offset, &credential, out_volume, out_unlock_bfio_handle, &error ) != 1 )
	{
		if( error != NULL )
		{
			print_bitlocker_error( &error );
		}
		else
		{
			fprintf( stderr, "解锁失败：凭据不正确，或者凭据类型和这个卷实际使用的保护器不匹配\n" );
		}
		return -1;
	}
	if( bitlocker_bfio_adapter_initialize( *out_volume, BITLOCKER_MAX_READ_RETRIES, bad_log, out_bfio_handle, &fsntfs_error ) != 1 )
	{
		fprintf( stderr, "无法构造自定义 I/O 句柄\n" );
		if( fsntfs_error != NULL )
		{
			libfsntfs_error_free( &fsntfs_error );
		}
		libbde_volume_close( *out_volume, NULL );
		libbfio_handle_free( out_unlock_bfio_handle, NULL );
		libbde_volume_free( out_volume, NULL );
		return -1;
	}
	return 1;
}

static int command_scan_bitlocker(
     const char *device_path,
     uint64_t base_offset,
     const char *offset_str,
     const char *type_str,
     const char *source_str,
     char *credential_value )
{
	bitlocker_credential_type_t credential_type;
	libbde_volume_t *volume = NULL;
	libbfio_handle_t *unlock_bfio_handle = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	bad_sector_log_t bad_log;
	char display_label[ 1200 ];
	char recover_hint[ 1200 ];
	int return_code;
	/* source=cli 时 credential_value 本身是明文凭据，解锁流程结束后
	 * 会被原地清零——不能在提示文案里原样回显它（那等于我们自己把
	 * 清零的意义抵消掉），用占位符代替；source=interactive 时
	 * credential_value 根本是 NULL（命令行里压根没有这个位置，B11c），
	 * 同样不能直接喂给 %s，也用占位符。 */
	int credential_is_secret = ( strcmp( source_str, "cli" ) == 0 ) || ( strcmp( source_str, "interactive" ) == 0 );

	if( parse_bitlocker_credential_type( type_str, &credential_type ) != 1 )
	{
		return EXIT_FAILURE;
	}
	memset( &bad_log, 0, sizeof( bad_log ) );

	if( bitlocker_open_volume_and_bfio(
	     device_path, base_offset, credential_type, source_str, credential_value, &bad_log, &volume, &unlock_bfio_handle, &bfio_handle ) != 1 )
	{
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}
	snprintf( display_label, sizeof( display_label ), "%s 偏移=%s (BitLocker)", device_path, offset_str );
	if( strcmp( source_str, "interactive" ) == 0 )
	{
		/* recover-bitlocker 的 interactive 渠道不接受凭据内容参数
		 * （见main()的参数个数判断），提示文案里不能拼一个本来就不
		 * 存在的位置 */
		snprintf( recover_hint, sizeof( recover_hint ), "recover-bitlocker %s %s %s %s",
		          device_path, offset_str, type_str, source_str );
	}
	else
	{
		snprintf( recover_hint, sizeof( recover_hint ), "recover-bitlocker %s %s %s %s %s",
		          device_path, offset_str, type_str, source_str, credential_is_secret ? "<凭据>" : credential_value );
	}

	return_code = command_scan_ntfs_via_bfio_handle( bfio_handle, &bad_log, display_label, recover_hint );

	libbde_volume_close( volume, NULL );
	libbfio_handle_free( &unlock_bfio_handle, NULL );
	libbde_volume_free( &volume, NULL );
	bad_sector_log_free( &bad_log );
	return return_code;
}

static int command_recover_bitlocker(
     const char *device_path,
     uint64_t base_offset,
     const char *offset_str,
     const char *type_str,
     const char *source_str,
     char *credential_value,
     const char *mft_index_str,
     const char *output_path )
{
	bitlocker_credential_type_t credential_type;
	libbde_volume_t *volume = NULL;
	libbfio_handle_t *unlock_bfio_handle = NULL;
	libbfio_handle_t *bfio_handle = NULL;
	bad_sector_log_t bad_log;
	char display_label[ 1200 ];
	uint64_t mft_entry_index;
	char *end_pointer = NULL;
	int return_code;

	if( parse_bitlocker_credential_type( type_str, &credential_type ) != 1 )
	{
		return EXIT_FAILURE;
	}
	mft_entry_index = (uint64_t) strtoull( mft_index_str, &end_pointer, 10 );
	if( ( end_pointer == mft_index_str ) || ( *end_pointer != '\0' ) )
	{
		fprintf( stderr, "无效的 MFT 索引: %s\n", mft_index_str );
		return EXIT_FAILURE;
	}
	memset( &bad_log, 0, sizeof( bad_log ) );

	if( bitlocker_open_volume_and_bfio(
	     device_path, base_offset, credential_type, source_str, credential_value, &bad_log, &volume, &unlock_bfio_handle, &bfio_handle ) != 1 )
	{
		bad_sector_log_free( &bad_log );
		return EXIT_FAILURE;
	}
	snprintf( display_label, sizeof( display_label ), "%s 偏移=%s (BitLocker)", device_path, offset_str );

	return_code = command_recover_ntfs_via_bfio_handle( bfio_handle, &bad_log, display_label, mft_entry_index, output_path );

	libbde_volume_close( volume, NULL );
	libbfio_handle_free( &unlock_bfio_handle, NULL );
	libbde_volume_free( &volume, NULL );
	bad_sector_log_free( &bad_log );
	return return_code;
}

int main( int argc, char *argv[] )
{
	/* 重定向到文件/管道时 CRT 会切成全缓冲，长时间操作（比如拷贝整个分区）
	 * 中途的提示信息要等进程退出才会落盘，调试时很容易误判成"卡住了"或
	 * "没执行到那一步"。关掉缓冲让每条 printf/fprintf 立刻可见。 */
	setvbuf( stdout, NULL, _IONBF, 0 );
	setvbuf( stderr, NULL, _IONBF, 0 );

	if( argc < 2 )
	{
		print_usage( argv[ 0 ] );
		return EXIT_FAILURE;
	}

	if( strcmp( argv[ 1 ], "scan" ) == 0 )
	{
		if( argc != 3 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
#ifdef _WIN32
		{
			char resolved_path[ 64 ];
			resolve_device_path( argv[ 2 ], resolved_path, sizeof( resolved_path ) );
			return command_scan( resolved_path );
		}
#else
		return command_scan( argv[ 2 ] );
#endif
	}

	if( strcmp( argv[ 1 ], "recover" ) == 0 )
	{
		if( argc != 5 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
#ifdef _WIN32
		{
			char resolved_path[ 64 ];
			resolve_device_path( argv[ 2 ], resolved_path, sizeof( resolved_path ) );
			return command_recover( resolved_path, argv[ 3 ], argv[ 4 ] );
		}
#else
		return command_recover( argv[ 2 ], argv[ 3 ], argv[ 4 ] );
#endif
	}

	if( strcmp( argv[ 1 ], "scan-partition" ) == 0 )
	{
		if( argc != 4 && argc != 5 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return command_scan_partition( argv[ 2 ], argv[ 3 ], argc == 5 ? argv[ 4 ] : NULL );
	}

	if( strcmp( argv[ 1 ], "recover-partition" ) == 0 )
	{
		if( argc != 7 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return command_recover_partition( argv[ 2 ], argv[ 3 ], argv[ 4 ], argv[ 5 ], argv[ 6 ] );
	}

if( strcmp( argv[ 1 ], "partitions" ) == 0 )
	{
		if( argc != 3 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
#ifdef _WIN32
		{
			char resolved_path[ 64 ];
			resolve_device_path( argv[ 2 ], resolved_path, sizeof( resolved_path ) );
			return command_partitions( resolved_path );
		}
#else
		return command_partitions( argv[ 2 ] );
#endif
	}

#ifdef _WIN32
	if( strcmp( argv[ 1 ], "list" ) == 0 )
	{
		return command_list();
	}

	/* V4（2026-11-05 第22步，VSS最小版 CLI 整合）：卷影副本设备路径已经
	 * 是完整路径，不需要 resolve_device_path 那套盘符/磁盘编号简写转换；
	 * 直接复用 command_scan_ntfs/command_recover_ntfs——跳过 command_scan/
	 * command_recover 里"先探测是不是NTFS"那段通用逻辑，因为卷影副本在
	 * 本项目目标场景下必然是NTFS，不需要再探测一遍（顺带绕开了第22步
	 * 调研发现的那个局限：libfsntfs 自带的设备路径探测只认 `\\.\` 开头，
	 * 认不出卷影副本 `\\?\GLOBALROOT\Device\...` 这种路径格式，见
	 * AGENTS.md 第8节） */
	if( strcmp( argv[ 1 ], "list-shadows" ) == 0 )
	{
		if( argc != 3 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return command_list_shadows( argv[ 2 ] );
	}

	if( strcmp( argv[ 1 ], "disk-encryption-status" ) == 0 )
	{
		if( argc != 3 || strlen( argv[ 2 ] ) < 1 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return command_disk_encryption_status( argv[ 2 ][ 0 ] );
	}

	if( strcmp( argv[ 1 ], "scan-shadow" ) == 0 )
	{
		int return_code;

		if( argc != 3 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return_code = command_scan_ntfs( argv[ 2 ] );

		/* command_scan_ntfs 自己打印的"用 recoverctl recover ..."提示不对
		 * （卷影副本设备路径不能走 recover，会撞上 command_recover 那段
		 * 通用探测逻辑的局限，必须走 recover-shadow），和
		 * command_scan_partition_ntfs 那边补正提示是同一个已有模式 */
		if( return_code == EXIT_SUCCESS )
		{
			printf( "\n（上面提示里的 `recover` 命令不对，卷影副本要用"
			        " `recoverctl recover-shadow %s <MFT 索引> <输出路径>`）\n",
			        argv[ 2 ] );
		}
		return return_code;
	}

	if( strcmp( argv[ 1 ], "recover-shadow" ) == 0 )
	{
		if( argc != 5 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return command_recover_ntfs( argv[ 2 ], argv[ 3 ], argv[ 4 ] );
	}
#endif

	/* 2026-11-05 第25步（P3-libvshadow离线解析）：跨平台，不放进
	 * #ifdef _WIN32——解析的是一个镜像文件本身，不依赖任何Windows专属的
	 * 系统API，Mac/Windows都能跑（Mac侧用来对着 vss.raw 这类fixture做
	 * TDD验证） */
	if( strcmp( argv[ 1 ], "list-shadows-offline" ) == 0 )
	{
		if( argc != 3 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return command_list_shadows_offline( argv[ 2 ] );
	}

	if( strcmp( argv[ 1 ], "scan-shadow-offline" ) == 0 )
	{
		if( argc != 4 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return command_scan_shadow_offline( argv[ 2 ], argv[ 3 ] );
	}

	if( strcmp( argv[ 1 ], "recover-shadow-offline" ) == 0 )
	{
		if( argc != 6 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return command_recover_shadow_offline( argv[ 2 ], argv[ 3 ], argv[ 4 ], argv[ 5 ] );
	}

	if( strcmp( argv[ 1 ], "recover-carve" ) == 0 )
	{
		if( argc != 5 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		return command_recover_carve( argv[ 2 ], argv[ 3 ], argv[ 4 ] );
	}

	if( strcmp( argv[ 1 ], "recover-carve-partition" ) == 0 )
	{
		long carve_offset;
		uint64_t carve_size;
		char *end_pointer = NULL;

		if( argc != 7 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		carve_offset = strtol( argv[ 4 ], &end_pointer, 10 );
		if( ( end_pointer == argv[ 4 ] ) || ( *end_pointer != '\0' ) )
		{
			fprintf( stderr, "无效的分区偏移: %s\n", argv[ 4 ] );
			return EXIT_FAILURE;
		}
		end_pointer = NULL;
		carve_size = (uint64_t) strtoull( argv[ 5 ], &end_pointer, 10 );
		if( ( end_pointer == argv[ 5 ] ) || ( *end_pointer != '\0' ) )
		{
			fprintf( stderr, "无效的分区大小: %s\n", argv[ 5 ] );
			return EXIT_FAILURE;
		}
		return command_recover_carve_partition( argv[ 2 ], argv[ 3 ], carve_offset, carve_size, argv[ 6 ] );
	}

	/* B9真机验证+2026-11-05小树裁决：锁定的BitLocker卷不能通过卷盘符
	 * 设备对象（`\\.\G:`）直接读取（Windows fvevol.sys过滤驱动会拒绝），
	 * 三个BitLocker命令的<设备路径>因此拆成<磁盘/卷路径> <偏移>两个
	 * 参数（对齐scan-partition的形状）——偏移=0就是"直接传卷路径"这个
	 * 旧用法的退化特例，没有保留单参数旧形式的必要（裁决③）。 */
	if( strcmp( argv[ 1 ], "list-bitlocker-key-protectors" ) == 0 )
	{
		uint64_t base_offset;
		char *end_pointer = NULL;

		if( argc != 4 )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		base_offset = (uint64_t) strtoull( argv[ 3 ], &end_pointer, 10 );
		if( ( end_pointer == argv[ 3 ] ) || ( *end_pointer != '\0' ) )
		{
			fprintf( stderr, "无效的偏移: %s\n", argv[ 3 ] );
			return EXIT_FAILURE;
		}
		return command_list_bitlocker_key_protectors( argv[ 2 ], base_offset );
	}

	if( strcmp( argv[ 1 ], "scan-bitlocker" ) == 0 )
	{
		uint64_t base_offset;
		char *end_pointer = NULL;
		/* B11c：interactive渠道不接受凭据内容参数（内容现场从终端
		 * 不回显读出来，压根不该出现在argv里）——这个渠道的命令行比
		 * file/cli渠道少一个位置，argc因此有两种合法值 */
		int is_interactive;

		if( ( argc != 6 ) && ( argc != 7 ) )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		is_interactive = ( strcmp( argv[ 5 ], "interactive" ) == 0 );
		if( is_interactive != ( argc == 6 ) )
		{
			fprintf( stderr, "参数个数不对：interactive 渠道不需要（也不接受）凭据内容参数；file/cli 渠道必须提供凭据内容参数（是不是漏传了？）\n" );
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		base_offset = (uint64_t) strtoull( argv[ 3 ], &end_pointer, 10 );
		if( ( end_pointer == argv[ 3 ] ) || ( *end_pointer != '\0' ) )
		{
			fprintf( stderr, "无效的偏移: %s\n", argv[ 3 ] );
			return EXIT_FAILURE;
		}
		return command_scan_bitlocker( argv[ 2 ], base_offset, argv[ 3 ], argv[ 4 ], argv[ 5 ], is_interactive ? NULL : argv[ 6 ] );
	}

	if( strcmp( argv[ 1 ], "recover-bitlocker" ) == 0 )
	{
		uint64_t base_offset;
		char *end_pointer = NULL;
		int is_interactive;

		if( ( argc != 8 ) && ( argc != 9 ) )
		{
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		is_interactive = ( strcmp( argv[ 5 ], "interactive" ) == 0 );
		if( is_interactive != ( argc == 8 ) )
		{
			fprintf( stderr, "参数个数不对：interactive 渠道不需要（也不接受）凭据内容参数；file/cli 渠道必须提供凭据内容参数（是不是漏传了？）\n" );
			print_usage( argv[ 0 ] );
			return EXIT_FAILURE;
		}
		base_offset = (uint64_t) strtoull( argv[ 3 ], &end_pointer, 10 );
		if( ( end_pointer == argv[ 3 ] ) || ( *end_pointer != '\0' ) )
		{
			fprintf( stderr, "无效的偏移: %s\n", argv[ 3 ] );
			return EXIT_FAILURE;
		}
		if( is_interactive )
		{
			return command_recover_bitlocker( argv[ 2 ], base_offset, argv[ 3 ], argv[ 4 ], argv[ 5 ], NULL, argv[ 6 ], argv[ 7 ] );
		}
		return command_recover_bitlocker( argv[ 2 ], base_offset, argv[ 3 ], argv[ 4 ], argv[ 5 ], argv[ 6 ], argv[ 7 ], argv[ 8 ] );
	}

	fprintf( stderr, "未知命令: %s\n", argv[ 1 ] );
	print_usage( argv[ 0 ] );
	return EXIT_FAILURE;
}
