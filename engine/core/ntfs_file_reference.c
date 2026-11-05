#include "ntfs_file_reference.h"

uint64_t ntfs_file_reference_to_mft_index( uint64_t file_reference )
{
	return file_reference & 0x0000FFFFFFFFFFFFULL;
}
