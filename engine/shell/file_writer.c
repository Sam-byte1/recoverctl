#include <stdio.h>

#include "file_writer.h"

#define FILE_WRITER_BUFFER_SIZE 4096

int file_writer_write_entry_content(
     libfsntfs_file_entry_t *file_entry,
     const char *output_path,
     libfsntfs_error_t **error )
{
	uint8_t buffer[ FILE_WRITER_BUFFER_SIZE ];
	ssize_t read_count = 0;
	FILE *output = NULL;

	if( file_entry == NULL || output_path == NULL )
	{
		return -1;
	}

	output = fopen( output_path, "wb" );

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
		if( fwrite( buffer, 1, (size_t) read_count, output ) != (size_t) read_count )
		{
			fclose( output );
			return -1;
		}
	}

	fclose( output );
	return 1;
}
