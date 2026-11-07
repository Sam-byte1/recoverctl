#include "volume_disk_extents.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

int volume_disk_extents_query(
     char drive_letter,
     volume_disk_extents_t *out )
{
	char path[ 16 ];
	HANDLE handle;
	VOLUME_DISK_EXTENTS extents;
	DWORD bytes_returned = 0;
	BOOL ok;

	if( out == NULL )
	{
		return -1;
	}
	memset( out, 0, sizeof( *out ) );
	memset( &extents, 0, sizeof( extents ) );

	snprintf( path, sizeof( path ), "\\\\.\\%c:", drive_letter );

	/* dwDesiredAccess传0——I0真机验证过，查询磁盘区间元数据不需要
	 * 请求任何读写权限，对一个被BitLocker锁定、没有任何权限能读出
	 * 内容的卷同样适用 */
	handle = CreateFileA( path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
	if( handle == INVALID_HANDLE_VALUE )
	{
		return -1;
	}

	ok = DeviceIoControl(
	     handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
	     NULL, 0, &extents, sizeof( extents ), &bytes_returned, NULL );
	CloseHandle( handle );

	if( !ok )
	{
		return -1;
	}

	if( extents.NumberOfDiskExtents != 1 )
	{
		out->is_single_extent = 0;
		return 1;
	}

	out->is_single_extent = 1;
	out->disk_number = (uint32_t) extents.Extents[ 0 ].DiskNumber;
	out->starting_offset = (uint64_t) extents.Extents[ 0 ].StartingOffset.QuadPart;
	out->extent_length = (uint64_t) extents.Extents[ 0 ].ExtentLength.QuadPart;
	return 1;
}
