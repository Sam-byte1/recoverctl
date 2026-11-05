#include <stdlib.h>
#include <string.h>

#include <libvsgpt.h>
#include <libvsmbr.h>

#include "partition_reader.h"

static int append_partition(
     gpt_partition_scan_result_t *result,
     const gpt_partition_info_t *entry )
{
	if( result->count == result->capacity )
	{
		int new_capacity = result->capacity == 0 ? 8 : result->capacity * 2;
		gpt_partition_info_t *new_array = (gpt_partition_info_t *) realloc(
		                                    result->partitions,
		                                    (size_t) new_capacity * sizeof( gpt_partition_info_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		result->partitions = new_array;
		result->capacity   = new_capacity;
	}
	result->partitions[ result->count ] = *entry;
	result->count++;
	return 1;
}

int partition_reader_scan_gpt(
     const char *device_path,
     gpt_partition_scan_result_t *out )
{
	libvsgpt_volume_t *volume = NULL;
	libvsgpt_error_t *error   = NULL;
	int number_of_partitions  = 0;
	int i;
	int return_code = -1;

	if( device_path == NULL || out == NULL )
	{
		return -1;
	}
	out->partitions = NULL;
	out->count      = 0;
	out->capacity   = 0;

	if( libvsgpt_volume_initialize( &volume, &error ) != 1 )
	{
		goto on_error;
	}
	if( libvsgpt_volume_open( volume, device_path, LIBVSGPT_OPEN_READ, &error ) != 1 )
	{
		goto on_error;
	}
	if( libvsgpt_volume_get_number_of_partitions( volume, &number_of_partitions, &error ) != 1 )
	{
		goto on_error;
	}

	for( i = 0; i < number_of_partitions; i++ )
	{
		libvsgpt_partition_t *partition = NULL;
		gpt_partition_info_t entry;
		off64_t offset = 0;
		size64_t size  = 0;

		memset( &entry, 0, sizeof( entry ) );

		if( libvsgpt_volume_get_partition_by_index( volume, i, &partition, &error ) != 1 )
		{
			goto on_error;
		}
		libvsgpt_partition_get_volume_offset( partition, &offset, &error );
		libvsgpt_partition_get_size( partition, &size, &error );
		libvsgpt_partition_get_type_identifier( partition, entry.type_guid, sizeof( entry.type_guid ), &error );
		libvsgpt_partition_free( &partition, &error );

		entry.index  = i;
		entry.offset = (uint64_t) offset;
		entry.size   = (uint64_t) size;

		if( append_partition( out, &entry ) != 1 )
		{
			goto on_error;
		}
	}

	return_code = 1;
	goto cleanup;

on_error:
	if( error != NULL )
	{
		libvsgpt_error_free( &error );
	}
	gpt_partition_scan_result_free( out );
	return_code = -1;

cleanup:
	if( volume != NULL )
	{
		libvsgpt_volume_free( &volume, NULL );
	}
	return return_code;
}

void gpt_partition_scan_result_free(
      gpt_partition_scan_result_t *result )
{
	if( result == NULL )
	{
		return;
	}
	free( result->partitions );
	result->partitions = NULL;
	result->count      = 0;
	result->capacity   = 0;
}

static int append_mbr_partition(
     mbr_partition_scan_result_t *result,
     const mbr_partition_info_t *entry )
{
	if( result->count == result->capacity )
	{
		int new_capacity = result->capacity == 0 ? 8 : result->capacity * 2;
		mbr_partition_info_t *new_array = (mbr_partition_info_t *) realloc(
		                                    result->partitions,
		                                    (size_t) new_capacity * sizeof( mbr_partition_info_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		result->partitions = new_array;
		result->capacity   = new_capacity;
	}
	result->partitions[ result->count ] = *entry;
	result->count++;
	return 1;
}

int partition_reader_scan_mbr(
     const char *device_path,
     mbr_partition_scan_result_t *out )
{
	libvsmbr_volume_t *volume = NULL;
	libvsmbr_error_t *error   = NULL;
	int number_of_partitions  = 0;
	int i;
	int return_code = -1;

	if( device_path == NULL || out == NULL )
	{
		return -1;
	}
	out->partitions = NULL;
	out->count      = 0;
	out->capacity   = 0;

	if( libvsmbr_volume_initialize( &volume, &error ) != 1 )
	{
		goto on_error;
	}
	if( libvsmbr_volume_open( volume, device_path, LIBVSMBR_OPEN_READ, &error ) != 1 )
	{
		goto on_error;
	}
	if( libvsmbr_volume_get_number_of_partitions( volume, &number_of_partitions, &error ) != 1 )
	{
		goto on_error;
	}

	for( i = 0; i < number_of_partitions; i++ )
	{
		libvsmbr_partition_t *partition = NULL;
		mbr_partition_info_t entry;
		off64_t offset = 0;
		size64_t size  = 0;
		uint8_t type   = 0;

		memset( &entry, 0, sizeof( entry ) );

		if( libvsmbr_volume_get_partition_by_index( volume, i, &partition, &error ) != 1 )
		{
			goto on_error;
		}
		libvsmbr_partition_get_volume_offset( partition, &offset, &error );
		libvsmbr_partition_get_size( partition, &size, &error );
		libvsmbr_partition_get_type( partition, &type, &error );
		libvsmbr_partition_free( &partition, &error );

		entry.index  = i;
		entry.offset = (uint64_t) offset;
		entry.size   = (uint64_t) size;
		entry.type   = type;

		if( append_mbr_partition( out, &entry ) != 1 )
		{
			goto on_error;
		}
	}

	return_code = 1;
	goto cleanup;

on_error:
	if( error != NULL )
	{
		libvsmbr_error_free( &error );
	}
	mbr_partition_scan_result_free( out );
	return_code = -1;

cleanup:
	if( volume != NULL )
	{
		libvsmbr_volume_free( &volume, NULL );
	}
	return return_code;
}

void mbr_partition_scan_result_free(
      mbr_partition_scan_result_t *result )
{
	if( result == NULL )
	{
		return;
	}
	free( result->partitions );
	result->partitions = NULL;
	result->count      = 0;
	result->capacity   = 0;
}
