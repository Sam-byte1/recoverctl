#include "photorec_command_builder.h"

#include <stdio.h>
#include <string.h>

int photorec_command_builder_build_cmd_run(
     const char *arch_type_token,
     char *out_buf,
     size_t out_buf_size )
{
	int written;

	if( out_buf == NULL )
	{
		return -1;
	}
	if( ( arch_type_token == NULL )
	 || ( arch_type_token[ 0 ] == '\0' ) )
	{
		if( out_buf_size < strlen( "search" ) + 1 )
		{
			return -1;
		}
		strcpy( out_buf, "search" );
		return 1;
	}
	written = snprintf( out_buf, out_buf_size, "%s,search", arch_type_token );
	if( ( written < 0 )
	 || ( (size_t) written >= out_buf_size ) )
	{
		return -1;
	}
	return 1;
}
