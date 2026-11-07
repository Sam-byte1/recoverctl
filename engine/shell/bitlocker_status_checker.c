#include "bitlocker_status_checker.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/* 纯ASCII内容——这个项目在AGENTS.md第8节记录过多次PowerShell脚本一旦
 * 混入非ASCII字符就可能触发各种诡异的静默失败/解析错误，这里虽然是
 * recoverctl.exe自己运行时现写现用的临时脚本（不经过prlctl exec那条
 * 更容易出问题的链路），仍然按同样保守的标准保持纯ASCII，不留隐患。
 * 用参数（$driveLetter）而不是在C代码里拼接字符串进PowerShell命令行
 * ——避免盘符之外的任何用户可控内容进入这个脚本。 */
static const char *kProbeScript =
	"param([string]$driveLetter)\r\n"
	"$ErrorActionPreference = 'SilentlyContinue'\r\n"
	"$v = Get-WmiObject -Namespace \"root\\cimv2\\security\\microsoftvolumeencryption\" -Class Win32_EncryptableVolume -Filter \"DriveLetter='$driveLetter'\"\r\n"
	"if ($null -eq $v) {\r\n"
	"    Write-Output \"NOENTITY\"\r\n"
	"} else {\r\n"
	"    $lock = $v.GetLockStatus()\r\n"
	"    $lockStatus = $lock.LockStatus\r\n"
	"    if ($lockStatus -eq 0) {\r\n"
	"        $prot = $v.GetProtectionStatus()\r\n"
	"        $conv = $v.GetConversionStatus()\r\n"
	"        Write-Output \"OK|$lockStatus|$($prot.ProtectionStatus)|$($conv.EncryptionPercentage)\"\r\n"
	"    } else {\r\n"
	"        Write-Output \"OK|$lockStatus|-1|-1\"\r\n"
	"    }\r\n"
	"}\r\n";

/* 把探测脚本写到一个临时文件——不是每次现场拼一条超长的-Command
 * 字符串：这个项目在AGENTS.md里记录过"通过bash->prlctl->cmd.exe这条
 * 链条传递复合命令容易在引号/转义上出问题"的教训，虽然这里是
 * recoverctl.exe自己直接调用、不经过那条更复杂的链路，但"脚本文件
 * 而不是复合命令行"这个原则本身是通用的，不是只对那条特定链路成立，
 * 写成文件能完全避开cmd.exe/PowerShell两层转义规则互相打架的问题。
 * 返回1成功，-1失败。 */
static int write_probe_script( char *out_path, size_t out_path_size )
{
	char temp_dir[ MAX_PATH ];
	char temp_path[ MAX_PATH ];
	FILE *file;

	if( GetTempPathA( sizeof( temp_dir ), temp_dir ) == 0 )
	{
		return -1;
	}
	if( GetTempFileNameA( temp_dir, "blp", 0, temp_path ) == 0 )
	{
		return -1;
	}
	/* GetTempFileNameA已经创建了一个空文件占位——按.ps1重命名，否则
	 * PowerShell的-File参数会因为扩展名不对拒绝执行（这个项目在
	 * AGENTS.md第8节也记录过同一个限制） */
	{
		char ps1_path[ MAX_PATH ];
		snprintf( ps1_path, sizeof( ps1_path ), "%s.ps1", temp_path );
		DeleteFileA( temp_path );
		strncpy( temp_path, ps1_path, sizeof( temp_path ) - 1 );
		temp_path[ sizeof( temp_path ) - 1 ] = '\0';
	}

	file = fopen( temp_path, "wb" );
	if( file == NULL )
	{
		return -1;
	}
	fwrite( kProbeScript, 1, strlen( kProbeScript ), file );
	fclose( file );

	if( strlen( temp_path ) >= out_path_size )
	{
		DeleteFileA( temp_path );
		return -1;
	}
	strncpy( out_path, temp_path, out_path_size - 1 );
	out_path[ out_path_size - 1 ] = '\0';
	return 1;
}

int bitlocker_status_checker_query(
     char drive_letter,
     bitlocker_status_t *out )
{
	char script_path[ MAX_PATH ];
	char command_line[ MAX_PATH + 128 ];
	char output_line[ 256 ];
	FILE *pipe;
	int success = -1;

	if( out == NULL )
	{
		return -1;
	}
	memset( out, 0, sizeof( *out ) );

	if( write_probe_script( script_path, sizeof( script_path ) ) != 1 )
	{
		return -1;
	}

	snprintf( command_line, sizeof( command_line ),
	     "powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\" \"%c:\"",
	     script_path, drive_letter );

	pipe = _popen( command_line, "r" );
	if( pipe == NULL )
	{
		DeleteFileA( script_path );
		return -1;
	}

	output_line[ 0 ] = '\0';
	while( fgets( output_line, sizeof( output_line ), pipe ) != NULL )
	{
		/* 只关心第一行有意义的输出（去掉行尾换行即可），探测脚本本身
		 * 只会Write-Output一行结果 */
		size_t len = strlen( output_line );
		while( len > 0 && ( output_line[ len - 1 ] == '\n' || output_line[ len - 1 ] == '\r' ) )
		{
			output_line[ --len ] = '\0';
		}
		if( len > 0 )
		{
			break;
		}
	}
	_pclose( pipe );
	DeleteFileA( script_path );

	if( strcmp( output_line, "NOENTITY" ) == 0 )
	{
		out->is_fve_manageable = 0;
		out->is_locked = 0;
		out->is_protected = 0;
		out->encryption_percentage = -1;
		return 1;
	}

	{
		int lock_status = 0;
		int protection_status = 0;
		int encryption_percentage = 0;

		if( sscanf( output_line, "OK|%d|%d|%d", &lock_status, &protection_status, &encryption_percentage ) == 3 )
		{
			out->is_fve_manageable = 1;
			out->is_locked = ( lock_status != 0 ) ? 1 : 0;
			if( out->is_locked )
			{
				/* I0真机验证过：锁定态下这两个字段不可信，固定标成
				 * "未知"，不传递PowerShell那边查询失败时的垃圾值 */
				out->is_protected = -1;
				out->encryption_percentage = -1;
			}
			else
			{
				out->is_protected = ( protection_status == 1 ) ? 1 : 0;
				out->encryption_percentage = encryption_percentage;
			}
			success = 1;
		}
	}

	return success;
}
