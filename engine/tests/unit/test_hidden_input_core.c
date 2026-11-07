/*
 * B11a（BitLocker交互式隐藏输入渠道，2026-11-05任务拆解排期）：
 * 核心状态机的TDD覆盖。见 engine/core/hidden_input_core.h 头注释——
 * 这里只测试"输入该怎么被解释"，不碰任何真实终端。
 */
#include <string.h>

#include "test_framework.h"
#include "../../core/hidden_input_core.h"

static void test_printable_chars_accumulate_in_order( void )
{
	hidden_input_state_t state;

	TEST_CASE( "连续输入可打印字符应该按顺序追加进buffer，每次都返回CONTINUE" );

	hidden_input_core_init( &state );

	TEST_ASSERT( hidden_input_core_feed( &state, 'a' ) == HIDDEN_INPUT_ACTION_CONTINUE, "'a'应该是CONTINUE" );
	TEST_ASSERT( hidden_input_core_feed( &state, 'b' ) == HIDDEN_INPUT_ACTION_CONTINUE, "'b'应该是CONTINUE" );
	TEST_ASSERT( hidden_input_core_feed( &state, 'c' ) == HIDDEN_INPUT_ACTION_CONTINUE, "'c'应该是CONTINUE" );

	TEST_ASSERT_EQ_INT( (int) state.length, 3, "length应该是3" );
	TEST_ASSERT( memcmp( state.buffer, "abc", 3 ) == 0, "buffer内容应该是\"abc\"" );
}

static void test_enter_completes_input( void )
{
	hidden_input_state_t state;

	TEST_CASE( "回车（'\\r'或'\\n'）应该返回COMPLETE，不应该把回车本身写进buffer" );

	hidden_input_core_init( &state );
	hidden_input_core_feed( &state, 'x' );

	TEST_ASSERT( hidden_input_core_feed( &state, '\r' ) == HIDDEN_INPUT_ACTION_COMPLETE, "'\\r'应该是COMPLETE" );
	TEST_ASSERT_EQ_INT( (int) state.length, 1, "length不应该因为回车而增加" );

	hidden_input_core_init( &state );
	hidden_input_core_feed( &state, 'y' );

	TEST_ASSERT( hidden_input_core_feed( &state, '\n' ) == HIDDEN_INPUT_ACTION_COMPLETE, "'\\n'也应该是COMPLETE（兼容POSIX终端习惯）" );
}

static void test_ctrl_c_cancels_input( void )
{
	hidden_input_state_t state;

	TEST_CASE( "Ctrl+C（0x03）应该返回CANCELLED" );

	hidden_input_core_init( &state );
	hidden_input_core_feed( &state, 'z' );

	TEST_ASSERT( hidden_input_core_feed( &state, 0x03 ) == HIDDEN_INPUT_ACTION_CANCELLED, "0x03应该是CANCELLED" );
}

static void test_backspace_removes_last_char( void )
{
	hidden_input_state_t state;

	TEST_CASE( "退格应该删掉最后一个字符，返回CONTINUE" );

	hidden_input_core_init( &state );
	hidden_input_core_feed( &state, 'a' );
	hidden_input_core_feed( &state, 'b' );

	TEST_ASSERT( hidden_input_core_feed( &state, '\b' ) == HIDDEN_INPUT_ACTION_CONTINUE, "退格应该是CONTINUE" );
	TEST_ASSERT_EQ_INT( (int) state.length, 1, "length应该减少到1" );
	TEST_ASSERT( memcmp( state.buffer, "a", 1 ) == 0, "剩下的内容应该是\"a\"" );

	TEST_ASSERT( hidden_input_core_feed( &state, 0x7F ) == HIDDEN_INPUT_ACTION_CONTINUE, "0x7F也应该是退格（POSIX终端常见编码），应该是CONTINUE" );
	TEST_ASSERT_EQ_INT( (int) state.length, 0, "length应该减少到0" );
}

static void test_backspace_on_empty_buffer_is_harmless( void )
{
	hidden_input_state_t state;

	TEST_CASE( "空缓冲区再按退格应该是安全的无副作用操作，不应该下溢" );

	hidden_input_core_init( &state );

	TEST_ASSERT( hidden_input_core_feed( &state, '\b' ) == HIDDEN_INPUT_ACTION_CONTINUE, "应该是CONTINUE" );
	TEST_ASSERT_EQ_INT( (int) state.length, 0, "length应该保持0，不应该下溢成一个很大的数" );
}

static void test_buffer_full_rejects_further_printable_chars( void )
{
	hidden_input_state_t state;
	size_t i;

	TEST_CASE( "缓冲区装满之后，再输入可打印字符应该返回REJECTED，不应该溢出写" );

	hidden_input_core_init( &state );
	for( i = 0; i < HIDDEN_INPUT_MAX_LENGTH; i++ )
	{
		TEST_ASSERT( hidden_input_core_feed( &state, 'x' ) == HIDDEN_INPUT_ACTION_CONTINUE,
		             "装满之前每次都应该是CONTINUE" );
	}
	TEST_ASSERT_EQ_INT( (int) state.length, HIDDEN_INPUT_MAX_LENGTH, "length应该正好等于上限" );

	TEST_ASSERT( hidden_input_core_feed( &state, 'y' ) == HIDDEN_INPUT_ACTION_REJECTED,
	             "满了之后应该是REJECTED" );
	TEST_ASSERT_EQ_INT( (int) state.length, HIDDEN_INPUT_MAX_LENGTH, "length不应该因为被拒绝的字符而改变" );

	TEST_ASSERT( hidden_input_core_feed( &state, '\b' ) == HIDDEN_INPUT_ACTION_CONTINUE,
	             "满了之后退格仍然应该正常工作" );
	TEST_ASSERT_EQ_INT( (int) state.length, HIDDEN_INPUT_MAX_LENGTH - 1, "退格应该让length减1" );
}

static void test_unrecognized_control_chars_rejected( void )
{
	hidden_input_state_t state;

	TEST_CASE( "既不是可打印字符、也不是退格/回车/Ctrl+C的其它控制字符应该被拒绝，不改变状态" );

	hidden_input_core_init( &state );
	hidden_input_core_feed( &state, 'a' );

	TEST_ASSERT( hidden_input_core_feed( &state, 0x01 ) == HIDDEN_INPUT_ACTION_REJECTED, "0x01应该是REJECTED" );
	TEST_ASSERT_EQ_INT( (int) state.length, 1, "length不应该改变" );

	TEST_ASSERT( hidden_input_core_feed( &state, 0x1B ) == HIDDEN_INPUT_ACTION_REJECTED, "ESC(0x1B)应该是REJECTED（这个状态机不处理方向键这类ESC序列）" );
}

static void test_null_state_rejected( void )
{
	TEST_CASE( "state为NULL应该被拒绝，而不是崩溃" );

	TEST_ASSERT( hidden_input_core_feed( NULL, 'a' ) == HIDDEN_INPUT_ACTION_REJECTED, "应该返回REJECTED" );

	/* init对NULL应该是安全的no-op，不崩溃就算通过，没有可断言的返回值 */
	hidden_input_core_init( NULL );
}

int main( void )
{
	test_printable_chars_accumulate_in_order();
	test_enter_completes_input();
	test_ctrl_c_cancels_input();
	test_backspace_removes_last_char();
	test_backspace_on_empty_buffer_is_harmless();
	test_buffer_full_rejects_further_printable_chars();
	test_unrecognized_control_chars_rejected();
	test_null_state_rejected();

	TEST_SUMMARY();
}
