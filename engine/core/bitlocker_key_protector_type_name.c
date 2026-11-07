#include "bitlocker_key_protector_type_name.h"

#include <stdio.h>

#define LIBBDE_KEY_PROTECTION_TYPE_CLEAR_KEY          0x0000
#define LIBBDE_KEY_PROTECTION_TYPE_TPM                0x0100
#define LIBBDE_KEY_PROTECTION_TYPE_STARTUP_KEY        0x0200
#define LIBBDE_KEY_PROTECTION_TYPE_TPM_AND_PIN        0x0500
#define LIBBDE_KEY_PROTECTION_TYPE_RECOVERY_PASSWORD  0x0800
#define LIBBDE_KEY_PROTECTION_TYPE_PASSWORD           0x2000

const char *bitlocker_key_protector_type_name(
             uint16_t type )
{
	static char unknown_buffer[ 64 ];

	switch( type )
	{
		case LIBBDE_KEY_PROTECTION_TYPE_CLEAR_KEY:
			return "明文密钥（无保护）";

		case LIBBDE_KEY_PROTECTION_TYPE_TPM:
			return "TPM（本工具不支持解锁，libbde本身的能力边界）";

		case LIBBDE_KEY_PROTECTION_TYPE_STARTUP_KEY:
			return "启动密钥（.BEK文件）";

		case LIBBDE_KEY_PROTECTION_TYPE_TPM_AND_PIN:
			return "TPM+PIN（本工具不支持解锁，libbde本身的能力边界）";

		case LIBBDE_KEY_PROTECTION_TYPE_RECOVERY_PASSWORD:
			return "恢复密钥（48位数字）";

		case LIBBDE_KEY_PROTECTION_TYPE_PASSWORD:
			return "密码";

		default:
			snprintf( unknown_buffer, sizeof( unknown_buffer ), "未知类型(0x%04x)", type );
			return unknown_buffer;
	}
}
