#include <stdlib.h>
#include <string.h>

#include "undelete_scanner.h"

static int undelete_scan_result_append(
     undelete_scan_result_t *result,
     const undelete_candidate_t *candidate )
{
	if( result->count == result->capacity )
	{
		int new_capacity = result->capacity == 0 ? 8 : result->capacity * 2;
		undelete_candidate_t *new_array = (undelete_candidate_t *) realloc(
		                                    result->candidates,
		                                    (size_t) new_capacity * sizeof( undelete_candidate_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		result->candidates = new_array;
		result->capacity   = new_capacity;
	}

	result->candidates[ result->count ] = *candidate;
	result->count++;

	return 1;
}

int undelete_scanner_scan_volume(
     libfsntfs_volume_t *volume,
     undelete_scan_result_t *out,
     libfsntfs_error_t **error )
{
	libfsntfs_file_entry_t *file_entry = NULL;
	uint64_t number_of_entries = 0;
	uint64_t index             = 0;
	undelete_candidate_t candidate;

	if( out == NULL || volume == NULL )
	{
		return -1;
	}

	out->candidates = NULL;
	out->count      = 0;
	out->capacity   = 0;

	if( libfsntfs_volume_get_number_of_file_entries( volume, &number_of_entries, error ) != 1 )
	{
		return -1;
	}

	for( index = 0; index < number_of_entries; index++ )
	{
		if( libfsntfs_volume_get_file_entry_by_index( volume, index, &file_entry, error ) != 1 )
		{
			/* 有些索引本身不可读（从未使用的槽位），这是正常情况，跳过继续 */
			if( error != NULL && *error != NULL )
			{
				libfsntfs_error_free( error );
			}
			continue;
		}

		if( undelete_detector_inspect( file_entry, index, &candidate, error ) != 1 )
		{
			libfsntfs_file_entry_free( &file_entry, NULL );
			undelete_scan_result_free( out );
			return -1;
		}

		if( candidate.is_deleted_recoverable )
		{
			if( undelete_scan_result_append( out, &candidate ) != 1 )
			{
				libfsntfs_file_entry_free( &file_entry, NULL );
				undelete_scan_result_free( out );
				return -1;
			}
		}

		libfsntfs_file_entry_free( &file_entry, error );
	}

	return 1;
}

void undelete_scan_result_free(
      undelete_scan_result_t *result )
{
	if( result == NULL )
	{
		return;
	}
	free( result->candidates );
	result->candidates = NULL;
	result->count      = 0;
	result->capacity   = 0;
}
