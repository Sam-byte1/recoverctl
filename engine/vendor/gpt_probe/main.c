/* 探测工具：验证 libvsgpt 能不能正确解析我们手工构造的 GPT 测试镜像 */
#include <stdio.h>
#include <stdlib.h>

#include <libvsgpt.h>

int main( int argc, char *argv[] )
{
	libvsgpt_volume_t *volume = NULL;
	libvsgpt_error_t *error   = NULL;
	int number_of_partitions  = 0;
	int i;

	if( argc != 2 )
	{
		fprintf( stderr, "usage: %s <image>\n", argv[0] );
		return 1;
	}

	if( libvsgpt_volume_initialize( &volume, &error ) != 1 )
	{
		fprintf( stderr, "init failed\n" );
		goto on_error;
	}
	if( libvsgpt_volume_open( volume, argv[1], LIBVSGPT_OPEN_READ, &error ) != 1 )
	{
		fprintf( stderr, "open failed\n" );
		goto on_error;
	}

	if( libvsgpt_volume_get_number_of_partitions( volume, &number_of_partitions, &error ) != 1 )
	{
		fprintf( stderr, "get_number_of_partitions failed\n" );
		goto on_error;
	}
	printf( "number_of_partitions = %d\n", number_of_partitions );

	for( i = 0; i < number_of_partitions; i++ )
	{
		libvsgpt_partition_t *partition = NULL;
		off64_t offset = 0;
		size64_t size = 0;
		uint8_t guid[16];
		int j;

		if( libvsgpt_volume_get_partition_by_index( volume, i, &partition, &error ) != 1 )
		{
			fprintf( stderr, "get_partition_by_index(%d) failed\n", i );
			goto on_error;
		}
		libvsgpt_partition_get_volume_offset( partition, &offset, &error );
		libvsgpt_partition_get_size( partition, &size, &error );
		libvsgpt_partition_get_type_identifier( partition, guid, sizeof(guid), &error );

		printf( "partition %d: offset=%lld size=%llu type_guid=", i, (long long) offset, (unsigned long long) size );
		for( j = 0; j < 16; j++ ) printf( "%02x", guid[j] );
		printf( "\n" );

		libvsgpt_partition_free( &partition, &error );
	}

	libvsgpt_volume_close( volume, &error );
	libvsgpt_volume_free( &volume, &error );
	return 0;

on_error:
	if( error != NULL )
	{
		libvsgpt_error_backtrace_fprint( error, stderr );
		libvsgpt_error_free( &error );
	}
	return 1;
}
