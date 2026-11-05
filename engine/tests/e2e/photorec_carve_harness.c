/*
 * D2 e2e harness（P4，2026-11-05 第28步排期）：不是单元测试框架下的 mock
 * 测试——`photorec_invoker.c` 是真正拉起真实 `photorec` 子进程的 shell
 * 层代码，只能靠端到端跑一遍真实二进制 + 哈希校验来验证，和
 * `ntfs_bfio_adapter`/`file_writer` 的测试哲学一致。
 *
 * 用法：photorec_carve_harness <photorec可执行文件路径> <目标镜像路径> <输出目录base>
 * 跑完把解析出的 report.xml 条目以 "filename|filesize|img_offset" 的
 * 格式逐行打印到 stdout，供外层 shell 脚本做断言；额外打印
 * "RECUP_DIR=<路径>" 一行，方便 shell 脚本直接拿到恢复文件所在目录去做
 * 哈希比对。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../shell/photorec_invoker.h"
#include "../../core/photorec_report_parser.h"

static int read_whole_file( const char *path, char **out_data, size_t *out_size )
{
	FILE *f = fopen( path, "rb" );
	long file_size;
	char *buf;

	if( f == NULL )
	{
		return -1;
	}
	if( fseek( f, 0, SEEK_END ) != 0 )
	{
		fclose( f );
		return -1;
	}
	file_size = ftell( f );
	if( file_size < 0 )
	{
		fclose( f );
		return -1;
	}
	rewind( f );

	buf = (char *) malloc( (size_t) file_size + 1 );
	if( buf == NULL )
	{
		fclose( f );
		return -1;
	}
	if( fread( buf, 1, (size_t) file_size, f ) != (size_t) file_size )
	{
		free( buf );
		fclose( f );
		return -1;
	}
	fclose( f );
	buf[ file_size ] = '\0';

	*out_data = buf;
	*out_size = (size_t) file_size;
	return 0;
}

int main( int argc, char **argv )
{
	photorec_invoke_result_t invoke_result;
	char report_path[ 1600 ];
	char *xml_data = NULL;
	size_t xml_size = 0;
	photorec_report_t report;
	int i;

	if( argc != 4 )
	{
		fprintf( stderr, "usage: %s <photorec_bin> <target_image> <output_dir_base>\n", argv[ 0 ] );
		return 2;
	}

	if( photorec_invoker_run( argv[ 1 ], argv[ 2 ], "partition_none", argv[ 3 ], &invoke_result ) != 1 )
	{
		fprintf( stderr, "photorec_invoker_run failed to launch/wait\n" );
		return 2;
	}
	printf( "EXIT_CODE=%d\n", invoke_result.exit_code );
	printf( "RECUP_DIR=%s\n", invoke_result.recup_dir_path );

	if( invoke_result.recup_dir_path[ 0 ] == '\0' )
	{
		printf( "ENTRY_COUNT=0\n" );
		return 0;
	}
	snprintf( report_path, sizeof( report_path ), "%s/report.xml", invoke_result.recup_dir_path );

	if( read_whole_file( report_path, &xml_data, &xml_size ) != 0 )
	{
		fprintf( stderr, "failed to read %s\n", report_path );
		return 2;
	}
	memset( &report, 0, sizeof( report ) );
	if( photorec_report_parser_parse( xml_data, xml_size, &report ) != 1 )
	{
		fprintf( stderr, "failed to parse report.xml\n" );
		free( xml_data );
		return 2;
	}
	printf( "ENTRY_COUNT=%d\n", report.count );
	for( i = 0; i < report.count; i++ )
	{
		printf( "ENTRY|%s|%llu|%llu\n",
		        report.entries[ i ].name,
		        (unsigned long long) report.entries[ i ].file_size,
		        (unsigned long long) report.entries[ i ].img_offset );
	}
	photorec_report_free( &report );
	free( xml_data );
	return 0;
}
