/*
 *  macos_util.cpp - MacOS definitions/utility functions
 *
 *  SheepShaver (C) 1997-2002 Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "xlowmem.h"
#include "emul_op.h"
#include "macos_util.h"

#define DEBUG 0
#include "debug.h"


// Function pointers
typedef long (*cu_ptr)(void *, uint32);
static uint32 cu_tvect = 0;
static inline long CallUniversal(void *arg1, uint32 arg2)
{
	return (long)CallMacOS2(cu_ptr, cu_tvect, arg1, arg2);
}
typedef int16 (*gsl_ptr)(char *, uint32, uint32, uint32 *, void **, char *);
static uint32 gsl_tvect = 0;
static inline int16 GetSharedLibrary(char *arg1, uint32 arg2, uint32 arg3, uint32 *arg4, void **arg5, char *arg6)
{
	return (int16)CallMacOS6(gsl_ptr, gsl_tvect, arg1, arg2, arg3, arg4, arg5, arg6);
}
typedef int16 (*fs_ptr)(uint32, char *, void **, uint32 *);
static uint32 fs_tvect = 0;
static inline int16 FindSymbol(uint32 arg1, char *arg2, void **arg3, uint32 *arg4)
{
	return (int16)CallMacOS4(fs_ptr, fs_tvect, arg1, arg2, arg3, arg4);
}
typedef int16 (*cc_ptr)(uint32 *);
static uint32 cc_tvect = 0;
static inline int16 CloseConnection(uint32 *arg1)
{
	return (int16)CallMacOS1(cc_ptr, cc_tvect, arg1);
}


/*
 *  Reset MacOS utilities
 */

void MacOSUtilReset(void)
{
	cu_tvect = 0;
	gsl_tvect = 0;
	fs_tvect = 0;
	cc_tvect = 0;
}


/*
 *  Enqueue QElem to list
 */

void Enqueue(uint32 elem, uint32 list)
{
	WriteMacInt32(elem + qLink, 0);
	if (!ReadMacInt32(list + qTail)) {
		WriteMacInt32(list + qHead, elem);
		WriteMacInt32(list + qTail, elem);
	} else {
		WriteMacInt32(ReadMacInt32(list + qTail) + qLink, elem);
		WriteMacInt32(list + qTail, elem);
	}
}


/*
 *  Find first free drive number, starting at num
 */

static bool is_drive_number_free(int num)
{
	uint32 e = ReadMacInt32(0x308 + qHead);
	while (e) {
		uint32 d = e - dsQLink;
		if ((int)ReadMacInt16(d + dsQDrive) == num)
			return false;
		e = ReadMacInt32(e + qLink);
	}
	return true;
}

int FindFreeDriveNumber(int num)
{
	while (!is_drive_number_free(num))
		num++;
	return num;
}


/*
 *  Mount volume with given file handle (call this function when you are unable to
 *  do automatic media change detection and the user has to press a special key
 *  or something to mount a volume; this function will check if there's really a
 *  volume in the drive with SysIsDiskInserted(); volumes which are present on startup
 *  are automatically mounted)
 */

void MountVolume(void *fh)
{
	SonyMountVolume(fh) || DiskMountVolume(fh) || CDROMMountVolume(fh);
}


/*
 *  Calculate disk image file layout given file size and first 256 data bytes
 */

void FileDiskLayout(loff_t size, uint8 *data, loff_t &start_byte, loff_t &real_size)
{
	if (size == 419284 || size == 838484) {
		// 400K/800K DiskCopy image, 84 byte header
		start_byte = 84;
		real_size = (size - 84) & ~0x1ff;
	} else {
		// 0..511 byte header
		start_byte = size & 0x1ff;
		real_size = size - start_byte;
	}
}


/*
 *  Find symbol in shared library (using CFM)
 *  lib and sym must be Pascal strings!
 */

void *FindLibSymbol(char *lib, char *sym)
{
	uint32 conn_id = 0;
	void *main_addr = NULL;
	char err[256] = "";
	uint32 *sym_addr = NULL;
	uint32 sym_class = 0;

	D(bug("FindLibSymbol %s in %s...\n", sym+1, lib+1));

	if (*(uint32 *)XLM_RUN_MODE == MODE_EMUL_OP) {
		M68kRegisters r;
	
		// Find shared library
		static const uint16 proc1[] = {
			0x558f,					// subq.l	#2,a7
			0x2f08,					// move.l	a0,-(a7)
			0x2f3c, 0x7077, 0x7063,	// move.l	#'pwpc',-(a7)
			0x2f3c, 0, 1,			// move.l	#kReferenceCFrag,-(a7)
			0x2f09,					// move.l	a1,-(a7)
			0x2f0a,					// move.l	a2,-(a7)
			0x2f0b,					// move.l	a3,-(a7)
			0x3f3c, 1,				// (GetSharedLibrary)
			0xaa5a,					// CFMDispatch
			0x301f,					// move.w	(a7)+,d0
			M68K_RTS
		};
		r.a[0] = (uint32)lib;
		r.a[1] = (uint32)&conn_id;
		r.a[2] = (uint32)&main_addr;
		r.a[3] = (uint32)err;
		Execute68k((uint32)proc1, &r);
		D(bug(" GetSharedLibrary: ret %d, connection ID %ld, main %p\n", (int16)r.d[0], conn_id, main_addr));
		if (r.d[0])
			return NULL;
	
		// Find symbol
		static const uint16 proc2[] = {
			0x558f,			// subq.l	#2,a7
			0x2f00,			// move.l	d0,-(a7)
			0x2f08,			// move.l	a0,-(a7)
			0x2f09,			// move.l	a1,-(a7)
			0x2f0a,			// move.l	a2,-(a7)
			0x3f3c, 5,		// (FindSymbol)
			0xaa5a,			// CFMDispatch
			0x301f,			// move.w	(a7)+,d0
			M68K_RTS
		};
		r.d[0] = conn_id;
		r.a[0] = (uint32)sym;
		r.a[1] = (uint32)&sym_addr;
		r.a[2] = (uint32)&sym_class;
		Execute68k((uint32)proc2, &r);
		D(bug(" FindSymbol: ret %d, sym_addr %p, sym_class %ld\n", (int16)r.d[0], sym_addr, sym_class));
//!! CloseConnection()?
		if (r.d[0])
			return NULL;
		else
			return sym_addr;

	} else {

		if (GetSharedLibrary == NULL || FindSymbol == NULL) {
			printf("FATAL: FindLibSymbol() called too early\n");
			return 0;
		}
		int16 res;
		res = GetSharedLibrary(lib, FOURCC('p','w','p','c'), 1, &conn_id, &main_addr, err);
		D(bug(" GetSharedLibrary: ret %d, connection ID %ld, main %p\n", res, conn_id, main_addr));
		if (res)
			return NULL;
		res = FindSymbol(conn_id, sym, (void **)&sym_addr, &sym_class);
		D(bug(" FindSymbol: ret %d, sym_addr %p, sym_class %ld\n", res, sym_addr, sym_class));
//!!??		CloseConnection(&conn_id);
		if (res)
			return NULL;
		else
			return sym_addr;
	}
}


/*
 *  Find CallUniversalProc() TVector
 */

void InitCallUniversalProc()
{
	cu_tvect = (uint32)FindLibSymbol("\014InterfaceLib", "\021CallUniversalProc");
	D(bug("CallUniversalProc TVECT at %08lx\n", cu_tvect));
	if (cu_tvect == 0) {
		printf("FATAL: Can't find CallUniversalProc()\n");
		QuitEmulator();
	}

	gsl_tvect = (uint32)FindLibSymbol("\014InterfaceLib", "\020GetSharedLibrary");
	D(bug("GetSharedLibrary TVECT at %08lx\n", gsl_tvect));
	if (gsl_tvect == 0) {
		printf("FATAL: Can't find GetSharedLibrary()\n");
		QuitEmulator();
	}

	fs_tvect = (uint32)FindLibSymbol("\014InterfaceLib", "\012FindSymbol");
	D(bug("FindSymbol TVECT at %08lx\n", fs_tvect));
	if (fs_tvect == 0) {
		printf("FATAL: Can't find FindSymbol()\n");
		QuitEmulator();
	}

	cc_tvect = (uint32)FindLibSymbol("\014InterfaceLib", "\017CloseConnection");
	D(bug("CloseConnection TVECT at %08lx\n", cc_tvect));
	if (cc_tvect == 0) {
		printf("FATAL: Can't find CloseConnection()\n");
		QuitEmulator();
	}
}


/*
 *  CallUniversalProc
 */

long CallUniversalProc(void *upp, uint32 info)
{
	if (cu_tvect == 0) {
		printf("FATAL: CallUniversalProc() called too early\n");
		return 0;
	}
	return CallUniversal(upp, info);
}


/*
 *  Convert time_t value to MacOS time (seconds since 1.1.1904)
 */

uint32 TimeToMacTime(time_t t)
{
	// This code is taken from glibc 2.2

	// Convert to number of seconds elapsed since 1-Jan-1904
	struct tm *local = localtime(&t);
	const int TM_EPOCH_YEAR = 1900;
	const int MAC_EPOCH_YEAR = 1904;
	int a4 = ((local->tm_year + TM_EPOCH_YEAR) >> 2) - !(local->tm_year & 3);
	int b4 = (MAC_EPOCH_YEAR >> 2) - !(MAC_EPOCH_YEAR & 3);
	int a100 = a4 / 25 - (a4 % 25 < 0);
	int b100 = b4 / 25 - (b4 % 25 < 0);
	int a400 = a100 >> 2;
	int b400 = b100 >> 2;
	int intervening_leap_days = (a4 - b4) - (a100 - b100) + (a400 - b400);
	uint32 days = local->tm_yday + 365 * (local->tm_year - 4) + intervening_leap_days;
	return local->tm_sec + 60 * (local->tm_min + 60 * (local->tm_hour + 24 * days));
}
