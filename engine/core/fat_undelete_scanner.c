#include <stdlib.h>
#include <string.h>

#include "fat_dir_entry.h"
#include "fat_undelete_scanner.h"
#include "utf16_to_utf8.h"

#define FAT_DIR_ENTRY_SIZE 32

/* VFAT 规范允许的最长文件名是 255 个字符，每条长文件名目录项装 13 个
 * 字符，255/13 向上取整是 20——这是缓冲区大小的来源，不是拍脑袋定的。
 * 真实数据超过这个数量极少见，万一出现就丢弃多出来的片段、退化成
 * 用短文件名显示，不会崩溃或者越界写。 */
#define FAT_LONG_NAME_MAX_PARTS 20

typedef struct
{
	uint8_t parts[ FAT_LONG_NAME_MAX_PARTS ][ 26 ];
	int count;
}
long_name_buffer_t;

static void long_name_buffer_push( long_name_buffer_t *buffer, const uint8_t *part_utf16le )
{
	if( buffer->count >= FAT_LONG_NAME_MAX_PARTS )
	{
		return;
	}
	memcpy( buffer->parts[ buffer->count ], part_utf16le, 26 );
	buffer->count++;
}

/* 把缓冲的长文件名片段按"物理顺序倒序拼接"组装成完整字符串。倒序的
 * 原因见 fat_dir_entry.h 的说明：VFAT 规范里，紧邻短名条目的那个长
 * 文件名片段对应文件名最前面的部分，远离短名条目的片段对应文件名
 * 末尾的部分——这和目录里从前往后扫描遇到它们的顺序正好相反。 */
static void assemble_long_name( const long_name_buffer_t *buffer, char *out_name, size_t out_name_size )
{
	uint8_t utf16le_bytes[ FAT_LONG_NAME_MAX_PARTS * 26 ];
	size_t byte_count = 0;
	int part_index;

	for( part_index = buffer->count - 1; part_index >= 0; part_index-- )
	{
		int char_index;
		int stopped = 0;

		for( char_index = 0; char_index < 13; char_index++ )
		{
			uint8_t low_byte  = buffer->parts[ part_index ][ char_index * 2 ];
			uint8_t high_byte = buffer->parts[ part_index ][ char_index * 2 + 1 ];

			if( low_byte == 0x00 && high_byte == 0x00 )
			{
				/* 文件名在这里结束（VFAT 规范：名字以一个 0x0000 字符结尾，
				 * 之后可能跟着 0xFFFF 填充，不是文件名的一部分） */
				stopped = 1;
				break;
			}
			utf16le_bytes[ byte_count ]     = low_byte;
			utf16le_bytes[ byte_count + 1 ] = high_byte;
			byte_count += 2;
		}

		if( stopped )
		{
			break;
		}
	}

	utf16le_to_utf8( utf16le_bytes, byte_count / 2, out_name, out_name_size );
}

/* 决定一个候选项/子目录最终应该显示的名字：有缓冲到长文件名片段就用
 * 重组后的长文件名，没有就退回 8.3 短文件名。 */
static void resolve_display_name(
     const fat_dir_entry_t *entry,
     const long_name_buffer_t *long_name_buffer,
     char *out_name,
     size_t out_name_size )
{
	if( long_name_buffer->count > 0 )
	{
		assemble_long_name( long_name_buffer, out_name, out_name_size );
	}
	else
	{
		strncpy( out_name, entry->short_name, out_name_size - 1 );
		out_name[ out_name_size - 1 ] = '\0';
	}
}

static int scan_result_append(
     fat_undelete_scan_result_t *result,
     const char *name,
     const fat_dir_entry_t *entry,
     const char *directory_path )
{
	fat_undelete_candidate_t *candidate;

	if( result->count == result->capacity )
	{
		int new_capacity = result->capacity == 0 ? 8 : result->capacity * 2;
		fat_undelete_candidate_t *new_array = (fat_undelete_candidate_t *) realloc(
		                                        result->candidates,
		                                        (size_t) new_capacity * sizeof( fat_undelete_candidate_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		result->candidates = new_array;
		result->capacity   = new_capacity;
	}

	candidate = &result->candidates[ result->count ];

	strncpy( candidate->name, name, sizeof( candidate->name ) - 1 );
	candidate->name[ sizeof( candidate->name ) - 1 ] = '\0';
	candidate->first_cluster = entry->first_cluster;
	candidate->file_size     = entry->file_size;

	strncpy( candidate->directory_path, directory_path, sizeof( candidate->directory_path ) - 1 );
	candidate->directory_path[ sizeof( candidate->directory_path ) - 1 ] = '\0';

	result->count++;

	return 1;
}

int fat_undelete_scanner_scan_directory(
     const uint8_t *directory_data,
     size_t directory_data_size,
     const char *directory_path,
     fat_undelete_scan_result_t *out )
{
	size_t offset;
	fat_dir_entry_t entry;
	long_name_buffer_t long_name_buffer;

	if( directory_data == NULL || directory_path == NULL || out == NULL )
	{
		return -1;
	}
	out->candidates = NULL;
	out->count      = 0;
	out->capacity   = 0;
	long_name_buffer.count = 0;

	for( offset = 0; offset + FAT_DIR_ENTRY_SIZE <= directory_data_size; offset += FAT_DIR_ENTRY_SIZE )
	{
		if( fat_dir_entry_parse( directory_data + offset, FAT_DIR_ENTRY_SIZE, &entry ) != 1 )
		{
			continue;
		}

		if( entry.kind == FAT_DIR_ENTRY_KIND_END_OF_DIRECTORY )
		{
			break;
		}

		if( entry.kind == FAT_DIR_ENTRY_KIND_LONG_NAME_PART )
		{
			long_name_buffer_push( &long_name_buffer, entry.long_name_part_utf16le );
			continue;
		}

		if( entry.kind != FAT_DIR_ENTRY_KIND_DELETED )
		{
			/* 不是已删除条目：缓冲到的长文件名片段（如果有）不属于任何
			 * 我们要收集的候选项，清空掉，避免"脏"片段串到下一个真正
			 * 已删除文件的长文件名里 */
			long_name_buffer.count = 0;
			continue;
		}

		/* 已删除但起始簇号是 0 的条目，大概率是从未真正写过数据的垃圾槽位
		 * （比如目录区里从未被用到过、恰好首字节凑巧是 0xE5 的全零区域），
		 * 不是真实被删除过的文件，不计入候选项——这个判断和 NTFS 那边
		 * "整条全零的槽位不算已删除文件"是同一个思路的 FAT 版本 */
		if( entry.first_cluster != 0 )
		{
			char display_name[ 256 ];

			resolve_display_name( &entry, &long_name_buffer, display_name, sizeof( display_name ) );

			if( scan_result_append( out, display_name, &entry, directory_path ) != 1 )
			{
				fat_undelete_scan_result_free( out );
				return -1;
			}
		}

		long_name_buffer.count = 0;
	}

	return 1;
}

static int subdirectory_list_append(
     fat_subdirectory_list_t *list,
     const char *name,
     const fat_dir_entry_t *entry )
{
	if( list->count == list->capacity )
	{
		int new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
		fat_subdirectory_t *new_array = (fat_subdirectory_t *) realloc(
		                                  list->entries,
		                                  (size_t) new_capacity * sizeof( fat_subdirectory_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		list->entries  = new_array;
		list->capacity = new_capacity;
	}

	strncpy( list->entries[ list->count ].name, name, sizeof( list->entries[ list->count ].name ) - 1 );
	list->entries[ list->count ].name[ sizeof( list->entries[ list->count ].name ) - 1 ] = '\0';
	list->entries[ list->count ].first_cluster = entry->first_cluster;
	list->count++;

	return 1;
}

int fat_undelete_scanner_find_subdirectories(
     const uint8_t *directory_data,
     size_t directory_data_size,
     fat_subdirectory_list_t *out )
{
	size_t offset;
	fat_dir_entry_t entry;
	long_name_buffer_t long_name_buffer;

	if( directory_data == NULL || out == NULL )
	{
		return -1;
	}
	out->entries  = NULL;
	out->count    = 0;
	out->capacity = 0;
	long_name_buffer.count = 0;

	for( offset = 0; offset + FAT_DIR_ENTRY_SIZE <= directory_data_size; offset += FAT_DIR_ENTRY_SIZE )
	{
		if( fat_dir_entry_parse( directory_data + offset, FAT_DIR_ENTRY_SIZE, &entry ) != 1 )
		{
			continue;
		}

		if( entry.kind == FAT_DIR_ENTRY_KIND_END_OF_DIRECTORY )
		{
			break;
		}

		if( entry.kind == FAT_DIR_ENTRY_KIND_LONG_NAME_PART )
		{
			long_name_buffer_push( &long_name_buffer, entry.long_name_part_utf16le );
			continue;
		}

		if( entry.kind != FAT_DIR_ENTRY_KIND_NORMAL || !entry.is_directory )
		{
			long_name_buffer.count = 0;
			continue;
		}

		/* "." 和 ".." 是目录自引用/父引用，不是真正的子目录（也从来不会有
		 * VFAT 长文件名，这个判断看短文件名本身就够了）——不跳过的话
		 * "." 会导致对自己无限递归，".." 在 FAT32 里父目录是根目录时
		 * 固定填 0，不是一个合法的可读取簇号，两者都必须排除 */
		if( strcmp( entry.short_name, "." ) == 0 || strcmp( entry.short_name, ".." ) == 0 )
		{
			long_name_buffer.count = 0;
			continue;
		}

		{
			char display_name[ 256 ];

			resolve_display_name( &entry, &long_name_buffer, display_name, sizeof( display_name ) );

			if( subdirectory_list_append( out, display_name, &entry ) != 1 )
			{
				fat_subdirectory_list_free( out );
				return -1;
			}
		}

		long_name_buffer.count = 0;
	}

	return 1;
}

void fat_undelete_scan_result_free(
      fat_undelete_scan_result_t *result )
{
	if( result == NULL )
	{
		return;
	}
	free( result->candidates );
	result->candidates = NULL;
	result->count      = 0;
	result->capacity   = 0;
}

void fat_subdirectory_list_free(
      fat_subdirectory_list_t *list )
{
	if( list == NULL )
	{
		return;
	}
	free( list->entries );
	list->entries  = NULL;
	list->count    = 0;
	list->capacity = 0;
}

int fat_undelete_scan_result_append_all(
     fat_undelete_scan_result_t *dest,
     const fat_undelete_scan_result_t *src )
{
	int i;

	if( dest == NULL || src == NULL )
	{
		return -1;
	}

	for( i = 0; i < src->count; i++ )
	{
		if( dest->count == dest->capacity )
		{
			int new_capacity = dest->capacity == 0 ? 8 : dest->capacity * 2;
			fat_undelete_candidate_t *new_array = (fat_undelete_candidate_t *) realloc(
			                                        dest->candidates,
			                                        (size_t) new_capacity * sizeof( fat_undelete_candidate_t ) );

			if( new_array == NULL )
			{
				return -1;
			}
			dest->candidates = new_array;
			dest->capacity   = new_capacity;
		}

		dest->candidates[ dest->count ] = src->candidates[ i ];
		dest->count++;
	}

	return 1;
}
