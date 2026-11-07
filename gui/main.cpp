/*
 * G3（P6 GUI壳层，2026-11-06小树裁决）：扫描/恢复主干流程的UI。
 * G1遗留的"最小技术验证demo"内容已经被真正的功能窗口（MainWindow）
 * 取代——G1当时验证过的"Qt能直接链接engine/shell现有C代码"这件事
 * 已经成立，不需要再保留那份极简demo。
 */
#include <QApplication>

#include "app_style.h"
#include "candidate_item.h"
#include "main_window.h"
#include "scan_source.h"

int main( int argc, char *argv[] )
{
	QApplication app( argc, argv );

	/* G9（UI视觉重做，2026-11-06）：全局字体+QSS样式表，见app_style.h
	 * 头注释——设计方案来自向Codex咨询的具体建议，不是随手调色 */
	applyAppFont( app );
	app.setStyleSheet( appStyleSheet() );

	/* 跨线程排队连接需要运行时注册这两个类型（Q_DECLARE_METATYPE只是
	 * 编译期让QMetaType认识它们，供QVariant使用；qRegisterMetaType是
	 * 运行时注册，signal/slot的排队连接机制要靠它才知道怎么把参数
	 * 拷贝进事件队列）——ScanWorker的progress/scanFinished/recoverFinished
	 * 信号跨线程发射，不注册会在运行时报"无法排队调用"的警告。 */
	qRegisterMetaType<CandidateItem>( "CandidateItem" );
	qRegisterMetaType<QVector<CandidateItem>>( "QVector<CandidateItem>" );
	/* G6新增：scan/recover/generateThumbnail的ScanSource参数同理需要
	 * 注册，否则跨线程排队调用会报同样的警告。 */
	qRegisterMetaType<ScanSource>( "ScanSource" );
	/* G7新增：requestScanBitLocker信号的一次性凭据参数，同样需要注册 */
	qRegisterMetaType<BitLockerCredentialInput>( "BitLockerCredentialInput" );

	MainWindow window;
	window.show();

	return app.exec();
}
