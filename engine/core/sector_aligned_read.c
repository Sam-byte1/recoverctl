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

void sector_aligned_read_compute_aligned_seek(
      uint64_t physical_target,
      size_t sector_size,
      uint64_t *out_aligned_physical_target,
      size_t *out_lead_in )
{
	uint64_t remainder;

	if( sector_size == 0 )
	{
		*out_aligned_physical_target = physical_target;
		*out_lead_in = 0;
		return;
	}
	remainder = physical_target % (uint64_t) sector_size;
	*out_aligned_physical_target = physical_target - remainder;
	*out_lead_in = (size_t) remainder;
}

size_t sector_aligned_read_usable_bytes_with_lead_in(
        size_t lead_in,
        size_t requested,
        size_t actually_read )
{
	size_t available_after_lead_in;

	if( actually_read <= lead_in )
	{
		return 0;
	}
	available_after_lead_in = actually_read - lead_in;
	return available_after_lead_in < requested ? available_after_lead_in : requested;
}
