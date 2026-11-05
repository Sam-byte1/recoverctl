#include "photorec_report_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 在 [data, data+data_len) 范围内找 needle 第一次出现的位置，找不到
 * 返回 NULL。手写而不是用 memmem，因为 memmem 不是所有目标平台（尤其
 * Windows/MSVC 系的 C 运行库）都保证有，项目里其它核心模块一直是手写
 * 字节级解析，这里保持同样的风格，不额外引入可移植性顾虑。 */
static const char *bounded_find(
     const char *data,
     size_t data_len,
     const char *needle )
{
	size_t needle_len = strlen( needle );
	size_t i;

	if( ( needle_len == 0 )
	 || ( data_len < needle_len ) )
	{
		return NULL;
	}
	for( i = 0; i + needle_len <= data_len; i++ )
	{
		if( memcmp( data + i, needle, needle_len ) == 0 )
		{
			return data + i;
		}
	}
	return NULL;
}

/* 抽取 <tag_name>内容</tag_name> 之间的文本，拷贝进 out_buf（超出
 * out_buf_size 会被截断，保证总是 NUL 结尾）。找不到完整的开始/结束
 * 标签返回 -1。 */
static int extract_tag_content(
     const char *block,
     size_t block_len,
     const char *tag_name,
     char *out_buf,
     size_t out_buf_size )
{
	char open_tag[ 64 ];
	char close_tag[ 64 ];
	const char *content_start;
	const char *content_end;
	size_t content_len;
	size_t avail;

	snprintf( open_tag, sizeof( open_tag ), "<%s>", tag_name );
	snprintf( close_tag, sizeof( close_tag ), "</%s>", tag_name );

	content_start = bounded_find( block, block_len, open_tag );
	if( content_start == NULL )
	{
		return -1;
	}
	content_start += strlen( open_tag );

	avail = block_len - (size_t) ( content_start - block );
	content_end = bounded_find( content_start, avail, close_tag );
	if( content_end == NULL )
	{
		return -1;
	}
	content_len = (size_t) ( content_end - content_start );
	if( content_len >= out_buf_size )
	{
		content_len = out_buf_size - 1;
	}
	memcpy( out_buf, content_start, content_len );
	out_buf[ content_len ] = '\0';
	return 0;
}

/* 抽取形如 attr_name='123' 或 attr_name="123" 的属性值（PhotoRec 实测
 * 输出用的是单引号，这里两种引号都认，防御性地兼容）。 */
static int extract_uint64_attribute(
     const char *block,
     size_t block_len,
     const char *attr_name,
     uint64_t *out_value )
{
	char pattern[ 64 ];
	const char *pos;
	const char *block_end = block + block_len;
	char quote;
	char digits[ 32 ];
	size_t digit_count = 0;

	snprintf( pattern, sizeof( pattern ), "%s=", attr_name );

	pos = bounded_find( block, block_len, pattern );
	if( pos == NULL )
	{
		return -1;
	}
	pos += strlen( pattern );

	if( ( pos >= block_end )
	 || ( ( *pos != '\'' ) && ( *pos != '"' ) ) )
	{
		return -1;
	}
	quote = *pos;
	pos++;

	while( ( pos < block_end )
	    && ( *pos != quote )
	    && ( digit_count < sizeof( digits ) - 1 ) )
	{
		digits[ digit_count ] = *pos;
		digit_count++;
		pos++;
	}
	digits[ digit_count ] = '\0';

	*out_value = strtoull( digits, NULL, 10 );
	return 0;
}

static int photorec_report_append(
     photorec_report_t *report,
     const photorec_report_entry_t *entry )
{
	if( report->count == report->capacity )
	{
		int new_capacity = report->capacity == 0 ? 8 : report->capacity * 2;
		photorec_report_entry_t *new_array = (photorec_report_entry_t *) realloc(
		                                        report->entries,
		                                        (size_t) new_capacity * sizeof( photorec_report_entry_t ) );

		if( new_array == NULL )
		{
			return -1;
		}
		report->entries  = new_array;
		report->capacity = new_capacity;
	}
	report->entries[ report->count ] = *entry;
	report->count++;
	return 0;
}

int photorec_report_parser_parse(
     const char *xml_text,
     size_t xml_text_size,
     photorec_report_t *out )
{
	const char *cursor;

	if( ( xml_text == NULL )
	 || ( out == NULL ) )
	{
		return -1;
	}
	out->entries  = NULL;
	out->count    = 0;
	out->capacity = 0;

	cursor = xml_text;

	while( 1 )
	{
		size_t avail = xml_text_size - (size_t) ( cursor - xml_text );
		const char *block_start = bounded_find( cursor, avail, "<fileobject" );
		const char *tag_close;
		const char *block_end;
		size_t block_len;
		photorec_report_entry_t entry;
		char numeric_buf[ 32 ];

		if( block_start == NULL )
		{
			break;
		}
		avail = xml_text_size - (size_t) ( block_start - xml_text );
		tag_close = bounded_find( block_start, avail, ">" );
		if( tag_close == NULL )
		{
			break;
		}
		block_start = tag_close + 1;

		avail = xml_text_size - (size_t) ( block_start - xml_text );
		block_end = bounded_find( block_start, avail, "</fileobject>" );
		if( block_end == NULL )
		{
			break;
		}
		block_len = (size_t) ( block_end - block_start );

		memset( &entry, 0, sizeof( entry ) );

		if( extract_tag_content( block_start, block_len, "filename", entry.name, sizeof( entry.name ) ) == 0 )
		{
			if( extract_tag_content( block_start, block_len, "filesize", numeric_buf, sizeof( numeric_buf ) ) == 0 )
			{
				entry.file_size = strtoull( numeric_buf, NULL, 10 );
			}
			/* 找不到 img_offset（理论上不该发生，PhotoRec 的 report.xml
			 * 每个 fileobject 都带 byte_runs）时，entry.img_offset 保持
			 * memset 清零出来的 0，不当成解析失败——宁可给一个明显能
			 * 识别出"没拿到"的占位值，也不中断整份报告其它条目的解析 */
			extract_uint64_attribute( block_start, block_len, "img_offset", &entry.img_offset );

			if( photorec_report_append( out, &entry ) != 0 )
			{
				photorec_report_free( out );
				return -1;
			}
		}
		cursor = block_end + strlen( "</fileobject>" );
	}
	return 1;
}

void photorec_report_free(
      photorec_report_t *report )
{
	if( report == NULL )
	{
		return;
	}
	free( report->entries );
	report->entries  = NULL;
	report->count    = 0;
	report->capacity = 0;
}
