/*
 * D1（P4 PhotoRec兜底雕刻引擎，2026-11-05 第28步排期）：report.xml 解析器
 * 的 TDD 覆盖。用手写的 XML 字符串做 fixture（不像 FAT32/exFAT 那样需要
 * 构造二进制镜像，这份格式本身就是人类可读文本，直接写字符串常量即可）。
 */
#include <string.h>

#include "test_framework.h"
#include "../../core/photorec_report_parser.h"

static void test_single_fileobject( void )
{
	photorec_report_t report;
	const char *xml =
		"<?xml version='1.0' encoding='UTF-8'?>\n"
		"<dfxml xmloutputversion='1.0'>\n"
		"  <fileobject>\n"
		"    <filename>f0000100.jpg</filename>\n"
		"    <filesize>694</filesize>\n"
		"    <byte_runs>\n"
		"      <byte_run offset='0' img_offset='51200' len='32768'/>\n"
		"    </byte_runs>\n"
		"  </fileobject>\n"
		"</dfxml>\n";

	memset( &report, 0, sizeof( report ) );

	TEST_CASE( "单个 <fileobject> 应该正确解析出文件名/大小/原始偏移量" );

	TEST_ASSERT( photorec_report_parser_parse( xml, strlen( xml ), &report ) == 1,
	             "解析不应该报错" );
	TEST_ASSERT_EQ_INT( report.count, 1, "应该解析出 1 条记录" );

	if( report.count == 1 )
	{
		TEST_ASSERT_EQ_STR( report.entries[ 0 ].name, "f0000100.jpg", "文件名应该匹配" );
		TEST_ASSERT( report.entries[ 0 ].file_size == 694, "文件大小应该是 694" );
		TEST_ASSERT( report.entries[ 0 ].img_offset == 51200, "原始偏移量应该是 51200" );
	}

	photorec_report_free( &report );
}

static void test_multiple_fileobjects( void )
{
	photorec_report_t report;
	const char *xml =
		"<dfxml>\n"
		"  <fileobject>\n"
		"    <filename>f0000001.jpg</filename>\n"
		"    <filesize>1024</filesize>\n"
		"    <byte_runs><byte_run offset='0' img_offset='4096' len='4096'/></byte_runs>\n"
		"  </fileobject>\n"
		"  <fileobject>\n"
		"    <filename>f0000002.png</filename>\n"
		"    <filesize>2048</filesize>\n"
		"    <byte_runs><byte_run offset='0' img_offset='8192' len='8192'/></byte_runs>\n"
		"  </fileobject>\n"
		"  <fileobject>\n"
		"    <filename>f0000003.doc</filename>\n"
		"    <filesize>512</filesize>\n"
		"    <byte_runs><byte_run offset='0' img_offset='16384' len='4096'/></byte_runs>\n"
		"  </fileobject>\n"
		"</dfxml>\n";

	memset( &report, 0, sizeof( report ) );

	TEST_CASE( "多个 <fileobject> 应该按出现顺序全部解析出来" );

	TEST_ASSERT( photorec_report_parser_parse( xml, strlen( xml ), &report ) == 1,
	             "解析不应该报错" );
	TEST_ASSERT_EQ_INT( report.count, 3, "应该解析出 3 条记录" );

	if( report.count == 3 )
	{
		TEST_ASSERT_EQ_STR( report.entries[ 0 ].name, "f0000001.jpg", "第1条文件名应该匹配" );
		TEST_ASSERT_EQ_STR( report.entries[ 1 ].name, "f0000002.png", "第2条文件名应该匹配" );
		TEST_ASSERT_EQ_STR( report.entries[ 2 ].name, "f0000003.doc", "第3条文件名应该匹配" );
		TEST_ASSERT( report.entries[ 1 ].img_offset == 8192, "第2条偏移量应该匹配" );
	}

	photorec_report_free( &report );
}

static void test_zero_fileobjects_is_not_an_error( void )
{
	photorec_report_t report;
	/* 真实场景（第25步 vss.raw fixture）：镜像/store 本身没有任何可雕刻的
	 * 内容时，PhotoRec 照样正常生成 report.xml，只是一个 <fileobject>
	 * 都没有。这种"没找到"必须和"解析失败"区分开，返回值应该依然是 1。 */
	const char *xml =
		"<?xml version='1.0' encoding='UTF-8'?>\n"
		"<dfxml xmloutputversion='1.0'>\n"
		"  <metadata><dc:type>Carve Report</dc:type></metadata>\n"
		"  <source><image_filename>test.dd</image_filename></source>\n"
		"</dfxml>\n";

	memset( &report, 0, sizeof( report ) );

	TEST_CASE( "零个 <fileobject>（什么都没雕刻出来）不应该被当成解析失败" );

	TEST_ASSERT( photorec_report_parser_parse( xml, strlen( xml ), &report ) == 1,
	             "解析不应该报错" );
	TEST_ASSERT_EQ_INT( report.count, 0, "应该是 0 条记录，不是失败" );

	photorec_report_free( &report );
}

static void test_large_image_offset_beyond_4gb( void )
{
	photorec_report_t report;
	/* 真实磁盘镜像常见超过 4GB，img_offset 必须按 64 位解析，不能被
	 * 截断成 32 位 */
	const char *xml =
		"<dfxml>\n"
		"  <fileobject>\n"
		"    <filename>f0009999.mov</filename>\n"
		"    <filesize>10737418240</filesize>\n"
		"    <byte_runs><byte_run offset='0' img_offset='8589934592' len='1048576'/></byte_runs>\n"
		"  </fileobject>\n"
		"</dfxml>\n";

	memset( &report, 0, sizeof( report ) );

	TEST_CASE( "超过 4GB 的偏移量/文件大小应该按 64 位正确解析，不能截断" );

	TEST_ASSERT( photorec_report_parser_parse( xml, strlen( xml ), &report ) == 1,
	             "解析不应该报错" );

	if( report.count == 1 )
	{
		TEST_ASSERT( report.entries[ 0 ].img_offset == 8589934592ULL,
		             "偏移量应该是 8589934592（8GB），不能被截断成 32 位" );
		TEST_ASSERT( report.entries[ 0 ].file_size == 10737418240ULL,
		             "文件大小应该是 10737418240（10GB），不能被截断成 32 位" );
	}

	photorec_report_free( &report );
}

static void test_whitespace_and_indentation_tolerant( void )
{
	photorec_report_t report;
	/* 真实 PhotoRec 输出里标签之间有换行/缩进（已在第27步实测样例里确认），
	 * 解析器不能假设标签紧挨着写在同一行 */
	const char *xml =
		"<fileobject>\n"
		"\n"
		"    <filename>f0000042.jpg</filename>\n"
		"\n"
		"    <filesize>99</filesize>\n"
		"    <byte_runs>\n"
		"        <byte_run offset='0' img_offset='123' len='4096'/>\n"
		"    </byte_runs>\n"
		"</fileobject>\n";

	memset( &report, 0, sizeof( report ) );

	TEST_CASE( "标签之间的换行/缩进空白不应该影响解析" );

	TEST_ASSERT( photorec_report_parser_parse( xml, strlen( xml ), &report ) == 1,
	             "解析不应该报错" );
	TEST_ASSERT_EQ_INT( report.count, 1, "应该解析出 1 条记录" );

	if( report.count == 1 )
	{
		TEST_ASSERT_EQ_STR( report.entries[ 0 ].name, "f0000042.jpg", "文件名应该匹配" );
	}

	photorec_report_free( &report );
}

static void test_invalid_arguments_rejected( void )
{
	photorec_report_t report;

	memset( &report, 0, sizeof( report ) );

	TEST_CASE( "空指针参数应该被拒绝，而不是崩溃" );

	TEST_ASSERT( photorec_report_parser_parse( NULL, 0, &report ) == -1,
	             "xml_text 为 NULL 应该返回 -1" );
	TEST_ASSERT( photorec_report_parser_parse( "<dfxml></dfxml>", 16, NULL ) == -1,
	             "out 为 NULL 应该返回 -1" );
}

static void test_many_entries_triggers_growth( void )
{
	photorec_report_t report;
	char xml[ 16384 ];
	size_t offset = 0;
	int i;

	offset += (size_t) snprintf( xml + offset, sizeof( xml ) - offset, "<dfxml>\n" );
	for( i = 0; i < 20; i++ )
	{
		offset += (size_t) snprintf( xml + offset, sizeof( xml ) - offset,
		     "<fileobject><filename>f%07d.jpg</filename><filesize>%d</filesize>"
		     "<byte_runs><byte_run offset='0' img_offset='%d' len='512'/></byte_runs></fileobject>\n",
		     i, i * 10, i * 512 );
	}
	offset += (size_t) snprintf( xml + offset, sizeof( xml ) - offset, "</dfxml>\n" );

	memset( &report, 0, sizeof( report ) );

	TEST_CASE( "超过初始容量的条目数应该触发动态扩容，不丢数据" );

	TEST_ASSERT( photorec_report_parser_parse( xml, offset, &report ) == 1,
	             "解析不应该报错" );
	TEST_ASSERT_EQ_INT( report.count, 20, "应该解析出全部 20 条记录" );

	if( report.count == 20 )
	{
		TEST_ASSERT_EQ_STR( report.entries[ 19 ].name, "f0000019.jpg", "最后一条文件名应该匹配" );
		TEST_ASSERT( report.entries[ 19 ].img_offset == 19 * 512, "最后一条偏移量应该匹配" );
	}

	photorec_report_free( &report );
}

int main( void )
{
	test_single_fileobject();
	test_multiple_fileobjects();
	test_zero_fileobjects_is_not_an_error();
	test_large_image_offset_beyond_4gb();
	test_whitespace_and_indentation_tolerant();
	test_invalid_arguments_rejected();
	test_many_entries_triggers_growth();

	TEST_SUMMARY();
}
