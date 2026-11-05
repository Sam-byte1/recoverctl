#include <string.h>

#include "fat_boot_sector.h"

static uint16_t read_u16_le( const uint8_t *buffer, size_t offset )
{
	return (uint16_t) ( buffer[ offset ] | ( buffer[ offset + 1 ] << 8 ) );
}

static uint32_t read_u32_le( const uint8_t *buffer, size_t offset )
{
	return (uint32_t) buffer[ offset ]
	     | ( (uint32_t) buffer[ offset + 1 ] << 8 )
	     | ( (uint32_t) buffer[ offset + 2 ] << 16 )
	     | ( (uint32_t) buffer[ offset + 3 ] << 24 );
}

int fat_boot_sector_parse(
     const uint8_t *sector_data,
     size_t sector_data_size,
     fat_boot_sector_t *out )
{
	uint16_t root_entry_count = 0;
	uint16_t fat_size_16      = 0;
	uint16_t boot_signature   = 0;

	if( sector_data == NULL || out == NULL || sector_data_size < 512 )
	{
		return -1;
	}
	memset( out, 0, sizeof( fat_boot_sector_t ) );

	out->bytes_per_sector      = read_u16_le( sector_data, 11 );
	out->sectors_per_cluster   = sector_data[ 13 ];
	out->reserved_sector_count = read_u16_le( sector_data, 14 );
	out->number_of_fats        = sector_data[ 16 ];
	out->total_sectors         = read_u32_le( sector_data, 32 );
	out->fat_size_sectors      = read_u32_le( sector_data, 36 );
	out->root_cluster          = read_u32_le( sector_data, 44 );

	root_entry_count = read_u16_le( sector_data, 17 );
	fat_size_16      = read_u16_le( sector_data, 22 );
	boot_signature   = read_u16_le( sector_data, 510 );

	/* 判定"像不像一个合法的 FAT32 引导扇区"，不是严格按 Microsoft 规范
	 * 用簇总数反推 FAT12/16/32（那需要先算出数据区大小，循环依赖），
	 * 而是看几个 FAT32 特有的、互相印证的字段组合：
	 * - 0x55AA 结束签名必须有
	 * - root_entry_count 必须是 0（FAT32 没有固定大小的根目录区，这是和 FAT12/16 的关键区别）
	 * - fat_size_16 必须是 0、fat_size_sectors（32位字段）必须非 0（FAT32 专用 32 位字段）
	 * - bytes_per_sector/sectors_per_cluster/number_of_fats 不能是明显不合理的值
	 */
	out->is_valid = ( boot_signature == 0xAA55 )
	             && ( root_entry_count == 0 )
	             && ( fat_size_16 == 0 )
	             && ( out->fat_size_sectors != 0 )
	             && ( out->bytes_per_sector != 0 )
	             && ( out->sectors_per_cluster != 0 )
	             && ( out->number_of_fats != 0 )
	             && ( out->root_cluster >= 2 );

	return 1;
}

uint32_t fat_boot_sector_fat_start_sector(
          const fat_boot_sector_t *bs )
{
	if( bs == NULL )
	{
		return 0;
	}
	return bs->reserved_sector_count;
}

uint32_t fat_boot_sector_data_start_sector(
          const fat_boot_sector_t *bs )
{
	if( bs == NULL )
	{
		return 0;
	}
	return bs->reserved_sector_count
	     + ( (uint32_t) bs->number_of_fats * bs->fat_size_sectors );
}

uint32_t fat_boot_sector_cluster_to_sector(
          const fat_boot_sector_t *bs,
          uint32_t cluster )
{
	if( bs == NULL || cluster < 2 )
	{
		return 0;
	}
	return fat_boot_sector_data_start_sector( bs )
	     + ( cluster - 2 ) * (uint32_t) bs->sectors_per_cluster;
}
