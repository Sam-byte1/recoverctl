#include "utf16_to_utf8.h"

#define UNICODE_REPLACEMENT_CHARACTER 0xFFFD

static uint16_t read_u16_le( const uint8_t *buffer, size_t offset )
{
	return (uint16_t) ( buffer[ offset ] | ( buffer[ offset + 1 ] << 8 ) );
}

/* 把一个码点（已经从 UTF-16 代理对或者单个码元解出来）编码成 UTF-8，
 * 写进 out 开头，返回用掉的字节数（1~4）。调用方保证 out 至少有 4 字节
 * 空间可写（真正写不写得下由调用方在外层统一检查，这里只管编码规则）。 */
static int encode_utf8_code_point( uint32_t code_point, uint8_t *out )
{
	if( code_point <= 0x7F )
	{
		out[ 0 ] = (uint8_t) code_point;
		return 1;
	}
	if( code_point <= 0x7FF )
	{
		out[ 0 ] = (uint8_t) ( 0xC0 | ( code_point >> 6 ) );
		out[ 1 ] = (uint8_t) ( 0x80 | ( code_point & 0x3F ) );
		return 2;
	}
	if( code_point <= 0xFFFF )
	{
		out[ 0 ] = (uint8_t) ( 0xE0 | ( code_point >> 12 ) );
		out[ 1 ] = (uint8_t) ( 0x80 | ( ( code_point >> 6 ) & 0x3F ) );
		out[ 2 ] = (uint8_t) ( 0x80 | ( code_point & 0x3F ) );
		return 3;
	}
	out[ 0 ] = (uint8_t) ( 0xF0 | ( code_point >> 18 ) );
	out[ 1 ] = (uint8_t) ( 0x80 | ( ( code_point >> 12 ) & 0x3F ) );
	out[ 2 ] = (uint8_t) ( 0x80 | ( ( code_point >> 6 ) & 0x3F ) );
	out[ 3 ] = (uint8_t) ( 0x80 | ( code_point & 0x3F ) );
	return 4;
}

int utf16le_to_utf8(
     const uint8_t *utf16le_bytes,
     size_t num_utf16_units,
     char *out_utf8,
     size_t out_utf8_size )
{
	size_t unit_index;
	size_t out_offset = 0;
	uint8_t encode_buffer[ 4 ];

	if( utf16le_bytes == NULL || out_utf8 == NULL || out_utf8_size == 0 )
	{
		return -1;
	}

	for( unit_index = 0; unit_index < num_utf16_units; unit_index++ )
	{
		uint16_t unit = read_u16_le( utf16le_bytes, unit_index * 2 );
		uint32_t code_point;
		int encoded_length;

		if( unit >= 0xD800 && unit <= 0xDBFF )
		{
			/* 高代理项：必须紧跟一个低代理项才能组成一个完整码点 */
			if( ( unit_index + 1 < num_utf16_units ) )
			{
				uint16_t low = read_u16_le( utf16le_bytes, ( unit_index + 1 ) * 2 );

				if( low >= 0xDC00 && low <= 0xDFFF )
				{
					code_point = 0x10000
					           + ( ( (uint32_t) ( unit - 0xD800 ) ) << 10 )
					           + ( low - 0xDC00 );
					unit_index++;  /* 低代理项已经消费掉了，多跳过一个码元 */
				}
				else
				{
					/* 高代理项后面不是合法的低代理项，孤立代理项处理 */
					code_point = UNICODE_REPLACEMENT_CHARACTER;
				}
			}
			else
			{
				/* 高代理项是最后一个码元，没有后续可配对，孤立代理项处理 */
				code_point = UNICODE_REPLACEMENT_CHARACTER;
			}
		}
		else if( unit >= 0xDC00 && unit <= 0xDFFF )
		{
			/* 孤立的低代理项（前面没有配对的高代理项） */
			code_point = UNICODE_REPLACEMENT_CHARACTER;
		}
		else
		{
			code_point = unit;
		}

		encoded_length = encode_utf8_code_point( code_point, encode_buffer );

		if( out_offset + (size_t) encoded_length + 1 > out_utf8_size )
		{
			/* 装不下这个完整字符了——到此截断，不写半个字符的残缺字节 */
			break;
		}

		{
			int k;
			for( k = 0; k < encoded_length; k++ )
			{
				out_utf8[ out_offset + k ] = (char) encode_buffer[ k ];
			}
		}
		out_offset += (size_t) encoded_length;
	}

	out_utf8[ out_offset ] = '\0';
	return (int) out_offset;
}
