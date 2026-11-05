/*
 * Task #1 冒烟测试：验证 libfsntfs 能被我们自己的代码正确调用。
 *
 * 用法：smoketest <ntfs 镜像/卷路径>
 * 行为：打开该 NTFS 卷，打印卷标（$VOLUME_NAME），成功退出 0，失败退出 1。
 *
 * 这不是产品代码，只是 Task #1 的验证程序，确认：
 * 1. libfsntfs 能在 Mac 上编译出来
 * 2. 我们自己的 C 代码能链接它、调用它的公开 API 拿到正确结果
 * 后续 engine/core 里的真实解析逻辑会用同样的方式引用这个库。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfsntfs.h>

int main( int argc, char *argv[] )
{
	libfsntfs_volume_t *volume = NULL;
	libfsntfs_error_t *error   = NULL;
	uint8_t name_buffer[ 256 ];
	size_t name_size = 0;
	int result = 0;

	if( argc != 2 )
	{
		fprintf( stderr, "用法: %s <ntfs 卷镜像路径>\n", argv[ 0 ] );
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

	result = libfsntfs_volume_get_utf8_name_size( volume, &name_size, &error );

	if( result == -1 )
	{
		fprintf( stderr, "读取卷标大小失败\n" );
		goto on_error;
	}
	else if( result == 0 || name_size == 0 || name_size > sizeof( name_buffer ) )
	{
		printf( "卷标: (该卷没有设置 $VOLUME_NAME，这是正常情况)\n" );
	}
	else
	{
		if( libfsntfs_volume_get_utf8_name( volume, name_buffer, name_size, &error ) != 1 )
		{
			fprintf( stderr, "读取卷标内容失败\n" );
			goto on_error;
		}
		printf( "卷标: %s\n", (char *) name_buffer );
	}

	if( libfsntfs_volume_close( volume, &error ) != 0 )
	{
		fprintf( stderr, "关闭卷失败\n" );
		goto on_error;
	}

	libfsntfs_volume_free( &volume, &error );

	printf( "libfsntfs 冒烟测试通过：能打开卷、能读取元数据。\n" );
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
