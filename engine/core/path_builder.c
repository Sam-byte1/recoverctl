#include <string.h>

#include "path_builder.h"

int path_builder_prepend_segment(
     char *path,
     size_t path_buffer_size,
     const char *segment )
{
	size_t segment_length;
	size_t existing_length;
	size_t separator_length;
	size_t new_length;

	if( path == NULL || segment == NULL )
	{
		return -1;
	}

	segment_length  = strlen( segment );
	existing_length = strlen( path );
	separator_length = ( existing_length > 0 ) ? 1 : 0;
	new_length = segment_length + separator_length + existing_length;

	if( new_length + 1 > path_buffer_size )
	{
		/* 拼了会放不下——不破坏原有内容，让调用方自己决定怎么降级处理 */
		return -1;
	}

	/* 先把已有内容往后挪出 segment+分隔符 的空间，再把 segment（和分隔符）
	 * 填进腾出来的前面部分。memmove 而不是 memcpy，因为 path 既是源也是
	 * 目的地，两段内存必然重叠。 */
	memmove( path + segment_length + separator_length, path, existing_length + 1 );
	memcpy( path, segment, segment_length );
	if( separator_length > 0 )
	{
		path[ segment_length ] = '\\';
	}

	return 1;
}

int path_builder_append_segment(
     char *path,
     size_t path_buffer_size,
     const char *segment )
{
	size_t segment_length;
	size_t existing_length;
	size_t separator_length;
	size_t new_length;

	if( path == NULL || segment == NULL )
	{
		return -1;
	}

	segment_length  = strlen( segment );
	existing_length = strlen( path );
	separator_length = ( existing_length > 0 ) ? 1 : 0;
	new_length = existing_length + separator_length + segment_length;

	if( new_length + 1 > path_buffer_size )
	{
		return -1;
	}

	/* 拼到后面不需要挪动已有内容，直接从已有内容末尾开始写，比 prepend
	 * 简单——不存在源和目的地重叠的问题，用 memcpy 就够了 */
	if( separator_length > 0 )
	{
		path[ existing_length ] = '\\';
	}
	memcpy( path + existing_length + separator_length, segment, segment_length );
	path[ new_length ] = '\0';

	return 1;
}
