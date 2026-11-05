#include <stdio.h>
#include <string.h>

#include "fat_dir_entry.h"

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LONG_NAME ( FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID )

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

/* 把 11 字节的 8.3 短文件名（8 字节主名 + 3 字节扩展名，空格补齐）
 * 转成形如 "HELLO.TXT" 的可读字符串。first_char_override 非 -1 时，
 * 用它替换掉原始名字第一个字节（用于已删除条目：那个位置已经被 0xE5
 * 覆盖，不是真实字符，调用方传入占位符 '_'）。
 */
static void format_short_name(
     const uint8_t *raw_name_11_bytes,
     int first_char_override,
     char *out_buffer,
     size_t out_buffer_size )
{
	char name_part[ 9 ];
	char ext_part[ 4 ];
	int i;
	int name_length;
	int ext_length;

	for( i = 0; i < 8; i++ )
	{
		name_part[ i ] = (char) raw_name_11_bytes[ i ];
	}
	name_part[ 8 ] = '\0';

	if( first_char_override != -1 )
	{
		name_part[ 0 ] = (char) first_char_override;
	}

	for( i = 0; i < 3; i++ )
	{
		ext_part[ i ] = (char) raw_name_11_bytes[ 8 + i ];
	}
	ext_part[ 3 ] = '\0';

	/* 去掉末尾空格 */
	name_length = 8;
	while( name_length > 0 && name_part[ name_length - 1 ] == ' ' )
	{
		name_length--;
	}
	name_part[ name_length ] = '\0';

	ext_length = 3;
	while( ext_length > 0 && ext_part[ ext_length - 1 ] == ' ' )
	{
		ext_length--;
	}
	ext_part[ ext_length ] = '\0';

	if( ext_length > 0 )
	{
		snprintf( out_buffer, out_buffer_size, "%s.%s", name_part, ext_part );
	}
	else
	{
		snprintf( out_buffer, out_buffer_size, "%s", name_part );
	}
}

int fat_dir_entry_parse(
     const uint8_t *entry_data,
     size_t entry_data_size,
     fat_dir_entry_t *out )
{
	uint8_t first_byte;
	uint8_t attribute;

	if( entry_data == NULL || out == NULL || entry_data_size < 32 )
	{
		return -1;
	}
	memset( out, 0, sizeof( fat_dir_entry_t ) );

	first_byte = entry_data[ 0 ];
	attribute  = entry_data[ 11 ];

	if( first_byte == 0x00 )
	{
		out->kind = FAT_DIR_ENTRY_KIND_END_OF_DIRECTORY;
		return 1;
	}

	if( ( attribute & FAT_ATTR_LONG_NAME ) == FAT_ATTR_LONG_NAME )
	{
		/* VFAT 长文件名条目的 13 个 UTF-16LE 字符分三段存放：
		 * 偏移 1-10（5 个字符）、偏移 14-25（6 个字符）、偏移 28-31（2 个字符）。
		 * 偏移 0（序号）、11（固定是 0x0F）、12（保留）、13（短名校验和）、
		 * 26-27（固定是 0）中间穿插在这些字符段之间，是 VFAT 规范本身
		 * 定下来的奇怪布局，不是我们选的。 */
		out->kind = FAT_DIR_ENTRY_KIND_LONG_NAME_PART;
		memcpy( out->long_name_part_utf16le,      entry_data + 1,  10 );
		memcpy( out->long_name_part_utf16le + 10, entry_data + 14, 12 );
		memcpy( out->long_name_part_utf16le + 22, entry_data + 28,  4 );
		return 1;
	}

	if( ( attribute & FAT_ATTR_VOLUME_ID ) != 0 )
	{
		out->kind = FAT_DIR_ENTRY_KIND_VOLUME_LABEL;
		return 1;
	}

	out->first_cluster = ( (uint32_t) read_u16_le( entry_data, 20 ) << 16 )
	                    | read_u16_le( entry_data, 26 );
	out->file_size   = read_u32_le( entry_data, 28 );
	out->is_directory = ( attribute & FAT_ATTR_DIRECTORY ) != 0;

	if( first_byte == 0xE5 )
	{
		out->kind = FAT_DIR_ENTRY_KIND_DELETED;
		format_short_name( entry_data, '_', out->short_name, sizeof( out->short_name ) );
	}
	else
	{
		out->kind = FAT_DIR_ENTRY_KIND_NORMAL;
		format_short_name( entry_data, -1, out->short_name, sizeof( out->short_name ) );
	}

	return 1;
}
