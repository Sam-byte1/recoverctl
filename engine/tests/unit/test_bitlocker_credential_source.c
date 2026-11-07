/*
 * B5（BitLocker，2026-11-05 任务拆解排期）：凭据获取——文件渠道的覆盖。
 * 这不是纯函数TDD（这个模块本身是imperative shell，做真实文件I/O），
 * 但文件读取/格式解析这部分逻辑是完全确定性的，用真实临时文件验证
 * 比mock更直接、更不容易"测的是mock的行为而不是真实行为"，和
 * file_writer/vshadow_offline_test这类"真实I/O靠端到端验证"的项目
 * 惯例一致，只是这里不需要真实设备/VM，用临时文件就能完整覆盖。
 */
#include <stdio.h>
#include <string.h>

#include "test_framework.h"
#include "../../shell/bitlocker_credential_source.h"

#define TEST_FILE_PATH "/tmp/bitlocker_credential_source_test.tmp"

static void write_test_file( const char *content )
{
	FILE *f = fopen( TEST_FILE_PATH, "wb" );

	fwrite( content, 1, strlen( content ), f );
	fclose( f );
}

static void test_password_not_trimmed( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "密码文件：第一行原样读出，不trim前后空格" );

	write_test_file( "  my password with spaces  \nsecond line ignored\n" );

	TEST_ASSERT( bitlocker_credential_source_load_from_file(
	             BITLOCKER_CREDENTIAL_TYPE_PASSWORD, TEST_FILE_PATH, &cred ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_INT( (int) cred.text.length, (int) strlen( "  my password with spaces  " ), "长度应该匹配（含前后空格）" );
	TEST_ASSERT( memcmp( cred.text.data, "  my password with spaces  ", cred.text.length ) == 0,
	             "内容应该原样保留，包括前后空格" );
}

static void test_recovery_key_normalized( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "恢复密钥文件：读出来之后应该规范化成libbde要的`XXXXXX-XXXXXX-...`格式"
	           "（B9真机验证发现libbde要带短横线的8段格式，不是纯数字，见"
	           "engine/core/bitlocker_credential.h头注释）" );

	write_test_file( "123456-789012-345678-901234-567890-123456-789012\n" );

	TEST_ASSERT( bitlocker_credential_source_load_from_file(
	             BITLOCKER_CREDENTIAL_TYPE_RECOVERY_KEY, TEST_FILE_PATH, &cred ) == 1,
	             "应该成功" );
	TEST_ASSERT( memcmp( cred.text.data, "123456-789012-345678-901234-567890-123456-789012", cred.text.length ) == 0,
	             "短横线应该被重新规范到每6位数字一组的正确位置" );
}

static void test_startup_key_stores_path_only( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "启动密钥：应该只记录文件路径，不读取/解析文件内容本身" );

	write_test_file( "\x00\x01\x02\xff\xfe\xfd" );

	TEST_ASSERT( bitlocker_credential_source_load_from_file(
	             BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY, TEST_FILE_PATH, &cred ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_STR( cred.startup_key_file_path, TEST_FILE_PATH, "路径应该原样记录" );
}

static void test_fvek_with_tweak( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "FVEK文件：冒号分隔的FVEK:TWEAK格式应该都解码正确" );

	write_test_file( "deadbeef:cafebabe\n" );

	TEST_ASSERT( bitlocker_credential_source_load_from_file(
	             BITLOCKER_CREDENTIAL_TYPE_FVEK, TEST_FILE_PATH, &cred ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_INT( (int) cred.fvek_length, 4, "FVEK应该是4字节" );
	TEST_ASSERT_EQ_INT( (int) cred.tweak_length, 4, "TWEAK应该是4字节" );
	TEST_ASSERT( memcmp( cred.fvek, "\xde\xad\xbe\xef", 4 ) == 0, "FVEK内容应该正确" );
	TEST_ASSERT( memcmp( cred.tweak, "\xca\xfe\xba\xbe", 4 ) == 0, "TWEAK内容应该正确" );
}

static void test_fvek_without_tweak( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "FVEK文件：没有冒号时，TWEAK应该是0字节（对应AES-CBC场景）" );

	write_test_file( "deadbeef\n" );

	TEST_ASSERT( bitlocker_credential_source_load_from_file(
	             BITLOCKER_CREDENTIAL_TYPE_FVEK, TEST_FILE_PATH, &cred ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_INT( (int) cred.fvek_length, 4, "FVEK应该是4字节" );
	TEST_ASSERT_EQ_INT( (int) cred.tweak_length, 0, "TWEAK应该是0字节" );
}

static void test_fvek_invalid_hex_rejected( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "FVEK文件：非法十六进制内容应该被拒绝" );

	write_test_file( "not-valid-hex\n" );

	TEST_ASSERT( bitlocker_credential_source_load_from_file(
	             BITLOCKER_CREDENTIAL_TYPE_FVEK, TEST_FILE_PATH, &cred ) == -1,
	             "应该返回 -1" );
}

static void test_nonexistent_file_rejected( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "文件不存在时应该返回-1，而不是崩溃" );

	TEST_ASSERT( bitlocker_credential_source_load_from_file(
	             BITLOCKER_CREDENTIAL_TYPE_PASSWORD, "/nonexistent/path/does/not/exist", &cred ) == -1,
	             "应该返回 -1" );
}

static void test_null_arguments_rejected( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "空指针参数应该被拒绝，而不是崩溃" );

	TEST_ASSERT( bitlocker_credential_source_load_from_file(
	             BITLOCKER_CREDENTIAL_TYPE_PASSWORD, NULL, &cred ) == -1, "file_path为NULL应该返回-1" );
	TEST_ASSERT( bitlocker_credential_source_load_from_file(
	             BITLOCKER_CREDENTIAL_TYPE_PASSWORD, TEST_FILE_PATH, NULL ) == -1, "out为NULL应该返回-1" );
}

static void test_cli_password_not_trimmed( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "B10 命令行参数渠道：密码应该原样记录，不trim" );

	TEST_ASSERT( bitlocker_credential_source_load_from_cli_argument(
	             BITLOCKER_CREDENTIAL_TYPE_PASSWORD, "  cli password  ", &cred ) == 1,
	             "应该成功" );
	TEST_ASSERT( memcmp( cred.text.data, "  cli password  ", cred.text.length ) == 0,
	             "内容应该原样保留" );
}

static void test_cli_recovery_key_normalized( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "B10 命令行参数渠道：恢复密钥应该和文件渠道一样被规范化成带短横线的格式" );

	TEST_ASSERT( bitlocker_credential_source_load_from_cli_argument(
	             BITLOCKER_CREDENTIAL_TYPE_RECOVERY_KEY, "111111-222222-333333", &cred ) == 1,
	             "应该成功" );
	TEST_ASSERT( memcmp( cred.text.data, "111111-222222-333333", cred.text.length ) == 0,
	             "应该规范化成每6位数字一组、短横线分隔的格式" );
}

static void test_cli_fvek_with_tweak( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "B10 命令行参数渠道：FVEK:TWEAK格式应该和文件渠道一样被正确解析" );

	TEST_ASSERT( bitlocker_credential_source_load_from_cli_argument(
	             BITLOCKER_CREDENTIAL_TYPE_FVEK, "deadbeef:cafebabe", &cred ) == 1,
	             "应该成功" );
	TEST_ASSERT_EQ_INT( (int) cred.fvek_length, 4, "FVEK应该是4字节" );
	TEST_ASSERT_EQ_INT( (int) cred.tweak_length, 4, "TWEAK应该是4字节" );
}

static void test_cli_startup_key_rejected( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "B10 命令行参数渠道：启动密钥类型应该被拒绝（libbde API本身只接受文件路径）" );

	TEST_ASSERT( bitlocker_credential_source_load_from_cli_argument(
	             BITLOCKER_CREDENTIAL_TYPE_STARTUP_KEY, "/some/path.bek", &cred ) == -1,
	             "应该返回 -1" );
}

static void test_cli_null_arguments_rejected( void )
{
	bitlocker_resolved_credential_t cred;

	TEST_CASE( "B10 命令行参数渠道：空指针参数应该被拒绝，而不是崩溃" );

	TEST_ASSERT( bitlocker_credential_source_load_from_cli_argument(
	             BITLOCKER_CREDENTIAL_TYPE_PASSWORD, NULL, &cred ) == -1, "argument_value为NULL应该返回-1" );
	TEST_ASSERT( bitlocker_credential_source_load_from_cli_argument(
	             BITLOCKER_CREDENTIAL_TYPE_PASSWORD, "x", NULL ) == -1, "out为NULL应该返回-1" );
}

int main( void )
{
	test_password_not_trimmed();
	test_recovery_key_normalized();
	test_startup_key_stores_path_only();
	test_fvek_with_tweak();
	test_fvek_without_tweak();
	test_fvek_invalid_hex_rejected();
	test_nonexistent_file_rejected();
	test_null_arguments_rejected();
	test_cli_password_not_trimmed();
	test_cli_recovery_key_normalized();
	test_cli_fvek_with_tweak();
	test_cli_startup_key_rejected();
	test_cli_null_arguments_rejected();

	remove( TEST_FILE_PATH );

	TEST_SUMMARY();
}
