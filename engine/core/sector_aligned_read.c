#include "sector_aligned_read.h"

size_t sector_aligned_read_compute_request_size(
        size_t requested,
        size_t sector_size,
        size_t buffer_capacity )
{
	size_t aligned_requested = requested;

	if( ( sector_size > 0 ) && ( requested % sector_size != 0 ) )
	{
		aligned_requested = requested + ( sector_size - ( requested % sector_size ) );
	}
	if( aligned_requested > buffer_capacity )
	{
		aligned_requested = buffer_capacity;
	}
	return aligned_requested;
}

size_t sector_aligned_read_usable_bytes(
        size_t requested,
        size_t actually_read )
{
	return actually_read < requested ? actually_read : requested;
}
