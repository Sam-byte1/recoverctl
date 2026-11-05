/*
 * functional core：解析 PhotoRec `/cmd` 批处理模式跑完之后生成的
 * `report.xml`（DFXML "Carve Report" 格式），抽出每个被雕刻出的文件的
 * 文件名/大小/在源镜像里的原始物理偏移量，供调用方（shell 层）把这份
 * 清单接进本项目统一的结果呈现层。
 *
 * 背景（2026-11-05 第27→28步，P4 调研+小树裁决）：实测确认 `report.xml`
 * 是结构化、可直接解析的格式，不需要去解析人类可读的 `photorec.log`。
 * 真实样例（已在第27步实测验证）：
 *
 *   <fileobject>
 *     <filename>f0000100.jpg</filename>
 *     <filesize>694</filesize>
 *     <byte_runs>
 *       <byte_run offset='0' img_offset='51200' len='32768'/>
 *     </byte_runs>
 *   </fileobject>
 *
 * 这不是一个通用 XML 解析器——只认 PhotoRec 自己生成的这个固定、简单的
 * schema（没有命名空间前缀混淆、没有需要转义解码的实体），按项目一贯的
 * "不为用不到的通用性买单"原则，没有引入任何第三方 XML 库。每个
 * `<fileobject>` 只取第一个 `<byte_run>` 的 `img_offset`（实测到的真实
 * 输出里一个 fileobject 只有一个 byte_run；多 byte_run 的碎片化雕刻场景
 * 目前没有实测证据，等真的遇到了再扩展，不提前假设）。
 */
#ifndef ENGINE_CORE_PHOTOREC_REPORT_PARSER_H
#define ENGINE_CORE_PHOTOREC_REPORT_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	char name[ 256 ];
	uint64_t file_size;

	/* 这个文件在源镜像/设备里的原始物理字节偏移量，来自第一个
	 * <byte_run> 的 img_offset 属性 */
	uint64_t img_offset;
}
photorec_report_entry_t;

typedef struct
{
	photorec_report_entry_t *entries;
	int count;
	int capacity;
}
photorec_report_t;

/* 解析整份 report.xml 的文本内容，把每个 <fileobject> 对应的条目依次
 * 追加进 out（out 必须先清零初始化，函数内部会按需扩容）。
 *
 * 一份"零恢复"的 report.xml（比如镜像里什么都没雕刻出来，<fileobject>
 * 一个都没有）是合法输入，解析结果是 count == 0，返回 1（成功），不是
 * 错误——不能把"没找到文件"和"解析失败"混为一谈。
 *
 * 返回 1 成功，-1 失败（参数不合法、内存分配失败）。
 */
int photorec_report_parser_parse(
     const char *xml_text,
     size_t xml_text_size,
     photorec_report_t *out );

void photorec_report_free(
      photorec_report_t *report );

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_PHOTOREC_REPORT_PARSER_H */
