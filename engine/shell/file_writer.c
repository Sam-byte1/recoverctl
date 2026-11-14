#include <stdio.h>
#include <string.h>

#include "file_writer.h"

#define FILE_WRITER_BUFFER_SIZE 4096

int file_writer_write_entry_content(
     libfsntfs_file_entry_t *file_entry,
     const char *stream_name,
     const char *output_path,
     libfsntfs_error_t **error )
{
	uint8_t buffer[ FILE_WRITER_BUFFER_SIZE ];
	libfsntfs_data_stream_t *data_stream = NULL;
	ssize_t read_count = 0;
	FILE *output = NULL;
	int return_code = 1;

	if( file_entry == NULL || output_path == NULL )
	{
		return -1;
	}

	/* stream_name非NULL时先定位具名备用数据流（ADS）对象，定位失败
	 * （包括这个名字的ADS压根不存在）就直接返回-1，不要退化成"悄悄
	 * 写出主数据流"——那样会让调用方以为恢复的是ADS内容，实际上
	 * 拿到的是完全不相关的主数据流字节，比明确报错更危险。 */
	if( stream_name != NULL )
	{
		if( libfsntfs_file_entry_get_alternate_data_stream_by_utf8_name(
		     file_entry,
		     (const uint8_t *) stream_name,
		     strlen( stream_name ),
		     &data_stream,
		     error ) != 1 )
		{
			return -1;
		}
	}

	output = fopen( output_path, "wb" );

	if( output == NULL )
	{
		if( data_stream != NULL )
		{
			libfsntfs_data_stream_free( &data_stream, NULL );
		}
		return -1;
	}

	while( 1 )
	{
		if( data_stream != NULL )
		{
			read_count = libfsntfs_data_stream_read_buffer( data_stream, buffer, sizeof( buffer ), error );
		}
		else
		{
			read_count = libfsntfs_file_entry_read_buffer( file_entry, buffer, sizeof( buffer ), error );
		}

		if( read_count == -1 )
		{
			return_code = -1;
			break;
		}
		if( read_count == 0 )
		{
			break;
		}
		if( fwrite( buffer, 1, (size_t) read_count, output ) != (size_t) read_count )
		{
			return_code = -1;
			break;
		}
	}

	fclose( output );

	if( data_stream != NULL )
	{
		libfsntfs_data_stream_free( &data_stream, NULL );
	}
	return return_code;
}
