#include <string.h>

#include "exfat_dir_entry.h"
#include "utf16_to_utf8.h"

#define EXFAT_ENTRY_TYPE_MASK_NO_INUSE 0x7F

#define EXFAT_ENTRY_TYPE_FILE       0x05  /* 0x85 清掉 InUse 位之后 */
#define EXFAT_ENTRY_TYPE_STREAM_EXT 0x40  /* 0xC0 清掉 InUse 位之后 */
#define EXFAT_ENTRY_TYPE_FILE_NAME  0x41  /* 0xC1 清掉 InUse 位之后 */

#define EXFAT_ATTR_DIRECTORY 0x0010
#define EXFAT_SECONDARY_FLAG_NO_FAT_CHAIN 0x02

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

static void decode_name(
     const uint8_t *name_entry_data,
     uint8_t name_length,
     char *out_name,
     size_t out_name_size )
{
	/* name_entry_data 指向这条 File Name 目录项的开头（32 字节），
	 * 前 2 字节是 EntryType+保留字段，真正的 UTF-16LE 文件名从偏移 2
	 * 开始。2026-11-05 小树裁决第13步修复：原来这里是"每个 UTF-16 码元
	 * 取低字节"的简化处理，对非 ASCII 字符（中文等）会产生垃圾字节，
	 * 是真实 bug；现在用正确的 UTF-16LE→UTF-8 转换。 */
	utf16le_to_utf8( name_entry_data + 2, name_length, out_name, out_name_size );
}

int exfat_dir_entry_set_parse(
     const uint8_t *entries_data,
     size_t entries_data_size,
     size_t entry_index,
     exfat_dir_entry_set_t *out )
{
	size_t entry_offset;
	uint8_t entry_type;
	uint8_t type_no_inuse;
	int in_use;

	if( entries_data == NULL || out == NULL )
	{
		return -1;
	}
	entry_offset = entry_index * 32;
	if( entry_offset + 32 > entries_data_size )
	{
		return -1;
	}
	memset( out, 0, sizeof( exfat_dir_entry_set_t ) );

	entry_type = entries_data[ entry_offset ];

	if( entry_type == 0x00 )
	{
		out->kind = EXFAT_DIR_ENTRY_SET_KIND_END_OF_DIRECTORY;
		out->consumed_entries = 1;
		return 1;
	}

	in_use        = ( entry_type & 0x80 ) != 0;
	type_no_inuse = entry_type & EXFAT_ENTRY_TYPE_MASK_NO_INUSE;

	if( type_no_inuse != EXFAT_ENTRY_TYPE_FILE )
	{
		/* 位图/大写表/卷标/其它不认识的条目类型：本轮不展开解析，跳过 1 条继续扫描 */
		out->kind = EXFAT_DIR_ENTRY_SET_KIND_SKIPPED;
		out->consumed_entries = 1;
		return 1;
	}

	{
		uint8_t secondary_count = entries_data[ entry_offset + 1 ];
		size_t total_entries_needed = 1 + (size_t) secondary_count;
		size_t stream_offset;
		size_t name_offset;
		uint8_t stream_type_no_inuse;
		uint8_t name_type_no_inuse;
		uint8_t name_length;
		uint16_t file_attributes;

		if( secondary_count < 2
		 || ( entry_index + total_entries_needed ) * 32 > entries_data_size )
		{
			/* SecondaryCount 声称的条目数要么太短装不下 Stream+Name，要么会
			 * 读出缓冲区范围——结构已经不可信，按"跳过这一条"处理，不尝试
			 * 展开，避免误读后面本不属于这个文件的数据 */
			out->kind = EXFAT_DIR_ENTRY_SET_KIND_SKIPPED;
			out->consumed_entries = 1;
			return 1;
		}

		stream_offset = ( entry_index + 1 ) * 32;
		stream_type_no_inuse = entries_data[ stream_offset ] & EXFAT_ENTRY_TYPE_MASK_NO_INUSE;

		name_offset = ( entry_index + 2 ) * 32;
		name_type_no_inuse = entries_data[ name_offset ] & EXFAT_ENTRY_TYPE_MASK_NO_INUSE;

		if( stream_type_no_inuse != EXFAT_ENTRY_TYPE_STREAM_EXT
		 || name_type_no_inuse != EXFAT_ENTRY_TYPE_FILE_NAME )
		{
			/* 紧跟着的条目不是预期的 Stream Extension + File Name 组合，
			 * 按整个 entry set 的大小跳过，不当作正常/已删除文件处理 */
			out->kind = EXFAT_DIR_ENTRY_SET_KIND_SKIPPED;
			out->consumed_entries = total_entries_needed;
			return 1;
		}

		name_length = entries_data[ stream_offset + 3 ];

		if( name_length > 15 )
		{
			/* 超过 1 条 File Name 目录项能装下的长度，本轮范围不支持多条
			 * File Name 目录项拼接，跳过但正确推进游标 */
			out->kind = EXFAT_DIR_ENTRY_SET_KIND_SKIPPED;
			out->consumed_entries = total_entries_needed;
			return 1;
		}

		out->first_cluster  = read_u32_le( entries_data, stream_offset + 20 );
		out->file_size      = read_u64_le( entries_data, stream_offset + 24 );
		out->no_fat_chain   = ( entries_data[ stream_offset + 1 ] & EXFAT_SECONDARY_FLAG_NO_FAT_CHAIN ) != 0;

		file_attributes   = (uint16_t) ( entries_data[ entry_offset + 4 ] | ( entries_data[ entry_offset + 5 ] << 8 ) );
		out->is_directory = ( file_attributes & EXFAT_ATTR_DIRECTORY ) != 0;

		decode_name( entries_data + name_offset, name_length, out->name, sizeof( out->name ) );

		out->kind = in_use ? EXFAT_DIR_ENTRY_SET_KIND_NORMAL_FILE : EXFAT_DIR_ENTRY_SET_KIND_DELETED_FILE;
		out->consumed_entries = total_entries_needed;
		return 1;
	}
}
