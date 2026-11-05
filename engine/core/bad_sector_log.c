#include <stdlib.h>

#include "bad_sector_log.h"

int bad_sector_log_append(
     bad_sector_log_t *log,
     uint64_t offset,
     uint64_t length )
{
	if( log == NULL )
	{
		return -1;
	}

	if( log->count == log->capacity )
	{
		int new_capacity = log->capacity == 0 ? 8 : log->capacity * 2;
		bad_sector_record_t *new_array = (bad_sector_record_t *) realloc(
		                                    log->records,
		                                    (size_t) new_capacity * sizeof( bad_sector_record_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		log->records  = new_array;
		log->capacity = new_capacity;
	}

	log->records[ log->count ].offset = offset;
	log->records[ log->count ].length = length;
	log->count++;

	return 1;
}

uint64_t bad_sector_log_total_bytes_skipped(
          const bad_sector_log_t *log )
{
	uint64_t total;
	int i;

	if( log == NULL )
	{
		return 0;
	}

	total = 0;
	for( i = 0; i < log->count; i++ )
	{
		total += log->records[ i ].length;
	}
	return total;
}

void bad_sector_log_free(
      bad_sector_log_t *log )
{
	if( log == NULL )
	{
		return;
	}
	free( log->records );
	log->records  = NULL;
	log->count    = 0;
	log->capacity = 0;
}
