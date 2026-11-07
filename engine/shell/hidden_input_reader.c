#include <stdio.h>

#include "hidden_input_reader.h"
#include "secure_memory.h"

#ifdef _WIN32
#include <conio.h>

int hidden_input_reader_read(
     const char *prompt_message,
     hidden_input_state_t *out )
{
	int c;
	hidden_input_action_t action;

	if( out == NULL )
	{
		return -1;
	}
	if( prompt_message != NULL )
	{
		printf( "%s", prompt_message );
		fflush( stdout );
	}
	hidden_input_core_init( out );

	while( 1 )
	{
		c = _getch();

		/* 扩展键前缀（方向键/功能键等）：第一个字节是0或0xE0，紧跟着
		 * 第二个字节才是真正的扫描码，对密码输入没有意义，两个字节
		 * 一起吞掉丢弃，不喂给状态机——否则第一个字节会被状态机当成
		 * 不认识的控制字符拒绝，第二个字节又会被误当成一次独立按键
		 * 处理，两次都是错的。 */
		if( ( c == 0 ) || ( c == 0xE0 ) )
		{
			_getch();
			continue;
		}

		action = hidden_input_core_feed( out, c );

		if( action == HIDDEN_INPUT_ACTION_COMPLETE )
		{
			printf( "\n" );
			return 1;
		}
		if( action == HIDDEN_INPUT_ACTION_CANCELLED )
		{
			printf( "\n" );
			secure_memory_zero( out->buffer, sizeof( out->buffer ) );
			out->length = 0;
			return 0;
		}
		/* CONTINUE/REJECTED都继续等下一个字符，REJECTED不是致命错误 */
	}
}
#else
#include <termios.h>
#include <unistd.h>

int hidden_input_reader_read(
     const char *prompt_message,
     hidden_input_state_t *out )
{
	struct termios original_attrs;
	struct termios raw_attrs;
	int return_code = -1;
	int terminal_modified = 0;

	if( out == NULL )
	{
		return -1;
	}
	if( tcgetattr( STDIN_FILENO, &original_attrs ) != 0 )
	{
		fprintf( stderr, "无法读取终端属性（标准输入不是一个真实终端？交互式输入渠道需要在真实终端里运行）\n" );
		return -1;
	}
	raw_attrs = original_attrs;
	/* 关ECHO（回显）+ICANON（行缓冲，不然要等一整行才能逐字符处理退格）
	 * +ISIG（否则Ctrl+C会被终端驱动直接转成SIGINT杀掉/中断进程，状态机
	 * 永远收不到0x03这个字节，CANCELLED分支就成了死代码） */
	raw_attrs.c_lflag &= (tcflag_t) ~( ECHO | ICANON | ISIG );
	raw_attrs.c_cc[ VMIN ]  = 1;
	raw_attrs.c_cc[ VTIME ] = 0;

	if( tcsetattr( STDIN_FILENO, TCSAFLUSH, &raw_attrs ) != 0 )
	{
		return -1;
	}
	terminal_modified = 1;

	if( prompt_message != NULL )
	{
		printf( "%s", prompt_message );
		fflush( stdout );
	}
	hidden_input_core_init( out );

	while( 1 )
	{
		unsigned char c;
		ssize_t read_result;
		hidden_input_action_t action;

		read_result = read( STDIN_FILENO, &c, 1 );
		if( read_result <= 0 )
		{
			/* 读取失败（终端被意外关闭等）——不留下半成品凭据 */
			secure_memory_zero( out->buffer, sizeof( out->buffer ) );
			out->length = 0;
			return_code = -1;
			goto cleanup;
		}

		action = hidden_input_core_feed( out, (int) c );

		if( action == HIDDEN_INPUT_ACTION_COMPLETE )
		{
			printf( "\n" );
			return_code = 1;
			goto cleanup;
		}
		if( action == HIDDEN_INPUT_ACTION_CANCELLED )
		{
			printf( "\n" );
			secure_memory_zero( out->buffer, sizeof( out->buffer ) );
			out->length = 0;
			return_code = 0;
			goto cleanup;
		}
		/* CONTINUE/REJECTED都继续等下一个字符 */
	}

cleanup:
	if( terminal_modified )
	{
		/* 不管上面走的是哪条返回路径，终端属性都必须恢复成进入这个
		 * 函数之前的原样——这是调用方没有能力替它做的责任 */
		tcsetattr( STDIN_FILENO, TCSAFLUSH, &original_attrs );
	}
	return return_code;
}
#endif
