#include "fat_fat_chain.h"

#define FAT32_FAT_ENTRY_SIZE 4
#define FAT32_FAT_ENTRY_MASK 0x0FFFFFFFu
#define FAT32_FAT_EOF_MIN 0x0FFFFFF8u
#define FAT32_FAT_BAD_CLUSTER 0x0FFFFFF7u
#define FAT32_FAT_FREE_MARKER 0x00000000u

static uint32_t read_u32le(
     const uint8_t *data,
     size_t offset )
{
	return ( (uint32_t) data[ offset ]
	     | ( (uint32_t) data[ offset + 1 ] << 8 )
	     | ( (uint32_t) data[ offset + 2 ] << 16 )
	     | ( (uint32_t) data[ offset + 3 ] << 24 ) ) & FAT32_FAT_ENTRY_MASK;
}

int fat_fat_chain_walk(
     const uint8_t *fat_table_data,
     size_t fat_table_data_size,
     uint32_t start_cluster,
     uint32_t total_data_clusters,
     uint32_t max_clusters,
     uint32_t *out_clusters,
     uint32_t *out_cluster_count,
     fat_fat_chain_walk_status_t *out_status )
{
	uint32_t current_cluster;
	uint32_t collected;
	uint32_t min_valid_cluster;
	uint32_t max_valid_cluster;

	if( fat_table_data == NULL || out_clusters == NULL
	 || out_cluster_count == NULL || out_status == NULL
	 || max_clusters == 0 || start_cluster < 2 )
	{
		return -1;
	}

	min_valid_cluster = 2;
	max_valid_cluster = total_data_clusters + 1;

	current_cluster = start_cluster;
	collected       = 0;

	while( collected < max_clusters )
	{
		size_t entry_offset;
		uint32_t entry_value;

		if( current_cluster < min_valid_cluster || current_cluster > max_valid_cluster )
		{
			*out_cluster_count = collected;
			*out_status         = FAT_FAT_CHAIN_WALK_OUT_OF_RANGE;
			return 1;
		}

		out_clusters[ collected ] = current_cluster;
		collected++;

		entry_offset = (size_t) current_cluster * FAT32_FAT_ENTRY_SIZE;
		if( entry_offset + FAT32_FAT_ENTRY_SIZE > fat_table_data_size )
		{
			*out_cluster_count = collected;
			*out_status         = FAT_FAT_CHAIN_WALK_OUT_OF_RANGE;
			return 1;
		}
		entry_value = read_u32le( fat_table_data, entry_offset );

		if( entry_value >= FAT32_FAT_EOF_MIN )
		{
			*out_cluster_count = collected;
			*out_status         = FAT_FAT_CHAIN_WALK_OK;
			return 1;
		}
		if( entry_value == FAT32_FAT_BAD_CLUSTER )
		{
			*out_cluster_count = collected;
			*out_status         = FAT_FAT_CHAIN_WALK_BAD_CLUSTER;
			return 1;
		}
		if( entry_value == FAT32_FAT_FREE_MARKER )
		{
			*out_cluster_count = collected;
			*out_status         = FAT_FAT_CHAIN_WALK_HIT_FREE;
			return 1;
		}

		current_cluster = entry_value;
	}

	*out_cluster_count = collected;
	*out_status         = FAT_FAT_CHAIN_WALK_TOO_LONG;
	return 1;
}
