#include "bitlocker_credential_source.h"

#include <stdio.h>
#include <string.h>

#include "secure_memory.h"
#include "hidden_input_reader.h"

/* 读取文件第一行，去掉行尾的 \r\n 或 \n（只去行终止符，不trim其它
 * 空白——密码里的前后空格是用户输入的一部分，这一层不应该悄悄改掉），
 * 结果写成一个NUL结尾的字符串。buffer_size 包含结尾NUL的空间。
 * 返回 1 成功，-1 失败（打不开文件、内容超出buffer_size）。 */
static int read_first_line(
     const char *file_path,
     char *buffer,
     size_t buffer_size )
{
	FILE *f;
	size_t length = 0;
	int c;

	f = fopen( file_path, "rb" );
	if( f == NULL )
	{
		fprintf( stderr, "无法打开文件: %s\n", file_path );
		return -1;
	}
	while( ( c = fgetc( f ) ) != EOF )
	{
		if( c == '\n' )
		{
			break;
		}
		if( length + 1 >= buffer_size )
		{
			fclose( f );
			fprintf( stderr, "文件内容超出缓冲区容量: %s\n", file_path );
			return -1;
		}
		buffer[ length ] = (char) c;
		length++;
	}
	fclose( f );

	if( ( length > 0 ) && ( buffer[ length - 1 ] == '\r' ) )
	{
		length--;
	}
	buffer[ length ] = '\0';
	return 1;
}

/* 下面这几个 resolve_*_from_text 函数不碰文件/argv——只负责"已经拿到
 * 一段NUL结尾的文本之后该怎么解析"，B5（文件渠道）和B10（命令行参数
 * 渠道）共用同一套解析逻辑，只是文本的来源不同（文件第一行 vs
 * argv里的一个参数），避免同样的解析规则写两遍、以后改一处漏改另一处。 */

static int resolve_password_from_text(
     const char *text,
     bitlocker_resolved_credential_t *out )
{
	out->text.length = strlen( text );
	if( out->text.length >= sizeof( out->text.data ) )
	{
		fprintf( stderr, "密码长度超出缓冲区容量\n" );
		return -1;
	}
	memcpy( out->text.data, text, out->text.length );
	return 1;
}

static int resolve_recovery_key_from_text(
     const char *text,
     bitlocker_resolved_credential_t *out )
{
	char normalized_buffer[ BITLOCKER_CREDENTIAL_MAX_LENGTH ];
	int result;

	result = bitlocker_credential_normalize_recovery_key( text, normalized_buffer, sizeof( normalized_buffer ) );
	if( result != 1 )
	{
		secure_memory_zero( normalized_buffer, sizeof( normalized_buffer ) );
		fprintf( stderr, "恢复密钥格式规范化失败\n" );
		return -1;
	}
	out->text.length = strlen( normalized_buffer );
	memcpy( out->text.data, normalized_buffer, out->text.length );

	secure_memory_zero( normalized_buffer, sizeof( normalized_buffer ) );
	return 1;
}

static int resolve_fvek_from_text(
     const char *text,
     bitlocker_resolved_credential_t *out )
{
	char line_buffer[ BITLOCKER_CREDENTIAL_MAX_LENGTH ];
	char *separator;
	int result;

	if( strlen( text ) >= sizeof( line_buffer ) )
	{
		fprintf( stderr, "FVEK/TWEAK文本长度超出缓冲区容量\n" );
		return -1;
	}
	strcpy( line_buffer, text );

	separator = strchr( line_buffer, ':' );
	if( separator != NULL )
	{
		*separator = '\0';
	}
	result = bitlocker_credential_hex_decode( line_buffer, out->fvek, sizeof( out->fvek ), &out->fvek_length );
	if( result == 1 )
	{
		if( separator != NULL )
		{
			result = bitlocker_credential_hex_decode( separator + 1, out->tweak, sizeof( out->tweak ), &out->tweak_length );
		}
		else
		{
			out->tweak_length = 0;
		}
	}
	secure_memory_zero( line_buffer, sizeof( line_buffer ) );

	if( result != 1 )
	{
		fprintf( stderr, "FVEK/TWEAK十六进制格式错误\n" );
		return -1;
	}
	return 1;
}

static int load_startup_key_path(
     const char *file_path,
     bitlocker_resolved_credential_t *out )
{
	size_t path_length = strlen( file_path );

	/* 这个类型不读取文件内容，只是把路径原样记下来，留给B6调用
	 * libbde_volume_read_startup_key 时直接用——我们完全不经手密钥
	 * 字节本身，不需要（也没有义务）清零这里的路径字符串。 */
	if( path_length + 1 > sizeof( out->startup_key_file_path ) )
	{
		fprintf( stderr, "启动密钥文件路径过长: %s\n", file_path );
		return -1;
	}
	memcpy( out->startup_key_file_path, file_path, path_length + 1 );
	return 1;
}

int bitlocker_credential_source_load_from_file(
     bitlocker_credential_type_t type,
     const char *file_path,
     bitlocker_resolved_credential_t *out )
{
	char line_buffer[ BITLOCKER_CREDENTIAL_MAX_LENGTH ];
	int result;

	if( ( file_path == NULL )
	 || ( out == NULL ) )
	{
		return -1;
	}
	memset( out, 0, sizeof( *out ) );
	out->type = type;

	if( type == BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY )
	{
		return load_startup_key_path( file_path, out );
	}

	result = read_first_line( file_path, line_buffer, sizeof( line_buffer ) );
	if( result != 1 )
	{
		secure_memory_zero( line_buffer, sizeof( line_buffer ) );
		return -1;
	}
	switch( type )
	{
		case BITLOCKER_CREDENTIAL_TYPE_PASSWORD:
			result = resolve_password_from_text( line_buffer, out );
			break;

		case BITLOCKER_CREDENTIAL_TYPE_RECOVERY_KEY:
			result = resolve_recovery_key_from_text( line_buffer, out );
			break;

		case BITLOCKER_CREDENTIAL_TYPE_FVEK:
			result = resolve_fvek_from_text( line_buffer, out );
			break;

		default:
			fprintf( stderr, "未知的凭据类型\n" );
			result = -1;
			break;
	}
	secure_memory_zero( line_buffer, sizeof( line_buffer ) );
	return result;
}

/* B10（2026-11-05 任务拆解排期）：命令行参数渠道。只是换了个文本来源
 * （argv里的一个参数，不是文件第一行），解析规则和B5完全复用
 * resolve_*_from_text。STARTUP_KEY类型不接受这个渠道——libbde的
 * `read_startup_key` API本身吃的就是文件路径，不是密钥内容字符串，
 * 命令行参数传一段"密钥内容"进来没有意义，这是libbde API本身的形状
 * 决定的，不是本项目刻意加的限制。 */
int bitlocker_credential_source_load_from_cli_argument(
     bitlocker_credential_type_t type,
     const char *argument_value,
     bitlocker_resolved_credential_t *out )
{
	if( ( argument_value == NULL )
	 || ( out == NULL ) )
	{
		return -1;
	}
	memset( out, 0, sizeof( *out ) );
	out->type = type;

	switch( type )
	{
		case BITLOCKER_CREDENTIAL_TYPE_PASSWORD:
			return resolve_password_from_text( argument_value, out );

		case BITLOCKER_CREDENTIAL_TYPE_RECOVERY_KEY:
			return resolve_recovery_key_from_text( argument_value, out );

		case BITLOCKER_CREDENTIAL_TYPE_FVEK:
			return resolve_fvek_from_text( argument_value, out );

		case BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY:
			fprintf( stderr, "启动密钥类型不支持从命令行参数渠道传入，只能用文件渠道（libbde的API本身只接受文件路径）\n" );
			return -1;

		default:
			fprintf( stderr, "未知的凭据类型\n" );
			return -1;
	}
}

/* B11c（2026-11-05 任务拆解排期）：交互式隐藏输入渠道。和B10同样只是
 * 换了个文本来源（现场从终端不回显读出来的，不是argv），解析规则
 * 继续复用resolve_*_from_text——STARTUP_KEY同样不支持（和B10同样的
 * 理由：libbde的API只接受文件路径，而且路径本身也不是需要隐藏的
 * 敏感内容）。 */
int bitlocker_credential_source_load_from_interactive(
     bitlocker_credential_type_t type,
     bitlocker_resolved_credential_t *out )
{
	hidden_input_state_t input;
	char text_buffer[ HIDDEN_INPUT_MAX_LENGTH + 1 ];
	int read_result;
	int result;

	if( out == NULL )
	{
		return -1;
	}
	if( type == BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY )
	{
		fprintf( stderr, "启动密钥类型不支持交互式输入渠道，只能用文件渠道（libbde的API本身只接受文件路径）\n" );
		return -1;
	}
	memset( out, 0, sizeof( *out ) );
	out->type = type;

	read_result = hidden_input_reader_read( "请输入凭据内容（不会回显）: ", &input );
	if( read_result != 1 )
	{
		/* read_result==0：用户按Ctrl+C主动取消，input已经被
		 * hidden_input_reader_read自己清零过，不是需要打印错误的失败；
		 * read_result==-1：终端层系统错误，已经没有明文残留需要清理 */
		return read_result == 0 ? 0 : -1;
	}

	/* hidden_input_state_t.buffer不是NUL结尾的字符串——resolve_*_from_text
	 * 系列函数都假设NUL结尾，这里拷贝成NUL结尾字符串再复用同一套解析
	 * 逻辑，不重新发明一遍解析规则 */
	memcpy( text_buffer, input.buffer, input.length );
	text_buffer[ input.length ] = '\0';
	secure_memory_zero( input.buffer, sizeof( input.buffer ) );
	input.length = 0;

	switch( type )
	{
		case BITLOCKER_CREDENTIAL_TYPE_PASSWORD:
			result = resolve_password_from_text( text_buffer, out );
			break;

		case BITLOCKER_CREDENTIAL_TYPE_RECOVERY_KEY:
			result = resolve_recovery_key_from_text( text_buffer, out );
			break;

		case BITLOCKER_CREDENTIAL_TYPE_FVEK:
			result = resolve_fvek_from_text( text_buffer, out );
			break;

		default:
			fprintf( stderr, "未知的凭据类型\n" );
			result = -1;
			break;
	}
	secure_memory_zero( text_buffer, sizeof( text_buffer ) );
	return result;
}
