#include "test_framework.h"
#include "../../core/bad_sector_policy.h"

static void test_retries_while_attempts_remain( void )
{
	TEST_CASE( "max_retries=3 时，第 1~3 次失败都应该判定为继续重试" );

	TEST_ASSERT( bad_sector_policy_decide( 1, 3 ) == BAD_SECTOR_ACTION_RETRY, "第 1 次失败应该重试" );
	TEST_ASSERT( bad_sector_policy_decide( 2, 3 ) == BAD_SECTOR_ACTION_RETRY, "第 2 次失败应该重试" );
	TEST_ASSERT( bad_sector_policy_decide( 3, 3 ) == BAD_SECTOR_ACTION_RETRY, "第 3 次失败应该重试" );
}

static void test_skips_once_retries_exhausted( void )
{
	TEST_CASE( "max_retries=3 时，第 4 次失败应该判定为放弃（跳过）" );

	TEST_ASSERT( bad_sector_policy_decide( 4, 3 ) == BAD_SECTOR_ACTION_SKIP, "第 4 次失败应该跳过" );
	TEST_ASSERT( bad_sector_policy_decide( 100, 3 ) == BAD_SECTOR_ACTION_SKIP, "远超重试次数也应该跳过" );
}

static void test_zero_max_retries_skips_immediately( void )
{
	TEST_CASE( "max_retries=0 时，第 1 次失败就应该直接跳过，不重试" );

	TEST_ASSERT( bad_sector_policy_decide( 1, 0 ) == BAD_SECTOR_ACTION_SKIP,
	             "max_retries=0 表示不重试，第一次失败就该放弃" );
}

int main( void )
{
	test_retries_while_attempts_remain();
	test_skips_once_retries_exhausted();
	test_zero_max_retries_skips_immediately();

	TEST_SUMMARY();
}
