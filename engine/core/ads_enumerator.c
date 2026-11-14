#include <stdlib.h>
#include <string.h>

#include "ads_enumerator.h"

static int ads_stream_list_append(
     ads_stream_list_t *list,
     const ads_stream_info_t *info )
{
	if( list->count == list->capacity )
	{
		int new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
		ads_stream_info_t *new_array = (ads_stream_info_t *) realloc(
		                                  list->streams,
		                                  (size_t) new_capacity * sizeof( ads_stream_info_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		list->streams  = new_array;
		list->capacity = new_capacity;
	}
	list->streams[ list->count ] = *info;
	list->count++;

	return 1;
}

int ads_enumerator_list_streams(
     libfsntfs_file_entry_t *file_entry,
     ads_stream_list_t *out,
     libfsntfs_error_t **error )
{
	libfsntfs_data_stream_t *data_stream = NULL;
	int number_of_streams = 0;
	int stream_index      = 0;

	if( out == NULL )
	{
		return -1;
	}
	out->streams  = NULL;
	out->count    = 0;
	out->capacity = 0;

	if( file_entry == NULL )
	{
		return -1;
	}

	if( libfsntfs_file_entry_get_number_of_alternate_data_streams(
	     file_entry, &number_of_streams, error ) != 1 )
	{
		return -1;
	}

	for( stream_index = 0; stream_index < number_of_streams; stream_index++ )
	{
		ads_stream_info_t info;
		size_t name_size      = 0;
		size64_t stream_size  = 0;

		memset( &info, 0, sizeof( ads_stream_info_t ) );

		if( libfsntfs_file_entry_get_alternate_data_stream_by_index(
		     file_entry, stream_index, &data_stream, error ) != 1 )
		{
			/* 单条流读取失败就跳过，不升级成整体失败——和
			 * undelete_detector_inspect 对单条属性读取失败的处理一致，
			 * 真实磁盘上某一条流读不出来不代表其它流也读不出来。 */
			if( ( error != NULL ) && ( *error != NULL ) )
			{
				libfsntfs_error_free( error );
			}
			continue;
		}

		if( libfsntfs_data_stream_get_utf8_name_size(
		     data_stream, &name_size, error ) == 1
		 && name_size > 0
		 && name_size <= sizeof( info.name ) )
		{
			libfsntfs_data_stream_get_utf8_name(
			 data_stream,
			 (uint8_t *) info.name,
			 name_size,
			 error );
		}
		else if( ( error != NULL ) && ( *error != NULL ) )
		{
			libfsntfs_error_free( error );
		}

		if( libfsntfs_data_stream_get_size( data_stream, &stream_size, error ) == 1 )
		{
			info.size     = (uint64_t) stream_size;
			info.has_size = 1;
		}
		else if( ( error != NULL ) && ( *error != NULL ) )
		{
			libfsntfs_error_free( error );
		}

		libfsntfs_data_stream_free( &data_stream, error );

		if( ads_stream_list_append( out, &info ) != 1 )
		{
			ads_stream_list_free( out );
			return -1;
		}
	}

	return 1;
}

void ads_stream_list_free(
      ads_stream_list_t *list )
{
	if( list == NULL )
	{
		return;
	}
	free( list->streams );
	list->streams  = NULL;
	list->count    = 0;
	list->capacity = 0;
}
