/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  Misc system stuff needed by Doom, implemented for Linux.
 *  Mainly timer handling, and ENDOOM/ENDBOOM.
 *
 *-----------------------------------------------------------------------------
 */

#include <stdio.h>

#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <signal.h>
#ifdef _MSC_VER
#define    F_OK    0    /* Check for file existence */
#define    W_OK    2    /* Check for write permission */
#define    R_OK    4    /* Check for read permission */
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>



#include "config.h"
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "m_argv.h"
#include "lprintf.h"
#include "doomtype.h"
#include "doomdef.h"
#include "lprintf.h"
#include "m_fixed.h"
#include "r_fps.h"
#include "i_system.h"
#include "i_joy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_partition.h"
#include "spi_flash_mmap.h"

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"

#include <sys/time.h>

int realtime=0;


void I_uSleep(unsigned long usecs)
{
	if (usecs == 0) {
		taskYIELD();
		return;
	}

	TickType_t ticks = (TickType_t)(((uint64_t)usecs * configTICK_RATE_HZ + 999999) / 1000000);
	vTaskDelay(ticks);
}

static unsigned long getMsTicks() {
  struct timeval tv;
  struct timezone tz;

  gettimeofday(&tv, &tz);

  //convert to ms
  unsigned long now = tv.tv_usec/1000+tv.tv_sec*1000;
  return now;
}

int I_GetTime_RealTime (void)
{
  struct timeval tv;
  struct timezone tz;
  int64_t now_us;
  int64_t scaled_time;
  int64_t remaining_us;

  gettimeofday(&tv, &tz);

  now_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
  scaled_time = now_us * TICRATE;
  remaining_us = (1000000 - scaled_time % 1000000 + TICRATE - 1) / TICRATE;
  ms_to_next_tick = (int)((remaining_us + 999) / 1000);

  return (int)(scaled_time / 1000000);

}

const int displaytime=0;

fixed_t I_GetTimeFrac (void)
{
  unsigned long now;
  fixed_t frac;


  now = getMsTicks();

  if (tic_vars.step == 0)
    return FRACUNIT;
  else
  {
    frac = (fixed_t)((now - tic_vars.start + displaytime) * FRACUNIT / tic_vars.step);
    if (frac < 0)
      frac = 0;
    if (frac > FRACUNIT)
      frac = FRACUNIT;
    return frac;
  }
}


void I_GetTime_SaveMS(void)
{
  if (!movement_smooth)
    return;

  tic_vars.start = getMsTicks();
  tic_vars.next = (unsigned int) ((tic_vars.start * tic_vars.msec + 1.0f) / tic_vars.msec);
  tic_vars.step = tic_vars.next - tic_vars.start;
}

unsigned long I_GetRandomTimeSeed(void)
{
	return 4; //per https://xkcd.com/221/
}

const char* I_GetVersionString(char* buf, size_t sz)
{
  sprintf(buf,"%s v%s (http://prboom.sourceforge.net/)",PACKAGE,VERSION);
  return buf;
}

const char* I_SigString(char* buf, size_t sz, int signum)
{
  return buf;
}

typedef struct {
	const esp_partition_t* part;
	const void *mmap_base;
	spi_flash_mmap_handle_t mmap_handle;
	int offset;
	int size;
} FileDesc;

static FileDesc fds[32];

static int I_ValidFd(int fd)
{
	return fd >= 0 && fd < (int)(sizeof(fds) / sizeof(fds[0])) &&
	       fds[fd].part != NULL;
}

int I_Open(const char *wad, int flags) {
	int x=3;
	(void)flags;
	while (x < (int)(sizeof(fds) / sizeof(fds[0])) && fds[x].part!=NULL) x++;
	if (x == (int)(sizeof(fds) / sizeof(fds[0]))) {
		lprintf(LO_ERROR, "I_Open: file descriptor table is full\n");
		return -1;
	}
	if (strcmp(wad, "DOOM1.WAD")==0) {
		fds[x].part=esp_partition_find_first(66, 6, NULL);
		if (!fds[x].part) {
			lprintf(LO_ERROR, "I_Open: WAD partition not found\n");
			return -1;
		}
		fds[x].offset=0;
		fds[x].size=fds[x].part->size;
		esp_err_t err = esp_partition_mmap(fds[x].part, 0, fds[x].size,
		                                  SPI_FLASH_MMAP_DATA, &fds[x].mmap_base,
		                                  &fds[x].mmap_handle);
		if (err == ESP_OK) {
			lprintf(LO_INFO, "I_Open: mapped %d-byte WAD partition, %u data MMU pages free\n",
			        fds[x].size, (unsigned)spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA));
		} else {
			fds[x].mmap_base = NULL;
			lprintf(LO_WARN, "I_Open: whole-WAD mmap failed (%x), using lump cache\n", err);
		}
	} else {
		lprintf(LO_INFO, "I_Open: open %s failed\n", wad);
		return -1;
	}
	return x;
}

int I_Lseek(int ifd, off_t offset, int whence) {
	int64_t new_offset;

	if (!I_ValidFd(ifd)) {
		lprintf(LO_ERROR, "I_Lseek: invalid file descriptor %d\n", ifd);
		return -1;
	}

	if (whence==SEEK_SET) {
		new_offset=offset;
	} else if (whence==SEEK_CUR) {
		new_offset=(int64_t)fds[ifd].offset+offset;
	} else if (whence==SEEK_END) {
		new_offset=(int64_t)fds[ifd].size+offset;
	} else {
		lprintf(LO_ERROR, "I_Lseek: invalid whence %d\n", whence);
		return -1;
	}

	if (new_offset < 0 || new_offset > fds[ifd].size) {
		lprintf(LO_ERROR, "I_Lseek: offset %d is outside file (size %d)\n",
		        (int)new_offset, fds[ifd].size);
		return -1;
	}

	fds[ifd].offset=(int)new_offset;
	return fds[ifd].offset;
}

int I_Filelength(int ifd)
{
	if (!I_ValidFd(ifd)) {
		lprintf(LO_ERROR, "I_Filelength: invalid file descriptor %d\n", ifd);
		return -1;
	}
	return fds[ifd].size;
}

void I_Close(int fd) {
	if (!I_ValidFd(fd)) {
		lprintf(LO_WARN, "I_Close: invalid file descriptor %d\n", fd);
		return;
	}
	if (fds[fd].mmap_base) {
		spi_flash_munmap(fds[fd].mmap_handle);
	}
	memset(&fds[fd], 0, sizeof(fds[fd]));
}


typedef struct {
	spi_flash_mmap_handle_t handle;
	void *addr;
	const esp_partition_t *part;
	int offset;
	size_t len;
	int used;
} MmapHandle;

#define NO_MMAP_HANDLES 128
static MmapHandle mmapHandle[NO_MMAP_HANDLES];

static int nextHandle=0;
static int getFreeHandle() {
	int n=NO_MMAP_HANDLES;
	while (mmapHandle[nextHandle].used!=0 && n!=0) {
		nextHandle++;
		if (nextHandle==NO_MMAP_HANDLES) nextHandle=0;
		n--;
	}
	if (n==0) {
		lprintf(LO_ERROR, "I_Mmap: More mmaps than NO_MMAP_HANDLES!");
		return -1;
	}
	
	if (mmapHandle[nextHandle].addr) {
		spi_flash_munmap(mmapHandle[nextHandle].handle);
//		printf("mmap: freeing handle %d\n", nextHandle);
	}
	memset(&mmapHandle[nextHandle], 0, sizeof(mmapHandle[nextHandle]));
	int r=nextHandle;
	nextHandle++;
	if (nextHandle==NO_MMAP_HANDLES) nextHandle=0;

	return r;
}

static void freeUnusedMmaps() {
	for (int i=0; i<NO_MMAP_HANDLES; i++) {
		//Check if handle is not in use but is mapped.
		if (mmapHandle[i].used==0 && mmapHandle[i].addr!=NULL) {
			spi_flash_munmap(mmapHandle[i].handle);
			memset(&mmapHandle[i], 0, sizeof(mmapHandle[i]));
		}
	}
}

void *I_Mmap(void *addr, size_t length, int prot, int flags, int ifd, off_t offset) {
	int i;
	esp_err_t err;
	void *retaddr=NULL;

	if (!I_ValidFd(ifd) || offset < 0 || (size_t)offset > (size_t)fds[ifd].size ||
	    length > (size_t)fds[ifd].size - (size_t)offset) {
		lprintf(LO_ERROR, "I_Mmap: invalid fd/range fd=%d offset=%d len=%d\n",
		        ifd, (int)offset, (int)length);
		return NULL;
	}

	if (fds[ifd].mmap_base) {
		return (uint8_t *)fds[ifd].mmap_base + offset;
	}

	for (i=0; i<NO_MMAP_HANDLES; i++) {
		if (mmapHandle[i].addr != NULL &&
		    mmapHandle[i].part == fds[ifd].part &&
		    mmapHandle[i].offset == offset &&
		    mmapHandle[i].len == length) {
			mmapHandle[i].used++;
			return mmapHandle[i].addr;
		}
	}

	i=getFreeHandle();
	if (i < 0) {
		return NULL;
	}

	//lprintf(LO_INFO, "I_Mmap: mmaping offset %d size %d handle %d\n", (int)offset, (int)length, i);
	err=esp_partition_mmap(fds[ifd].part, offset, length, SPI_FLASH_MMAP_DATA, (const void**)&retaddr, &mmapHandle[i].handle);
	if (err==ESP_ERR_NO_MEM) {
		lprintf(LO_ERROR, "I_Mmap: No free address space. Cleaning up unused cached mmaps...\n");
		freeUnusedMmaps();
		err=esp_partition_mmap(fds[ifd].part, offset, length, SPI_FLASH_MMAP_DATA, (const void**)&retaddr, &mmapHandle[i].handle);
	}

	if (err!=ESP_OK) {
		lprintf(LO_ERROR, "I_Mmap: Can't mmap: %x (len=%d)!", err, length);
		memset(&mmapHandle[i], 0, sizeof(mmapHandle[i]));
		return NULL;
	}

	mmapHandle[i].addr=retaddr;
	mmapHandle[i].part=fds[ifd].part;
	mmapHandle[i].len=length;
	mmapHandle[i].used=1;
	mmapHandle[i].offset=offset;
	return retaddr;
}


int I_Munmap(void *addr, size_t length) {
	int i;
	uintptr_t address = (uintptr_t)addr;

	if (!addr) {
		lprintf(LO_ERROR, "I_Munmap: NULL address\n");
		return -1;
	}

	for (i=0; i<(int)(sizeof(fds) / sizeof(fds[0])); i++) {
		uintptr_t base = (uintptr_t)fds[i].mmap_base;
		if (base && address >= base &&
		    address - base <= (uintptr_t)fds[i].size &&
		    length <= (uintptr_t)fds[i].size - (address - base)) {
			return 0;
		}
	}

	for (i=0; i<NO_MMAP_HANDLES; i++) {
		if (mmapHandle[i].addr==addr && mmapHandle[i].len==length) break;
	}
	if (i==NO_MMAP_HANDLES) {
		lprintf(LO_ERROR, "I_Munmap: freeing non-mmapped address/len combo\n");
		return -1;
	}
	if (mmapHandle[i].used <= 0) {
		lprintf(LO_ERROR, "I_Munmap: handle %d is already unused\n", i);
		return -1;
	}
//	lprintf(LO_INFO, "I_Mmap: freeing handle %d\n", i);
	mmapHandle[i].used--;
	return 0;
}

void I_Read(int ifd, void* vbuf, size_t sz)
{
	uint8_t *d;

	if (!I_ValidFd(ifd) || (!vbuf && sz != 0)) {
		I_Error("I_Read: invalid file descriptor or buffer");
		return;
	}
	if (sz == 0) {
		return;
	}

	d=I_Mmap(NULL, sz, 0, 0, ifd, fds[ifd].offset);
	if (!d) {
		I_Error("I_Read: failed at offset %d for %u bytes",
		        fds[ifd].offset, (unsigned)sz);
		return;
	}
	memcpy(vbuf, d, sz);
	I_Munmap(d, sz);
	fds[ifd].offset+=(int)sz;
}

const char *I_DoomExeDir(void)
{
  return "";
}



char* I_FindFile(const char* wfname, const char* ext)
{
  (void)wfname;
  (void)ext;
  return NULL;
}

void I_SetAffinityMask(void)
{
}


#ifndef ESP_PLATFORM
int access(const char *path, int atype) {
    return 1;
}
#endif
