#include <string.h>

#include "ntfs_file_reference.h"
#include "undelete_detector.h"

int undelete_detector_inspect(
     libfsntfs_file_entry_t *file_entry,
     uint64_t mft_entry_index,
     undelete_candidate_t *out,
     libfsntfs_error_t **error )
{
	libfsntfs_attribute_t *attribute = NULL;
	int is_allocated           = 0;
	int number_of_attributes   = 0;
	int attribute_index        = 0;
	uint32_t attribute_type    = 0;
	size_t name_size           = 0;

	if( out == NULL )
	{
		return -1;
	}
	memset( out, 0, sizeof( undelete_candidate_t ) );

	out->mft_entry_index = mft_entry_index;

	if( file_entry == NULL )
	{
		return -1;
	}

	is_allocated = libfsntfs_file_entry_is_allocated( file_entry, error );

	if( is_allocated == -1 )
	{
		/* 这里失败不代表扫描本身出了问题，而是这条 MFT 记录压根不是一条
		 * 合法记录（比如 NTFS 规范里"预留给未来系统文件"的区域，真实
		 * Windows 格式化后这块是彻底的全零字节，不像我们早期合成的测试
		 * 镜像那样盖了个"空白但合法"的记录戳）。2026-11-04 在真实 Windows
		 * 卷上做 Task #9 测试时发现：libfsntfs 对着这种垃圾字节判断
		 * is_allocated 会报错，而不是返回"未分配"。这种情况下，这条记录
		 * 既不是正常文件也不是可恢复的已删除文件，直接当作"不可恢复"处理，
		 * 不能把这个错误升级成整个扫描失败——真实磁盘上这种槽位大概率不止
		 * 一处，一遇到就中断扫描会导致后面所有真正可恢复的文件都扫不到。
		 */
		if( ( error != NULL ) && ( *error != NULL ) )
		{
			libfsntfs_error_free( error );
		}
		out->is_deleted_recoverable = 0;
		return 1;
	}
	if( is_allocated == 1 )
	{
		/* 正常存在的文件，不是我们要找的已删除候选项 */
		out->is_deleted_recoverable = 0;
		return 1;
	}

	if( libfsntfs_file_entry_get_number_of_attributes( file_entry, &number_of_attributes, error ) != 1 )
	{
		return -1;
	}

	for( attribute_index = 0; attribute_index < number_of_attributes; attribute_index++ )
	{
		if( libfsntfs_file_entry_get_attribute_by_index( file_entry, attribute_index, &attribute, error ) != 1 )
		{
			/* 单条属性读取失败就跳过，不代表整体失败 */
			continue;
		}

		if( libfsntfs_attribute_get_type( attribute, &attribute_type, error ) == 1
		 && attribute_type == LIBFSNTFS_ATTRIBUTE_TYPE_FILE_NAME )
		{
			if( libfsntfs_file_name_attribute_get_utf8_name_size( attribute, &name_size, error ) == 1
			 && name_size > 0
			 && name_size <= sizeof( out->name ) )
			{
				libfsntfs_file_name_attribute_get_utf8_name(
				 attribute,
				 (uint8_t *) out->name,
				 name_size,
				 error );

				/* 同一个 attribute 对象上顺手把父目录引用也读出来，不需要
				 * 额外打开任何文件/做任何新的 I/O——这是为后续路径重建
				 * 准备的，这个函数本身不负责真正走父目录链 */
				{
					uint64_t parent_file_reference = 0;

					if( libfsntfs_file_name_attribute_get_parent_file_reference(
					     attribute, &parent_file_reference, error ) == 1 )
					{
						out->parent_mft_index = ntfs_file_reference_to_mft_index( parent_file_reference );
					}
					else if( ( error != NULL ) && ( *error != NULL ) )
					{
						libfsntfs_error_free( error );
					}
				}
			}
		}

		libfsntfs_attribute_free( &attribute, error );

		if( out->name[ 0 ] != '\0' )
		{
			break;
		}
	}

	/* 未分配 + 还能读到 $FILE_NAME，才算"已删除但可恢复"；
	 * 未分配但读不到任何名字的，是从未使用过的空槽位，不算 */
	out->is_deleted_recoverable = ( out->name[ 0 ] != '\0' ) ? 1 : 0;

	return 1;
}
