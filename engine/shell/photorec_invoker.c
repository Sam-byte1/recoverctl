#include "photorec_invoker.h"

#ifdef _WIN32

/* Windows 分支（D6，2026-11-05 VM真机实现+验证）：实现之前先在VM上手工
 * 跑通过整条链路，踩出了两个在Mac上完全想不到的真实坑，都已验证确认
 * 根因并修好，记录在函数注释里，不是凭对CreateProcess API的理解直接
 * 写代码：
 *
 * 坑①：ncurses在这个Windows交叉编译产物上默认拿不到可用的terminfo
 * 数据库——不是"没装"，是native Windows可执行文件不认MSYS2/POSIX风格
 * 的路径（`/clangarm64/share/terminfo`），必须显式把 TERMINFO 环境变量
 * 设成Windows风格路径（`C:\...\terminfo`）才能让ncurses初始化成功，
 * 否则不管TERM设成什么值（试过默认的xterm-256color和明确存在于
 * terminfo库里的xterm，结果一样）都会在ncurses初始化阶段直接退出。
 * 因此这个产品的Windows分发物必须跟着photorec.exe一起打包一份
 * terminfo目录（已验证9.6MB，整份打包，不值得为了省这点体积去抽取
 * 最小子集增加维护复杂度）。
 *
 * 坑②（只影响手工测试脚本，不影响这里的产品代码，记录是为了避免
 * 以后又被同一个坑绊一次）：用MSYS2 bash手工调用photorec.exe时，
 * bash会把看起来像POSIX路径的参数（`/log`/`/d`/`/cmd`这些"/"开头的
 * 参数）自动翻译成Windows路径，导致photorec.exe实际收到的是
 * "C:/msys64/log"这种错误参数——这是MSYS2 bash自己的参数自动转换
 * 行为，只在"用MSYS2 bash启动子进程"这个场景下发生。这里用
 * CreateProcessA直接构造命令行字符串，不经过任何shell，不会有这个
 * 问题，不需要做任何特殊处理。 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../core/photorec_command_builder.h"

static void split_path_windows(
     const char *path,
     char *out_dir,
     size_t out_dir_size,
     char *out_base,
     size_t out_base_size )
{
	const char *slash_fwd = strrchr( path, '/' );
	const char *slash_back = strrchr( path, '\\' );
	const char *slash = slash_fwd;

	if( ( slash == NULL )
	 || ( ( slash_back != NULL ) && ( slash_back > slash ) ) )
	{
		slash = slash_back;
	}
	if( slash == NULL )
	{
		snprintf( out_dir, out_dir_size, "." );
		snprintf( out_base, out_base_size, "%s", path );
	}
	else
	{
		size_t dir_len = (size_t) ( slash - path );

		if( dir_len == 0 )
		{
			dir_len = 1;
		}
		if( dir_len >= out_dir_size )
		{
			dir_len = out_dir_size - 1;
		}
		memcpy( out_dir, path, dir_len );
		out_dir[ dir_len ] = '\0';
		snprintf( out_base, out_base_size, "%s", slash + 1 );
	}
}

/* 和POSIX版同样的"跑前跑后各扫一次、比较最大编号变化"算法，换成
 * FindFirstFileA/FindNextFileA。 */
static int find_max_existing_suffix_windows(
     const char *parent_dir,
     const char *base_name )
{
	char search_pattern[ 1024 ];
	WIN32_FIND_DATAA find_data;
	HANDLE find_handle;
	size_t base_len = strlen( base_name );
	int max_n = 0;

	snprintf( search_pattern, sizeof( search_pattern ), "%s\\%s.*", parent_dir, base_name );

	find_handle = FindFirstFileA( search_pattern, &find_data );
	if( find_handle == INVALID_HANDLE_VALUE )
	{
		return 0;
	}
	do
	{
		if( ( strncmp( find_data.cFileName, base_name, base_len ) == 0 )
		 && ( find_data.cFileName[ base_len ] == '.' ) )
		{
			const char *suffix = find_data.cFileName + base_len + 1;
			char *endptr = NULL;
			long n = strtol( suffix, &endptr, 10 );

			if( ( endptr != suffix )
			 && ( *endptr == '\0' )
			 && ( n > max_n ) )
			{
				max_n = (int) n;
			}
		}
	}
	while( FindNextFileA( find_handle, &find_data ) != 0 );

	FindClose( find_handle );
	return max_n;
}

/* 把一个参数按Windows命令行规则加引号拼接进命令行字符串——不走任何
 * shell（CreateProcessA直接接收整条命令行字符串自己做token化），所以
 * 不会碰到"坑②"那种shell自己翻译参数的问题，但仍然需要正确处理参数
 * 内部的引号和反斜杠，否则路径里一旦带空格就会被错误拆分。 */
static int append_quoted_arg(
     char *out_buf,
     size_t out_buf_size,
     size_t *in_out_offset,
     const char *arg )
{
	size_t offset = *in_out_offset;
	size_t i;
	size_t arg_len = strlen( arg );

	if( offset > 0 )
	{
		if( offset + 1 >= out_buf_size )
		{
			return -1;
		}
		out_buf[ offset++ ] = ' ';
	}
	if( offset + 1 >= out_buf_size )
	{
		return -1;
	}
	out_buf[ offset++ ] = '"';

	for( i = 0; i < arg_len; i++ )
	{
		if( offset + 2 >= out_buf_size )
		{
			return -1;
		}
		if( ( arg[ i ] == '"' ) || ( arg[ i ] == '\\' ) )
		{
			out_buf[ offset++ ] = '\\';
		}
		out_buf[ offset++ ] = arg[ i ];
	}
	if( offset + 2 >= out_buf_size )
	{
		return -1;
	}
	out_buf[ offset++ ] = '"';
	out_buf[ offset ] = '\0';
	*in_out_offset = offset;
	return 0;
}

int photorec_invoker_run(
     const char *photorec_executable_path,
     const char *target_path,
     const char *arch_type_token,
     const char *output_dir_base,
     photorec_invoke_result_t *out_result )
{
	char cmd_run[ 64 ];
	char parent_dir[ 768 ];
	char base_name[ 256 ];
	char photorec_dir[ 768 ];
	char photorec_exe_name[ 256 ];
	char terminfo_path[ 900 ];
	char command_line[ 4096 ];
	size_t offset = 0;
	int suffix_before;
	int suffix_after;
	STARTUPINFOA startup_info;
	PROCESS_INFORMATION process_info;
	SECURITY_ATTRIBUTES security_attributes;
	HANDLE null_read_handle;
	HANDLE null_write_handle;
	DWORD exit_code = 0;
	char previous_term[ 256 ];
	char previous_terminfo[ 900 ];
	DWORD previous_term_length;
	DWORD previous_terminfo_length;
	BOOL had_previous_term;
	BOOL had_previous_terminfo;

	if( ( photorec_executable_path == NULL )
	 || ( target_path == NULL )
	 || ( output_dir_base == NULL )
	 || ( out_result == NULL ) )
	{
		return -1;
	}
	memset( out_result, 0, sizeof( *out_result ) );

	if( photorec_command_builder_build_cmd_run( arch_type_token, cmd_run, sizeof( cmd_run ) ) != 1 )
	{
		return -1;
	}
	split_path_windows( output_dir_base, parent_dir, sizeof( parent_dir ), base_name, sizeof( base_name ) );
	suffix_before = find_max_existing_suffix_windows( parent_dir, base_name );

	/* 坑①的修复：TERMINFO必须指向photorec.exe自己同目录下的terminfo
	 * 子目录（产品分发时这两样东西放在一起，见 docs/开发日志.md D5/D6
	 * 记录），而不是开发机上MSYS2安装目录那个路径——那个路径只在
	 * 构建这台VM上存在，终端用户的机器上不会有MSYS2。 */
	split_path_windows( photorec_executable_path, photorec_dir, sizeof( photorec_dir ), photorec_exe_name, sizeof( photorec_exe_name ) );
	snprintf( terminfo_path, sizeof( terminfo_path ), "%s\\terminfo", photorec_dir );

	offset = 0;
	if( ( append_quoted_arg( command_line, sizeof( command_line ), &offset, photorec_executable_path ) != 0 )
	 || ( append_quoted_arg( command_line, sizeof( command_line ), &offset, "/log" ) != 0 )
	 || ( append_quoted_arg( command_line, sizeof( command_line ), &offset, "/d" ) != 0 )
	 || ( append_quoted_arg( command_line, sizeof( command_line ), &offset, output_dir_base ) != 0 )
	 || ( append_quoted_arg( command_line, sizeof( command_line ), &offset, "/cmd" ) != 0 )
	 || ( append_quoted_arg( command_line, sizeof( command_line ), &offset, target_path ) != 0 )
	 || ( append_quoted_arg( command_line, sizeof( command_line ), &offset, cmd_run ) != 0 ) )
	{
		return -1;
	}

	/* 用进程级环境变量而不是自己拼一份完整的环境块传给CreateProcessA：
	 * lpEnvironment传NULL时子进程直接继承父进程当前的环境，修改完
	 * 用完就还原，不影响recoverctl自身其它逻辑、也不需要手写环境块
	 * 的双NUL结尾这类容易出错的细节。 */
	had_previous_term = GetEnvironmentVariableA( "TERM", previous_term, sizeof( previous_term ) ) > 0;
	previous_term_length = had_previous_term ? (DWORD) strlen( previous_term ) : 0;
	(void) previous_term_length;
	had_previous_terminfo = GetEnvironmentVariableA( "TERMINFO", previous_terminfo, sizeof( previous_terminfo ) ) > 0;
	previous_terminfo_length = had_previous_terminfo ? (DWORD) strlen( previous_terminfo ) : 0;
	(void) previous_terminfo_length;

	SetEnvironmentVariableA( "TERM", "xterm" );
	SetEnvironmentVariableA( "TERMINFO", terminfo_path );

	security_attributes.nLength = sizeof( SECURITY_ATTRIBUTES );
	security_attributes.lpSecurityDescriptor = NULL;
	security_attributes.bInheritHandle = TRUE;

	/* stdin接到NUL（Windows下"/dev/null"的等价物）：第27步实测确认的
	 * 关键前提，保证任何意外的交互式等待都会立刻拿到EOF。stdout/stderr
	 * 同样接到NUL：PhotoRec自己的ncurses输出是大量转义序列，不是给
	 * 调用方看的，真正的结果看report.xml（用
	 * engine/core/photorec_report_parser.c解析）。 */
	null_read_handle = CreateFileA( "NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                                 &security_attributes, OPEN_EXISTING, 0, NULL );
	null_write_handle = CreateFileA( "NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                                  &security_attributes, OPEN_EXISTING, 0, NULL );
	if( ( null_read_handle == INVALID_HANDLE_VALUE )
	 || ( null_write_handle == INVALID_HANDLE_VALUE ) )
	{
		if( null_read_handle != INVALID_HANDLE_VALUE )
		{
			CloseHandle( null_read_handle );
		}
		if( null_write_handle != INVALID_HANDLE_VALUE )
		{
			CloseHandle( null_write_handle );
		}
		return -1;
	}

	memset( &startup_info, 0, sizeof( startup_info ) );
	startup_info.cb = sizeof( startup_info );
	startup_info.dwFlags = STARTF_USESTDHANDLES;
	startup_info.hStdInput = null_read_handle;
	startup_info.hStdOutput = null_write_handle;
	startup_info.hStdError = null_write_handle;

	memset( &process_info, 0, sizeof( process_info ) );

	if( !CreateProcessA( NULL, command_line, NULL, NULL, TRUE, 0, NULL, NULL, &startup_info, &process_info ) )
	{
		CloseHandle( null_read_handle );
		CloseHandle( null_write_handle );
		if( had_previous_term ) { SetEnvironmentVariableA( "TERM", previous_term ); } else { SetEnvironmentVariableA( "TERM", NULL ); }
		if( had_previous_terminfo ) { SetEnvironmentVariableA( "TERMINFO", previous_terminfo ); } else { SetEnvironmentVariableA( "TERMINFO", NULL ); }
		return -1;
	}
	CloseHandle( null_read_handle );
	CloseHandle( null_write_handle );

	WaitForSingleObject( process_info.hProcess, INFINITE );
	if( !GetExitCodeProcess( process_info.hProcess, &exit_code ) )
	{
		exit_code = (DWORD) -1;
	}
	CloseHandle( process_info.hProcess );
	CloseHandle( process_info.hThread );

	if( had_previous_term ) { SetEnvironmentVariableA( "TERM", previous_term ); } else { SetEnvironmentVariableA( "TERM", NULL ); }
	if( had_previous_terminfo ) { SetEnvironmentVariableA( "TERMINFO", previous_terminfo ); } else { SetEnvironmentVariableA( "TERMINFO", NULL ); }

	out_result->exit_code = (int) exit_code;

	suffix_after = find_max_existing_suffix_windows( parent_dir, base_name );
	if( suffix_after > suffix_before )
	{
		snprintf( out_result->recup_dir_path, sizeof( out_result->recup_dir_path ),
		          "%s.%d", output_dir_base, suffix_after );
	}
	return 1;
}

#else

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "../core/photorec_command_builder.h"

/* 有些平台的 <unistd.h> 在严格的 -std=c11 模式下不会隐式声明 environ，
 * 手写声明是项目里一贯的做法（参见 ntfs_bfio_adapter.c 对
 * libbfio_handle_initialize 的处理），不依赖隐式声明。 */
extern char **environ;

static void split_path(
     const char *path,
     char *out_dir,
     size_t out_dir_size,
     char *out_base,
     size_t out_base_size )
{
	const char *slash = strrchr( path, '/' );

	if( slash == NULL )
	{
		snprintf( out_dir, out_dir_size, "." );
		snprintf( out_base, out_base_size, "%s", path );
	}
	else
	{
		size_t dir_len = (size_t) ( slash - path );

		if( dir_len == 0 )
		{
			dir_len = 1; /* 根目录 "/" 的情况 */
		}
		if( dir_len >= out_dir_size )
		{
			dir_len = out_dir_size - 1;
		}
		memcpy( out_dir, path, dir_len );
		out_dir[ dir_len ] = '\0';
		snprintf( out_base, out_base_size, "%s", slash + 1 );
	}
}

/* 在 parent_dir 下找所有形如 "<base_name>.N" 的目录项，返回其中最大的
 * N（没有任何匹配项时返回 0）。PhotoRec 自己的行为是每次都挑"比当前
 * 已存在的最大编号再大 1"的新编号（已在第27步实测反复验证），所以
 * 跑之前/跑之后各扫一次、比较 N 的变化，就能确定性地定位到这次新创建
 * 的那个目录，不需要用任何更脆弱的"猜文件创建时间"之类的办法。 */
static int find_max_existing_suffix(
     const char *parent_dir,
     const char *base_name )
{
	DIR *dir = opendir( parent_dir );
	struct dirent *entry;
	size_t base_len = strlen( base_name );
	int max_n = 0;

	if( dir == NULL )
	{
		return 0;
	}
	while( ( entry = readdir( dir ) ) != NULL )
	{
		if( ( strncmp( entry->d_name, base_name, base_len ) == 0 )
		 && ( entry->d_name[ base_len ] == '.' ) )
		{
			const char *suffix = entry->d_name + base_len + 1;
			char *endptr = NULL;
			long n = strtol( suffix, &endptr, 10 );

			if( ( endptr != suffix )
			 && ( *endptr == '\0' )
			 && ( n > max_n ) )
			{
				max_n = (int) n;
			}
		}
	}
	closedir( dir );
	return max_n;
}

int photorec_invoker_run(
     const char *photorec_executable_path,
     const char *target_path,
     const char *arch_type_token,
     const char *output_dir_base,
     photorec_invoke_result_t *out_result )
{
	char cmd_run[ 64 ];
	char parent_dir[ 768 ];
	char base_name[ 256 ];
	int suffix_before;
	int suffix_after;
	pid_t pid;
	int spawn_status;
	int wait_status;
	posix_spawn_file_actions_t file_actions;
	char *argv[ 8 ];

	if( ( photorec_executable_path == NULL )
	 || ( target_path == NULL )
	 || ( output_dir_base == NULL )
	 || ( out_result == NULL ) )
	{
		return -1;
	}
	memset( out_result, 0, sizeof( *out_result ) );

	if( photorec_command_builder_build_cmd_run( arch_type_token, cmd_run, sizeof( cmd_run ) ) != 1 )
	{
		return -1;
	}
	split_path( output_dir_base, parent_dir, sizeof( parent_dir ), base_name, sizeof( base_name ) );
	suffix_before = find_max_existing_suffix( parent_dir, base_name );

	argv[ 0 ] = (char *) photorec_executable_path;
	argv[ 1 ] = (char *) "/log";
	argv[ 2 ] = (char *) "/d";
	argv[ 3 ] = (char *) output_dir_base;
	argv[ 4 ] = (char *) "/cmd";
	argv[ 5 ] = (char *) target_path;
	argv[ 6 ] = cmd_run;
	argv[ 7 ] = NULL;

	if( posix_spawn_file_actions_init( &file_actions ) != 0 )
	{
		return -1;
	}
	/* stdin 固定接到 /dev/null：第27步实测确认的关键前提，保证任何
	 * 意外的交互式等待都会立刻拿到 EOF，不会真的卡住子进程。
	 * stdout/stderr 同样接到 /dev/null：PhotoRec 自己的 ncurses 输出
	 * 是大量转义序列，不是给调用方看的；真正的结果看 /log 产生的
	 * photorec.log 和 report.xml，不依赖捕获这里的终端输出。 */
	posix_spawn_file_actions_addopen( &file_actions, 0, "/dev/null", O_RDONLY, 0 );
	posix_spawn_file_actions_addopen( &file_actions, 1, "/dev/null", O_WRONLY, 0 );
	posix_spawn_file_actions_addopen( &file_actions, 2, "/dev/null", O_WRONLY, 0 );

	spawn_status = posix_spawn( &pid, photorec_executable_path, &file_actions, NULL, argv, environ );
	posix_spawn_file_actions_destroy( &file_actions );

	if( spawn_status != 0 )
	{
		return -1;
	}
	if( waitpid( pid, &wait_status, 0 ) == -1 )
	{
		return -1;
	}
	if( WIFEXITED( wait_status ) )
	{
		out_result->exit_code = WEXITSTATUS( wait_status );
	}
	else
	{
		out_result->exit_code = -1;
	}
	suffix_after = find_max_existing_suffix( parent_dir, base_name );
	if( suffix_after > suffix_before )
	{
		snprintf( out_result->recup_dir_path, sizeof( out_result->recup_dir_path ),
		          "%s.%d", output_dir_base, suffix_after );
	}
	return 1;
}

#endif /* _WIN32 */
