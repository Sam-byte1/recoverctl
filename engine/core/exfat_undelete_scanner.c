#include <stdlib.h>
#include <string.h>

#include "exfat_dir_entry.h"
#include "exfat_undelete_scanner.h"

#define EXFAT_DIR_ENTRY_SIZE 32

static int scan_result_append(
     exfat_undelete_scan_result_t *result,
     const exfat_dir_entry_set_t *entry_set,
     const char *directory_path )
{
	exfat_undelete_candidate_t *candidate;

	if( result->count == result->capacity )
	{
		int new_capacity = result->capacity == 0 ? 8 : result->capacity * 2;
		exfat_undelete_candidate_t *new_array = (exfat_undelete_candidate_t *) realloc(
		                                           result->candidates,
		                                           (size_t) new_capacity * sizeof( exfat_undelete_candidate_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		result->candidates = new_array;
		result->capacity   = new_capacity;
	}

	candidate = &result->candidates[ result->count ];

	strncpy( candidate->name, entry_set->name, sizeof( candidate->name ) - 1 );
	candidate->name[ sizeof( candidate->name ) - 1 ] = '\0';
	candidate->first_cluster = entry_set->first_cluster;
	candidate->file_size     = entry_set->file_size;
	candidate->no_fat_chain  = entry_set->no_fat_chain;

	strncpy( candidate->directory_path, directory_path, sizeof( candidate->directory_path ) - 1 );
	candidate->directory_path[ sizeof( candidate->directory_path ) - 1 ] = '\0';

	result->count++;

	return 1;
}

int exfat_undelete_scanner_scan_directory(
     const uint8_t *directory_data,
     size_t directory_data_size,
     const char *directory_path,
     exfat_undelete_scan_result_t *out )
{
	size_t entry_index;
	size_t total_entries;
	exfat_dir_entry_set_t entry_set;

	if( directory_data == NULL || directory_path == NULL || out == NULL )
	{
		return -1;
	}
	out->candidates = NULL;
	out->count      = 0;
	out->capacity   = 0;

	total_entries = directory_data_size / EXFAT_DIR_ENTRY_SIZE;

	for( entry_index = 0; entry_index < total_entries; )
	{
		if( exfat_dir_entry_set_parse( directory_data, directory_data_size, entry_index, &entry_set ) != 1 )
		{
			/* 结构明显损坏到连 1 条都解析不出来——按 FAT32 那边同样的保守
			 * 做法：停止扫描而不是硬着头皮继续，避免对损坏数据做出错误判断 */
			break;
		}

		if( entry_set.kind == EXFAT_DIR_ENTRY_SET_KIND_END_OF_DIRECTORY )
		{
			break;
		}

		if( entry_set.kind == EXFAT_DIR_ENTRY_SET_KIND_DELETED_FILE
		 && entry_set.first_cluster != 0
		 && !entry_set.is_directory )
		{
			/* 起始簇号 0 的已删除条目、以及已删除的目录（本轮范围只处理
			 * 文件，已删除的子目录本身不递归，是明确的后续增量）不计入候选项 */
			if( scan_result_append( out, &entry_set, directory_path ) != 1 )
			{
				exfat_undelete_scan_result_free( out );
				return -1;
			}
		}

		if( entry_set.consumed_entries == 0 )
		{
			/* 防御性处理：理论上不应该发生（exfat_dir_entry_set_parse 保证
			 * 至少消费 1 条），但如果真的出现，避免死循环 */
			break;
		}
		entry_index += entry_set.consumed_entries;
	}

	return 1;
}

static int subdirectory_list_append(
     exfat_subdirectory_list_t *list,
     const exfat_dir_entry_set_t *entry_set )
{
	if( list->count == list->capacity )
	{
		int new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
		exfat_subdirectory_t *new_array = (exfat_subdirectory_t *) realloc(
		                                     list->entries,
		                                     (size_t) new_capacity * sizeof( exfat_subdirectory_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		list->entries  = new_array;
		list->capacity = new_capacity;
	}

	strncpy( list->entries[ list->count ].name, entry_set->name,
	         sizeof( list->entries[ list->count ].name ) - 1 );
	list->entries[ list->count ].name[ sizeof( list->entries[ list->count ].name ) - 1 ] = '\0';
	list->entries[ list->count ].first_cluster = entry_set->first_cluster;
	list->entries[ list->count ].file_size     = entry_set->file_size;
	list->entries[ list->count ].no_fat_chain  = entry_set->no_fat_chain;
	list->count++;

	return 1;
}

int exfat_undelete_scanner_find_subdirectories(
     const uint8_t *directory_data,
     size_t directory_data_size,
     exfat_subdirectory_list_t *out )
{
	size_t entry_index;
	size_t total_entries;
	exfat_dir_entry_set_t entry_set;

	if( directory_data == NULL || out == NULL )
	{
		return -1;
	}
	out->entries  = NULL;
	out->count    = 0;
	out->capacity = 0;

	total_entries = directory_data_size / EXFAT_DIR_ENTRY_SIZE;

	for( entry_index = 0; entry_index < total_entries; )
	{
		if( exfat_dir_entry_set_parse( directory_data, directory_data_size, entry_index, &entry_set ) != 1 )
		{
			break;
		}

		if( entry_set.kind == EXFAT_DIR_ENTRY_SET_KIND_END_OF_DIRECTORY )
		{
			break;
		}

		if( entry_set.kind == EXFAT_DIR_ENTRY_SET_KIND_NORMAL_FILE && entry_set.is_directory )
		{
			/* exFAT 没有 FAT32 那种 "." "..' 自引用/父引用条目，不需要
			 * 额外过滤——每一条正常、标了目录属性的 entry set 都是真正的
			 * 子目录 */
			if( subdirectory_list_append( out, &entry_set ) != 1 )
			{
				exfat_subdirectory_list_free( out );
				return -1;
			}
		}

		if( entry_set.consumed_entries == 0 )
		{
			break;
		}
		entry_index += entry_set.consumed_entries;
	}

	return 1;
}

void exfat_undelete_scan_result_free(
      exfat_undelete_scan_result_t *result )
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

void exfat_subdirectory_list_free(
      exfat_subdirectory_list_t *list )
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

int exfat_undelete_scan_result_append_all(
     exfat_undelete_scan_result_t *dest,
     const exfat_undelete_scan_result_t *src )
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
			exfat_undelete_candidate_t *new_array = (exfat_undelete_candidate_t *) realloc(
			                                           dest->candidates,
			                                           (size_t) new_capacity * sizeof( exfat_undelete_candidate_t ) );

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
