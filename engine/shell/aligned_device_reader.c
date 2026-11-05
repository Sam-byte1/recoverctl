#include <stdio.h>
#include <string.h>

#include "../core/bad_sector_policy.h"
#include "../core/sector_aligned_read.h"
#include "aligned_device_reader.h"

#define ALIGNED_DEVICE_READER_BUFFER_SIZE 4096

int aligned_device_reader_copy_to_file(
     aligned_device_read_fn read_fn,
     void *context,
     size_t sector_size,
     uint64_t total_size,
     uint32_t max_retries,
     bad_sector_log_t *out_bad_sector_log,
     const char *output_path )
{
	FILE *output;
	uint8_t buffer[ ALIGNED_DEVICE_READER_BUFFER_SIZE ];
	uint64_t remaining;
	uint64_t stream_offset;

	if( read_fn == NULL || output_path == NULL )
	{
		return -1;
	}

	output = fopen( output_path, "wb" );
	if( output == NULL )
	{
		return -1;
	}

	remaining      = total_size;
	stream_offset  = 0;

	while( remaining > 0 )
	{
		size_t requested = remaining < sizeof( buffer ) ? (size_t) remaining : sizeof( buffer );
		size_t aligned_requested = sector_aligned_read_compute_request_size(
		                             requested, sector_size, sizeof( buffer ) );
		size_t usable;
		uint32_t attempt_number;

		attempt_number = 1;
		while( 1 )
		{
			size_t actually_read = read_fn( context, buffer, aligned_requested );

			usable = sector_aligned_read_usable_bytes( requested, actually_read );
			if( usable > 0 )
			{
				break;
			}
			if( bad_sector_policy_decide( attempt_number, max_retries ) == BAD_SECTOR_ACTION_RETRY )
			{
				attempt_number++;
				continue;
			}
			break;  /* 重试用完，跳出去交给下面统一处理"这一块到底是失败还是要跳过" */
		}

		if( usable == 0 )
		{
			if( out_bad_sector_log == NULL )
			{
				/* 没有启用坏道容忍——保持和原来完全一样的行为：直接失败 */
				fclose( output );
				return -1;
			}

			/* 跳过这一块：用 0 填充写出去，保持后续内容的偏移量不变
			 * （否则后面读出来的数据会整体错位，比空白更具误导性），
			 * 记一笔坏道日志，然后继续处理下一块，不中断整个拷贝 */
			memset( buffer, 0, requested );
			if( fwrite( buffer, 1, requested, output ) != requested )
			{
				fclose( output );
				return -1;
			}
			if( bad_sector_log_append( out_bad_sector_log, stream_offset, (uint64_t) requested ) != 1 )
			{
				fclose( output );
				return -1;
			}
			remaining     -= requested;
			stream_offset += requested;
			continue;
		}

		if( fwrite( buffer, 1, usable, output ) != usable )
		{
			fclose( output );
			return -1;
		}
		remaining     -= usable;
		stream_offset += usable;
	}

	fclose( output );
	return 1;
}

int aligned_device_reader_read_block(
     aligned_device_read_fn read_fn,
     void *context,
     uint8_t *buffer,
     size_t block_size,
     uint32_t max_retries,
     uint64_t block_offset_for_log,
     bad_sector_log_t *out_bad_sector_log )
{
	uint32_t attempt_number;

	if( read_fn == NULL || buffer == NULL || block_size == 0 )
	{
		return -1;
	}

	attempt_number = 1;
	while( 1 )
	{
		size_t actually_read = read_fn( context, buffer, block_size );

		if( actually_read == block_size )
		{
			return 1;
		}
		if( bad_sector_policy_decide( attempt_number, max_retries ) == BAD_SECTOR_ACTION_RETRY )
		{
			attempt_number++;
			continue;
		}
		break;  /* 重试用完 */
	}

	if( out_bad_sector_log == NULL )
	{
		return -1;
	}

	memset( buffer, 0, block_size );
	if( bad_sector_log_append( out_bad_sector_log, block_offset_for_log, (uint64_t) block_size ) != 1 )
	{
		return -1;
	}
	return 1;
}
