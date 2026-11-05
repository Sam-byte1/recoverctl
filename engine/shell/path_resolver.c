#include <string.h>

#include "../core/ntfs_file_reference.h"
#include "../core/path_builder.h"
#include "path_resolver.h"

/* 防御真实损坏数据可能出现的环形父目录引用——正常 NTFS 卷的目录嵌套
 * 深度不可能接近这个数字，纯粹是安全阀 */
#define PATH_RESOLVER_MAX_DEPTH 64

/* 找到 file_entry 的第一个 $FILE_NAME 属性，读出 UTF-8 名字和父目录引用。
 * 返回 1 成功找到，0 表示没找到任何 $FILE_NAME 属性（不是错误，只是没有），
 * -1 表示过程中出现无法处理的错误。 */
static int get_name_and_parent_reference(
     libfsntfs_file_entry_t *file_entry,
     char *out_name,
     size_t out_name_size,
     uint64_t *out_parent_file_reference,
     libfsntfs_error_t **error )
{
	libfsntfs_attribute_t *attribute = NULL;
	int number_of_attributes = 0;
	int attribute_index      = 0;
	uint32_t attribute_type  = 0;
	size_t name_size         = 0;
	int found = 0;

	if( libfsntfs_file_entry_get_number_of_attributes( file_entry, &number_of_attributes, error ) != 1 )
	{
		return -1;
	}

	for( attribute_index = 0; attribute_index < number_of_attributes && !found; attribute_index++ )
	{
		if( libfsntfs_file_entry_get_attribute_by_index( file_entry, attribute_index, &attribute, error ) != 1 )
		{
			continue;
		}

		if( libfsntfs_attribute_get_type( attribute, &attribute_type, error ) == 1
		 && attribute_type == LIBFSNTFS_ATTRIBUTE_TYPE_FILE_NAME )
		{
			if( libfsntfs_file_name_attribute_get_utf8_name_size( attribute, &name_size, error ) == 1
			 && name_size > 0
			 && name_size <= out_name_size
			 && libfsntfs_file_name_attribute_get_utf8_name(
			     attribute, (uint8_t *) out_name, name_size, error ) == 1
			 && libfsntfs_file_name_attribute_get_parent_file_reference(
			     attribute, out_parent_file_reference, error ) == 1 )
			{
				found = 1;
			}
		}

		libfsntfs_attribute_free( &attribute, error );
	}

	return found;
}

int path_resolver_reconstruct_ancestor_path(
     libfsntfs_volume_t *volume,
     uint64_t parent_mft_index,
     char *out_path,
     size_t out_path_size,
     int *out_is_complete,
     libfsntfs_error_t **error )
{
	uint64_t current_index;
	int depth;

	if( volume == NULL || out_path == NULL || out_path_size == 0 || out_is_complete == NULL )
	{
		return -1;
	}

	out_path[ 0 ] = '\0';
	current_index = parent_mft_index;

	for( depth = 0; depth < PATH_RESOLVER_MAX_DEPTH; depth++ )
	{
		libfsntfs_file_entry_t *file_entry = NULL;
		char name[ 256 ];
		uint64_t parent_file_reference = 0;
		int get_name_result;

		if( current_index == NTFS_ROOT_DIRECTORY_MFT_INDEX )
		{
			*out_is_complete = 1;
			return 1;
		}

		if( libfsntfs_volume_get_file_entry_by_index( volume, current_index, &file_entry, error ) != 1 )
		{
			/* 这一层打不开（比如父目录自己也被删除、槽位已经被复用）——
			 * 路径重建到此为止，返回已经拼好的部分，不当成整体失败 */
			if( error != NULL && *error != NULL )
			{
				libfsntfs_error_free( error );
			}
			*out_is_complete = 0;
			return 1;
		}

		name[ 0 ] = '\0';
		get_name_result = get_name_and_parent_reference(
		                    file_entry, name, sizeof( name ), &parent_file_reference, error );

		if( get_name_result != 1 )
		{
			libfsntfs_file_entry_free( &file_entry, NULL );
			if( error != NULL && *error != NULL )
			{
				libfsntfs_error_free( error );
			}
			*out_is_complete = 0;
			return 1;
		}

		libfsntfs_file_entry_free( &file_entry, error );

		if( path_builder_prepend_segment( out_path, out_path_size, name ) != 1 )
		{
			/* 路径太长装不下——同样不当成整体失败，把目前拼到的内容
			 * 标记为不完整返回，调用方可以自行决定怎么展示 */
			*out_is_complete = 0;
			return 1;
		}

		current_index = ntfs_file_reference_to_mft_index( parent_file_reference );
	}

	/* 走到这里说明超过了防御性深度上限，大概率是损坏数据造成的环形引用 */
	*out_is_complete = 0;
	return 1;
}
