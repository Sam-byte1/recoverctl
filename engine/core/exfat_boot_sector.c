#include <string.h>

#include "exfat_boot_sector.h"

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

static uint64_t read_u64_le( const uint8_t *buffer, size_t offset )
{
	return (uint64_t) read_u32_le( buffer, offset )
	     | ( (uint64_t) read_u32_le( buffer, offset + 4 ) << 32 );
}

int exfat_boot_sector_parse(
     const uint8_t *sector_data,
     size_t sector_data_size,
     exfat_boot_sector_t *out )
{
	uint16_t boot_signature;
	uint8_t bytes_per_sector_shift;
	uint8_t sectors_per_cluster_shift;

	if( sector_data == NULL || out == NULL || sector_data_size < 512 )
	{
		return -1;
	}
	memset( out, 0, sizeof( exfat_boot_sector_t ) );

	out->volume_length_sectors          = read_u64_le( sector_data, 0x48 );
	out->fat_offset_sectors             = read_u32_le( sector_data, 0x50 );
	out->fat_length_sectors             = read_u32_le( sector_data, 0x54 );
	out->cluster_heap_offset_sectors    = read_u32_le( sector_data, 0x58 );
	out->cluster_count                  = read_u32_le( sector_data, 0x5C );
	out->first_cluster_of_root_directory = read_u32_le( sector_data, 0x60 );

	bytes_per_sector_shift    = sector_data[ 0x6C ];
	sectors_per_cluster_shift = sector_data[ 0x6D ];
	out->number_of_fats       = sector_data[ 0x6E ];

	/* shift 值超过 31 会导致移位未定义行为（规范里合法范围是 9~12 和 0~(25-shift)，
	 * 这里只做"不会读越界/不会 UB"这个最基本的保护，不是完整的规范校验 */
	out->bytes_per_sector    = ( bytes_per_sector_shift    < 32 ) ? ( 1u << bytes_per_sector_shift )    : 0;
	out->sectors_per_cluster = ( sectors_per_cluster_shift < 32 ) ? ( 1u << sectors_per_cluster_shift ) : 0;

	boot_signature = read_u16_le( sector_data, 0x1FE );

	/* 判定"像不像一个合法的 exFAT 主引导扇区"：看 JumpBoot 固定前缀、
	 * FileSystemName 固定字符串、0x55AA 结束签名、以及几个关键字段不能是
	 * 明显不合理的 0 值——这几项互相印证，和 FAT32 那边的 is_valid 判定
	 * 思路一致，不是严格按规范做完整校验（校验和、保留字段这些不检查）。 */
	out->is_valid = ( sector_data[ 0 ] == 0xEB )
	             && ( sector_data[ 2 ] == 0x90 )
	             && ( memcmp( sector_data + 3, "EXFAT   ", 8 ) == 0 )
	             && ( boot_signature == 0xAA55 )
	             && ( out->bytes_per_sector != 0 )
	             && ( out->sectors_per_cluster != 0 )
	             && ( out->number_of_fats != 0 )
	             && ( out->cluster_heap_offset_sectors != 0 )
	             && ( out->first_cluster_of_root_directory >= 2 );

	return 1;
}

uint32_t exfat_boot_sector_cluster_to_sector(
          const exfat_boot_sector_t *bs,
          uint32_t cluster )
{
	if( bs == NULL || cluster < 2 )
	{
		return 0;
	}
	return bs->cluster_heap_offset_sectors + ( cluster - 2 ) * bs->sectors_per_cluster;
}
