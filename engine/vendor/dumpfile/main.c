/*
 * Task #2 辅助工具：在"删除"一个文件之前，先把它的原始内容导出到本地文件。
 * 用途：记录 fixture 里被删除文件的"标准答案"（原始内容/哈希），
 * 之后对比恢复出来的内容是否一致。
 *
 * 用法：dumpfile <ntfs 镜像路径> <卷内文件路径，如 passwords.txt> <输出文件路径>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfsntfs.h>

int main( int argc, char *argv[] )
{
	libfsntfs_volume_t *volume         = NULL;
	libfsntfs_file_entry_t *file_entry = NULL;
	libfsntfs_error_t *error           = NULL;
	FILE *output                       = NULL;
	uint8_t buffer[ 4096 ];
	size64_t file_size = 0;
	ssize_t read_count  = 0;
	int result = 0;

	if( argc != 4 )
	{
		fprintf( stderr, "用法: %s <ntfs 镜像> <卷内路径> <输出文件>\n", argv[ 0 ] );
		return EXIT_FAILURE;
	}

	if( libfsntfs_volume_initialize( &volume, &error ) != 1 )
	{
		fprintf( stderr, "无法初始化 volume 对象\n" );
		goto on_error;
	}

	if( libfsntfs_volume_open( volume, argv[ 1 ], LIBFSNTFS_OPEN_READ, &error ) != 1 )
	{
		fprintf( stderr, "无法打开卷: %s\n", argv[ 1 ] );
		goto on_error;
	}

	result = libfsntfs_volume_get_file_entry_by_utf8_path(
	          volume,
	          (uint8_t *) argv[ 2 ],
	          strlen( argv[ 2 ] ),
	          &file_entry,
	          &error );

	if( result != 1 )
	{
		fprintf( stderr, "找不到文件: %s\n", argv[ 2 ] );
		goto on_error;
	}

	if( libfsntfs_file_entry_get_size( file_entry, &file_size, &error ) != 1 )
	{
		fprintf( stderr, "读取文件大小失败\n" );
		goto on_error;
	}

	output = fopen( argv[ 3 ], "wb" );

	if( output == NULL )
	{
		fprintf( stderr, "无法创建输出文件: %s\n", argv[ 3 ] );
		goto on_error;
	}

	while( 1 )
	{
		read_count = libfsntfs_file_entry_read_buffer( file_entry, buffer, sizeof( buffer ), &error );

		if( read_count == -1 )
		{
			fprintf( stderr, "读取文件内容失败\n" );
			fclose( output );
			goto on_error;
		}
		if( read_count == 0 )
		{
			break;
		}
		fwrite( buffer, 1, (size_t) read_count, output );
	}

	fclose( output );

	libfsntfs_file_entry_free( &file_entry, &error );
	libfsntfs_volume_close( volume, &error );
	libfsntfs_volume_free( &volume, &error );

	printf( "已导出 %s (%llu 字节) -> %s\n", argv[ 2 ], (unsigned long long) file_size, argv[ 3 ] );
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
	return EXIT_FAILURE;
}
