/*
 * Task #2 验收工具：按 MFT 索引逐条扫描（不走目录索引），
 * 找出 is_allocated() == false 但仍带 $FILE_NAME 的条目，
 * 并把它们的内容导出，用来验证"已删除但未覆盖"场景是否真的可被发现和恢复。
 *
 * 这不是 Task #3/#4 的正式实现（那部分要走 TDD），只是用来确认
 * Task #2 构造出的 fixture 确实可以被"按索引扫描+检查分配状态"这个
 * 真实恢复思路发现，为后面写测试提供信心。
 *
 * 用法：listdeleted <ntfs 镜像路径> <输出目录>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfsntfs.h>

static int dump_entry_content( libfsntfs_file_entry_t *file_entry, const char *out_path, libfsntfs_error_t **error )
{
	uint8_t buffer[ 4096 ];
	ssize_t read_count;
	FILE *output = fopen( out_path, "wb" );

	if( output == NULL )
	{
		return -1;
	}
	while( 1 )
	{
		read_count = libfsntfs_file_entry_read_buffer( file_entry, buffer, sizeof( buffer ), error );
		if( read_count == -1 )
		{
			fclose( output );
			return -1;
		}
		if( read_count == 0 )
		{
			break;
		}
		fwrite( buffer, 1, (size_t) read_count, output );
	}
	fclose( output );
	return 0;
}

int main( int argc, char *argv[] )
{
	libfsntfs_volume_t *volume         = NULL;
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;
	uint64_t number_of_entries = 0;
	uint64_t index             = 0;
	int is_allocated      = 0;
	int number_of_attrs   = 0;
	uint8_t name_buffer[ 256 ];
	size_t name_size = 0;
	int found_count  = 0;

	if( argc != 3 )
	{
		fprintf( stderr, "用法: %s <ntfs 镜像> <输出目录>\n", argv[ 0 ] );
		return EXIT_FAILURE;
	}

	if( libfsntfs_volume_initialize( &volume, &error ) != 1
	 || libfsntfs_volume_open( volume, argv[ 1 ], LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		fprintf( stderr, "无法打开卷: %s\n", argv[ 1 ] );
		goto on_error;
	}

	if( libfsntfs_volume_get_number_of_file_entries( volume, &number_of_entries, &error ) != 1 )
	{
		fprintf( stderr, "无法获取 MFT 条目总数\n" );
		goto on_error;
	}

	printf( "MFT 条目总数: %llu，开始按索引逐条扫描（不依赖目录结构）...\n", (unsigned long long) number_of_entries );

	for( index = 0; index < number_of_entries; index++ )
	{
		if( libfsntfs_volume_get_file_entry_by_index( volume, index, &file_entry, &error ) != 1 )
		{
			/* 有些索引本身不可读（从未使用的槽位），跳过，这是正常情况 */
			libfsntfs_error_free( &error );
			continue;
		}

		is_allocated = libfsntfs_file_entry_is_allocated( file_entry, &error );

		if( is_allocated != 0 )
		{
			libfsntfs_file_entry_free( &file_entry, &error );
			continue;
		}

		if( libfsntfs_file_entry_get_number_of_attributes( file_entry, &number_of_attrs, &error ) != 1
		 || number_of_attrs == 0 )
		{
			libfsntfs_file_entry_free( &file_entry, &error );
			continue;
		}

		/* 按索引拿到的 file_entry 没有"查找路径"，libfsntfs_file_entry_get_utf8_name
		 * 这类函数拿不到名字；真正的文件名要从 $FILE_NAME 属性里单独取。 */
		name_size = 0;

		{
			int attribute_index;
			for( attribute_index = 0; attribute_index < number_of_attrs; attribute_index++ )
			{
				libfsntfs_attribute_t *attribute = NULL;
				uint32_t attribute_type = 0;

				if( libfsntfs_file_entry_get_attribute_by_index( file_entry, attribute_index, &attribute, &error ) != 1 )
				{
					continue;
				}
				if( libfsntfs_attribute_get_type( attribute, &attribute_type, &error ) == 1
				 && attribute_type == LIBFSNTFS_ATTRIBUTE_TYPE_FILE_NAME )
				{
					if( libfsntfs_file_name_attribute_get_utf8_name_size( attribute, &name_size, &error ) == 1
					 && name_size > 0 && name_size <= sizeof( name_buffer ) )
					{
						libfsntfs_file_name_attribute_get_utf8_name( attribute, name_buffer, name_size, &error );
					}
				}
				libfsntfs_attribute_free( &attribute, &error );

				if( name_size > 0 )
				{
					break;
				}
			}
		}

		if( name_size == 0 )
		{
			libfsntfs_file_entry_free( &file_entry, &error );
			continue;
		}

		printf( "发现已删除但未覆盖的条目: index=%llu  文件名=%s\n", (unsigned long long) index, (char *) name_buffer );

		{
			char out_path[ 512 ];
			snprintf( out_path, sizeof( out_path ), "%s/recovered_%llu_%s", argv[ 2 ], (unsigned long long) index, (char *) name_buffer );
			if( dump_entry_content( file_entry, out_path, &error ) == 0 )
			{
				printf( "  -> 内容已恢复到: %s\n", out_path );
				found_count++;
			}
			else
			{
				printf( "  -> 内容恢复失败\n" );
			}
		}

		libfsntfs_file_entry_free( &file_entry, &error );
	}

	printf( "扫描完成，共恢复 %d 个已删除文件\n", found_count );

	libfsntfs_volume_close( volume, &error );
	libfsntfs_volume_free( &volume, &error );
	return EXIT_SUCCESS;

on_error:
	if( error != NULL )
	{
		libfsntfs_error_backtrace_fprint( error, stderr );
		libfsntfs_error_free( &error );
	}
	if( volume != NULL )
	{
		libfsntfs_volume_free( &volume, NULL );
	}
	return EXIT_FAILURE;
}
