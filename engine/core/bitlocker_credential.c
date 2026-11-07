#include "bitlocker_credential.h"

static int hex_nibble_value(
     char c,
     uint8_t *out_value )
{
	if( ( c >= '0' ) && ( c <= '9' ) )
	{
		*out_value = (uint8_t) ( c - '0' );
		return 1;
	}
	if( ( c >= 'a' ) && ( c <= 'f' ) )
	{
		*out_value = (uint8_t) ( c - 'a' + 10 );
		return 1;
	}
	if( ( c >= 'A' ) && ( c <= 'F' ) )
	{
		*out_value = (uint8_t) ( c - 'A' + 10 );
		return 1;
	}
	return 0;
}

int bitlocker_credential_normalize_recovery_key(
     const char *input,
     char *out,
     size_t out_size )
{
	size_t in_index;
	size_t digit_count = 0;
	size_t out_index = 0;

	if( ( input == NULL )
	 || ( out == NULL )
	 || ( out_size == 0 ) )
	{
		return -1;
	}
	for( in_index = 0; input[ in_index ] != '\0'; in_index++ )
	{
		if( ( input[ in_index ] >= '0' ) && ( input[ in_index ] <= '9' ) )
		{
			if( ( digit_count > 0 ) && ( ( digit_count % 6 ) == 0 ) )
			{
				if( out_index + 1 >= out_size )
				{
					return -1;
				}
				out[ out_index ] = '-';
				out_index++;
			}
			if( out_index + 1 >= out_size )
			{
				return -1;
			}
			out[ out_index ] = input[ in_index ];
			out_index++;
			digit_count++;
		}
	}
	out[ out_index ] = '\0';
	return 1;
}

int bitlocker_credential_hex_decode(
     const char *hex_string,
     uint8_t *out,
     size_t out_capacity,
     size_t *out_length )
{
	size_t hex_len;
	size_t byte_count;
	size_t i;

	if( ( hex_string == NULL )
	 || ( out_length == NULL )
	 || ( ( out == NULL ) && ( out_capacity != 0 ) ) )
	{
		return -1;
	}
	hex_len = 0;
	while( hex_string[ hex_len ] != '\0' )
	{
		hex_len++;
	}
	if( ( hex_len % 2 ) != 0 )
	{
		return -1;
	}
	byte_count = hex_len / 2;
	if( byte_count > out_capacity )
	{
		return -1;
	}
	for( i = 0; i < byte_count; i++ )
	{
		uint8_t high_nibble;
		uint8_t low_nibble;

		if( ( hex_nibble_value( hex_string[ i * 2 ], &high_nibble ) != 1 )
		 || ( hex_nibble_value( hex_string[ i * 2 + 1 ], &low_nibble ) != 1 ) )
		{
			return -1;
		}
		out[ i ] = (uint8_t) ( ( high_nibble << 4 ) | low_nibble );
	}
	*out_length = byte_count;
	return 1;
}
