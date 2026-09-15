/*
    SdGeckoDma.c -- SD over EXI (SD Gecko / SD2SP2) with DMA data transfers.

    ALTERED SOURCE. Derived from libogc v3.0.4 libogc/sdgecko_io.c, with a
    DISC_INTERFACE wrapper modelled on libogc's gcsd.c (Sven Peter). Changes made
    for Octave:
      - Everything is static and renamed, so it links next to libogc's own driver.
      - Once a card is initialized, data blocks on EXI channels 1 and 2 (memory
        card slot B, serial port 2) are received with EXI DMA instead of 4-byte
        PIO, and the reading thread sleeps until the transfer-complete interrupt.
        As in libogc2 (commit 232a13d8), this needs a semi-passive adapter: receives
        are done with the EXI chip select released (EXI_SelectSD), and the adapter
        is detected by resetting the card that way first.
      - The bus clock goes from EXI_SPEED16MHZ to EXI_SPEED32MHZ after init.
      - If a sector read fails, the drive steps down one transfer setting
        (DMA 27MHz, DMA 13.5MHz, PIO 27MHz, PIO 13.5MHz = stock), re-initializes
        the card and retries. Failures are queued for the engine's diagnostic log.
      - Sector writes are unchanged (PIO).

    Original libogc license:

  libogc copyright (C) 2004 - 2025
    Michael Wiedenbauer (shagkur)
    Dave Murphy (WinterMute)

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any
  damages arising from the use of this software.

  Permission is granted to anyone to use this software for any
  purpose, including commercial applications, and to alter it and
  redistribute it freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you
     must not claim that you wrote the original software. If you use
     this software in a product, an acknowledgment in the product
     documentation would be appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and
     must not be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source
     distribution.
*/

#if PLATFORM_GAMECUBE

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/exi.h>
#include <ogc/lwp.h>
#include <ogc/cache.h>
#include <ogc/disc_io.h>
#include <sdcard/card_cmn.h>
#include <sdcard/gcsd.h>
#include <gcutil.h>
#include <fat.h>

#define TYPE_SD								0
#define TYPE_SDHC							1

#define PAGE_SIZE512						512

#define MMC_ERROR_PARAM						0x0040
#define MMC_ERROR_ADDRESS					0x0020
#define MMC_ERROR_ERASE_SEQ					0x0010
#define MMC_ERROR_CRC						0x0008
#define MMC_ERROR_ILL						0x0004
#define MMC_ERROR_ERASE_RES					0x0002
#define MMC_ERROR_IDLE						0x0001

#define CARDIO_OP_INITFAILED				0x8000
#define CARDIO_OP_TIMEDOUT					0x4000
#define CARDIO_OP_IOERR_IDLE				0x2000
#define CARDIO_OP_IOERR_PARAM				0x1000
#define CARDIO_OP_IOERR_WRITE				0x0200
#define CARDIO_OP_IOERR_ADDR				0x0100
#define CARDIO_OP_IOERR_CRC					0x0002
#define CARDIO_OP_IOERR_ILL					0x0001
#define CARDIO_OP_IOERR_FATAL				(CARDIO_OP_IOERR_PARAM|CARDIO_OP_IOERR_WRITE|CARDIO_OP_IOERR_ADDR|CARDIO_OP_IOERR_CRC|CARDIO_OP_IOERR_ILL)

#define CARD_IO_SECTOR_ADDRESSING			0
#define CARD_IO_BYTE_ADDRESSING				1

#define WRITE_BL_LEN(drv_no)				((u8)((_ioCSD[drv_no][12]&0x03)<<2)|((_ioCSD[drv_no][13]>>6)&0x03))

#define SAFE_FREQ							EXI_SPEED16MHZ
#define FAST_FREQ							EXI_SPEED32MHZ

typedef s32 (*cardiocallback)(s32 drv_no);

static u8 _ioCID[MAX_DRIVE][16];
static u8 _ioCSD[MAX_DRIVE][16];
static u8 _ioCardStatus[MAX_DRIVE][64];

static u8 _ioWPFlag;
static u8 _ioClrFlag;

static u32 _ioCardFreq[MAX_DRIVE];
static u32 _ioRetryCnt;
static cardiocallback _ioRetryCB = NULL;

static lwpq_t _ioEXILock[MAX_DRIVE];
static lwpq_t _ioDmaQueue[MAX_DRIVE];

static u32 _ioPageSize[MAX_DRIVE];
static u32 _ioFlag[MAX_DRIVE];
static u32 _ioError[MAX_DRIVE];
static bool _ioCardInserted[MAX_DRIVE];

// Transfer settings tried in order once a card is initialized. A failed sector read
// moves the drive one step down (re-initializing the card) and is retried, so each
// setting is tested on its own. DMA steps are skipped on channel 0 (PIO there, as in
// libogc2) and for inverted-bus (_ioWPFlag) adapters, which need the idle byte driven.
typedef struct { u32 freq; bool dma; const char *name; } sdlevel;
static const sdlevel _ioLevels[] = {
	{ FAST_FREQ, true,  "dma27" },
	{ SAFE_FREQ, true,  "dma13.5" },
	{ FAST_FREQ, false, "pio27" },
	{ SAFE_FREQ, false, "pio13.5" },
};
#define LEVEL_COUNT (sizeof(_ioLevels)/sizeof(_ioLevels[0]))

static bool _ioUseDma[MAX_DRIVE];
static bool _ioSemiPassive[MAX_DRIVE];
static u32 _ioLevel[MAX_DRIVE];
static const char *_ioFailStage[MAX_DRIVE];
static u32 _ioFailBlock[MAX_DRIVE];

static void __card_applyLevel(s32 drv_no)
{
	bool dmaAllowed = (drv_no!=0 && !_ioWPFlag && _ioSemiPassive[drv_no]);

	while(_ioLevel[drv_no]<LEVEL_COUNT-1 && _ioLevels[_ioLevel[drv_no]].dma && !dmaAllowed)
		_ioLevel[drv_no]++;
	_ioCardFreq[drv_no] = _ioLevels[_ioLevel[drv_no]].freq;
	_ioUseDma[drv_no] = _ioLevels[_ioLevel[drv_no]].dma;
}

// Select for exchanges that receive from the card. With a semi-passive adapter the
// EXI chip select is left released (EXI_SelectSD), which DMA reads need; as in
// libogc2 commit 232a13d8 ("Initial support for semi-passive SD card adapters").
static s32 __card_selectrx(s32 drv_no)
{
	if(_ioSemiPassive[drv_no])
		return EXI_SelectSD(drv_no,EXI_DEVICE_0,_ioCardFreq[drv_no]);
	return EXI_Select(drv_no,EXI_DEVICE_0,_ioCardFreq[drv_no]);
}

// Events for the diagnostic log. The driver runs inside libfat calls (and the engine's
// ISO read lock), so it can't write the log itself; the engine drains these instead.
#define EVENT_COUNT							8
#define EVENT_SIZE							128

static char _ioEvents[EVENT_COUNT][EVENT_SIZE];
static u32 _ioEventHead,_ioEventCount;

static void __sd_event(const char *format,...)
{
	char text[EVENT_SIZE];
	va_list args;
	u32 level;

	va_start(args,format);
	vsnprintf(text,sizeof(text),format,args);
	va_end(args);

	_CPU_ISR_Disable(level);
	if(_ioEventCount==EVENT_COUNT) {
		_ioEventHead = (_ioEventHead+1)%EVENT_COUNT;
		_ioEventCount--;
	}
	memcpy(_ioEvents[(_ioEventHead+_ioEventCount)%EVENT_COUNT],text,EVENT_SIZE);
	_ioEventCount++;
	_CPU_ISR_Restore(level);
}

static u8 _ioResponse[MAX_DRIVE][128];
static u8 _ioCrc7Table[256];
static u16 _ioCrc16Table[256];

static u32 _initType[MAX_DRIVE];
static u32 _ioAddressingType[MAX_DRIVE];

static s32 __card_initIO(s32 drv_no);
static s32 __card_doUnmount(s32 drv_no);

static __inline__ u32 __check_response(s32 drv_no,u8 res)
{
	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	_ioError[drv_no] = 0;
	if(_ioFlag[drv_no]==INITIALIZING && res&MMC_ERROR_IDLE) {
		_ioError[drv_no] |= CARDIO_OP_IOERR_IDLE;
		return CARDIO_ERROR_READY;
	} else {
		if(res&MMC_ERROR_PARAM) _ioError[drv_no] |= CARDIO_OP_IOERR_PARAM;
		if(res&MMC_ERROR_ADDRESS) _ioError[drv_no] |= CARDIO_OP_IOERR_ADDR;
		if(res&MMC_ERROR_CRC) _ioError[drv_no] |= CARDIO_OP_IOERR_CRC;
		if(res&MMC_ERROR_ILL) _ioError[drv_no] |= CARDIO_OP_IOERR_ILL;
	}
	return ((_ioError[drv_no]&CARDIO_OP_IOERR_FATAL)?CARDIO_ERROR_INTERNAL:CARDIO_ERROR_READY);
}

static void __init_crc7(void)
{
	s32 i,j;
	u8 c,crc7;

	crc7 = 0;
	for(i=0;i<256;i++) {
		c = i;
		crc7 = 0;
		for(j=0;j<8;j++) {
			crc7 <<= 1;
			if((crc7^c)&0x80) crc7 ^= 0x09;
			c <<= 1;
		}
		crc7 &= 0x7f;
		_ioCrc7Table[i] = crc7;
	}
}

static u8 __make_crc7(void *buffer,u32 len)
{
	s32 i;
	u8 crc7;
	u8 *ptr;

	crc7 = 0;
	ptr = buffer;
	for(i=0;i<len;i++)
		crc7 = _ioCrc7Table[(u8)((crc7<<1)^ptr[i])];

	return ((crc7<<1)|1);
}

static void __init_crc16(void)
{
	s32 i,j;
	u16 crc16,c;

	for(i=0;i<256;i++) {
		crc16 = 0;
		c = ((u16)i)<<8;
		for(j=0;j<8;j++) {
			if((crc16^c)&0x8000) crc16 = (crc16<<1)^0x1021;
			else crc16 <<= 1;

			c <<= 1;
		}

		_ioCrc16Table[i] = crc16;
	}
}

static u16 __make_crc16(void *buffer,u32 len)
{
	s32 i;
	u8 *ptr;
	u16 crc16;

	crc16 = 0;
	ptr = buffer;
	for(i=0;i<len;i++)
		crc16 = (crc16<<8)^_ioCrc16Table[((crc16>>8)^(u16)(ptr[i]))];

	return crc16;
}

static u32 __card_checktimeout(s32 drv_no,u32 startT,u32 timeout)
{
	u32 endT,diff;
	u32 msec;

	endT = gettick();
	if(endT<startT) {
		diff = (endT+(-1-startT))+1;
	} else
		diff = (endT-startT);

	msec = (diff/TB_TIMER_CLOCK);
	if(msec<=timeout) return 0;

	_ioError[drv_no] |= CARDIO_OP_TIMEDOUT;
	return 1;
}

static s32 __exi_unlock(s32 chn,s32 dev)
{
	LWP_ThreadBroadcast(_ioEXILock[chn]);
	return 1;
}

static void __exi_wait(s32 drv_no)
{
	u32 ret;

	do {
		if((ret=EXI_Lock(drv_no,EXI_DEVICE_0,__exi_unlock))==1) break;
		LWP_ThreadSleep(_ioEXILock[drv_no]);
	} while(ret==0);
}

static s32 __dma_done(s32 chn,s32 dev)
{
	LWP_ThreadBroadcast(_ioDmaQueue[chn]);
	return 1;
}

// Receive len bytes into ptr (pre-filled with _ioClrFlag). The cache-aligned middle
// goes over DMA and the thread sleeps until the transfer-complete interrupt clears
// EXI_FLAG_DMA; the unaligned head and tail use PIO.
static s32 __card_dmaread(s32 drv_no,u8 *ptr,u32 len)
{
	u32 level,head,body,tail;

	head = (-(u32)ptr)&0x1f;
	if(head>len) head = len;
	body = (len-head)&~0x1f;
	tail = len-head-body;

	if(head>0 && EXI_ImmEx(drv_no,ptr,head,EXI_READWRITE)==0) return 0;

	if(body>0) {
		DCInvalidateRange(ptr+head,body);
		_CPU_ISR_Disable(level);
		if(EXI_Dma(drv_no,ptr+head,body,EXI_READ,__dma_done)==0) {
			_CPU_ISR_Restore(level);
			return 0;
		}
		while(EXI_GetState(drv_no)&EXI_FLAG_DMA)
			LWP_ThreadSleep(_ioDmaQueue[drv_no]);
		_CPU_ISR_Restore(level);
	}

	if(tail>0 && EXI_ImmEx(drv_no,ptr+head+body,tail,EXI_READWRITE)==0) return 0;
	return 1;
}

static s32 __card_exthandler(s32 chn,s32 dev)
{
	_ioCardInserted[chn] = FALSE;
	_ioFlag[chn] = NOT_INITIALIZED;
	return 1;
}

static s32 __card_writecmd0(s32 drv_no)
{
	u8 crc;
	u32 cnt;
	u8 dummy[128];
	u8 cmd[5] = {0,0,0,0,0};

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	_ioClrFlag = 0xff;
	cmd[0] = 0x40;
	crc = __make_crc7(cmd,5);

	if(_ioWPFlag) {
		_ioClrFlag = 0x00;
		for(cnt=0;cnt<5;cnt++) cmd[cnt] ^= -1;
	}

	for(cnt=0;cnt<128;cnt++) dummy[cnt] = _ioClrFlag;

	__exi_wait(drv_no);

	if(EXI_SelectSD(drv_no,EXI_DEVICE_0,_ioCardFreq[drv_no])==0) {
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_NOCARD;
	}

	cnt = 0;
	while(cnt<20) {
		if(EXI_ImmEx(drv_no,dummy,128,EXI_WRITE)==0) {
			EXI_Deselect(drv_no);
			EXI_Unlock(drv_no);
			return CARDIO_ERROR_IOERROR;
		}
		cnt++;
	}
	EXI_Deselect(drv_no);

	if(EXI_Select(drv_no,EXI_DEVICE_0,_ioCardFreq[drv_no])==0) {
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_NOCARD;
	}

	crc |= 0x01;
	if(_ioWPFlag) crc ^= -1;
	if(EXI_ImmEx(drv_no,cmd,5,EXI_WRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	if(EXI_ImmEx(drv_no,&crc,1,EXI_WRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	EXI_Deselect(drv_no);
	EXI_Unlock(drv_no);
	return CARDIO_ERROR_READY;
}

static s32 __card_writecmd(s32 drv_no,void *buf,s32 len)
{
	u8 crc,*ptr;
	u8 dummy[32];
	u32 cnt;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ptr = buf;
	ptr[0] |= 0x40;
	crc = __make_crc7(buf,len);

	if(_ioWPFlag) {
		for(cnt=0;cnt<len;cnt++) ptr[cnt] ^= -1;
	}

	__exi_wait(drv_no);

	if(EXI_Select(drv_no,EXI_DEVICE_0,_ioCardFreq[drv_no])==0) {
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_NOCARD;
	}

	for(cnt=0;cnt<32;cnt++) dummy[cnt] = _ioClrFlag;

	if(EXI_ImmEx(drv_no,dummy,10,EXI_WRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	crc |= 0x01;
	if(_ioWPFlag) crc ^= -1;
	if(EXI_ImmEx(drv_no,buf,len,EXI_WRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}
	if(EXI_ImmEx(drv_no,&crc,1,EXI_WRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	EXI_Deselect(drv_no);
	EXI_Unlock(drv_no);
	return CARDIO_ERROR_READY;
}

static s32 __card_readresponse(s32 drv_no,void *buf,s32 len)
{
	u8 *ptr;
	u32 cnt;
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	__exi_wait(drv_no);

	if(__card_selectrx(drv_no)==0) {
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_NOCARD;
	}

	ret = CARDIO_ERROR_READY;
	ptr = buf;
	for(cnt=0;cnt<16;cnt++) {
		*ptr = _ioClrFlag;
		if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
			EXI_Deselect(drv_no);
			EXI_Unlock(drv_no);
			return CARDIO_ERROR_IOERROR;
		}
		if(!(*ptr&0x80)) break;
	}
	if(cnt>=16) ret = CARDIO_ERROR_IOTIMEOUT;
	if(len>1 && ret==CARDIO_ERROR_READY) {
		*(++ptr) = _ioClrFlag;
		if(EXI_ImmEx(drv_no,ptr,len-1,EXI_READWRITE)==0) ret = CARDIO_ERROR_IOERROR;
	}

	EXI_Deselect(drv_no);
	EXI_Unlock(drv_no);
	return ret;
}

static s32 __card_stopreadresponse(s32 drv_no,void *buf,s32 len)
{
	u8 *ptr,tmp;
	s32 startT,ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ptr = buf;

	__exi_wait(drv_no);

	if(__card_selectrx(drv_no)==0) {
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_NOCARD;
	}

	ret = CARDIO_ERROR_READY;
	*ptr = _ioClrFlag;
	if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	*ptr = _ioClrFlag;
	if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	startT = gettick();
	while(*ptr&0x80) {
		*ptr = _ioClrFlag;
		if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
			EXI_Deselect(drv_no);
			EXI_Unlock(drv_no);
			return CARDIO_ERROR_IOERROR;
		}
		if(!(*ptr&0x80)) break;
		if(__card_checktimeout(drv_no,startT,1500)!=0) {
			*ptr = _ioClrFlag;
			if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
				EXI_Deselect(drv_no);
				EXI_Unlock(drv_no);
				return CARDIO_ERROR_IOERROR;
			}
			if(*ptr&0x80) ret = CARDIO_ERROR_IOTIMEOUT;
			break;
		}
	}

	tmp = *ptr;
	while(*ptr!=0xff) {
		*ptr = _ioClrFlag;
		if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
			EXI_Deselect(drv_no);
			EXI_Unlock(drv_no);
			return CARDIO_ERROR_IOERROR;
		}
		if(*ptr==0xff) break;
		if(__card_checktimeout(drv_no,startT,1500)!=0) {
			*ptr = _ioClrFlag;
			if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
				EXI_Deselect(drv_no);
				EXI_Unlock(drv_no);
				return CARDIO_ERROR_IOERROR;
			}
			if(*ptr!=0xff) ret = CARDIO_ERROR_IOTIMEOUT;
			break;
		}
	}
	*ptr = tmp;

	if(len>1 && ret==CARDIO_ERROR_READY) {
		*(++ptr) = _ioClrFlag;
		if(EXI_ImmEx(drv_no,ptr,len-1,EXI_READWRITE)==0) ret = CARDIO_ERROR_IOERROR;
	}

	EXI_Deselect(drv_no);
	EXI_Unlock(drv_no);
	return ret;
}

static s32 __card_datares(s32 drv_no,void *buf)
{
	u8 *ptr;
	s32 startT,ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ptr = buf;

	__exi_wait(drv_no);

	if(__card_selectrx(drv_no)==0) {
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_NOCARD;
	}

	ret = CARDIO_ERROR_READY;
	*ptr = _ioClrFlag;
	if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}
	startT = gettick();
	while(*ptr&0x10) {
		*ptr = _ioClrFlag;
		if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
			EXI_Deselect(drv_no);
			EXI_Unlock(drv_no);
			return CARDIO_ERROR_IOERROR;
		}
		if(!(*ptr&0x10)) break;
		if(__card_checktimeout(drv_no,startT,1500)!=0) {
			*ptr = _ioClrFlag;
			if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
				EXI_Deselect(drv_no);
				EXI_Unlock(drv_no);
				return CARDIO_ERROR_IOERROR;
			}
			if(*ptr&0x10) ret = CARDIO_ERROR_IOTIMEOUT;
			break;
		}
	}

	*(++ptr) = _ioClrFlag;
	if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	startT = gettick();
	while(!*ptr) {
		*ptr = _ioClrFlag;
		if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
			EXI_Deselect(drv_no);
			EXI_Unlock(drv_no);
			return CARDIO_ERROR_IOERROR;
		}
		if(*ptr) break;
		if(__card_checktimeout(drv_no,startT,1500)!=0) {
			*ptr = _ioClrFlag;
			if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
				EXI_Deselect(drv_no);
				EXI_Unlock(drv_no);
				return CARDIO_ERROR_IOERROR;
			}
			if(!*ptr) ret = CARDIO_ERROR_IOTIMEOUT;
			break;
		}
	}
	EXI_Deselect(drv_no);
	EXI_Unlock(drv_no);

	return ret;
}

static s32 __card_stopresponse(s32 drv_no)
{
	s32 ret;

	if((ret=__card_stopreadresponse(drv_no,_ioResponse[drv_no],1))!=0) return ret;
	ret = __check_response(drv_no,_ioResponse[drv_no][0]);

	return ret;
}

static s32 __card_dataresponse(s32 drv_no)
{
	s32 ret;
	u8 res;

	if((ret=__card_datares(drv_no,_ioResponse[drv_no]))!=0) return ret;
	res = _SHIFTR(_ioResponse[drv_no][0],1,3);
	if(res==0x0005) ret = CARDIO_OP_IOERR_CRC;
	else if(res==0x0006) ret = CARDIO_OP_IOERR_WRITE;

	return ret;
}

static s32 __card_dataread(s32 drv_no,void *buf,u32 len)
{
	u8 *ptr;
	u32 cnt;
	u8 res[2];
	u16 crc,crc_org;
	s32 startT,ret,ok;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	__exi_wait(drv_no);

	if(__card_selectrx(drv_no)==0) {
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_NOCARD;
	}

	ret = CARDIO_ERROR_READY;
	ptr = buf;
	for(cnt=0;cnt<len;cnt++) ptr[cnt] = _ioClrFlag;
	if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	startT = gettick();
	while(*ptr!=0xfe) {
		*ptr = _ioClrFlag;
		if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
			EXI_Deselect(drv_no);
			EXI_Unlock(drv_no);
			return CARDIO_ERROR_IOERROR;
		}
		if(*ptr==0xfe) break;
		if(__card_checktimeout(drv_no,startT,1500)!=0) {
			*ptr = _ioClrFlag;
			if(EXI_ImmEx(drv_no,ptr,1,EXI_READWRITE)==0) {
				EXI_Deselect(drv_no);
				EXI_Unlock(drv_no);
				return CARDIO_ERROR_IOERROR;
			}
			if(*ptr!=0xfe) ret = CARDIO_ERROR_IOTIMEOUT;
			break;
		}
	}

	*ptr = _ioClrFlag;
	if(_ioUseDma[drv_no] && _ioFlag[drv_no]==INITIALIZED)
		ok = __card_dmaread(drv_no,ptr,len);
	else
		ok = EXI_ImmEx(drv_no,ptr,len,EXI_READWRITE);
	if(ok==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	/* sleep 1us*/
	usleep(1);

	res[0] = res[1] = _ioClrFlag;
	if(EXI_ImmEx(drv_no,res,2,EXI_READWRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}
	crc_org = ((res[0]<<8)&0xff00)|(res[1]&0xff);

	EXI_Deselect(drv_no);
	EXI_Unlock(drv_no);

	crc = __make_crc16(buf,len);
	if(crc!=crc_org) ret = CARDIO_OP_IOERR_CRC;
	return ret;
}

static s32 __card_multidatawrite(s32 drv_no,void *buf,u32 len)
{
	u8 dummy[32];
	u16 crc;
	u32 cnt;
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	for(cnt=0;cnt<32;cnt++) dummy[cnt] = _ioClrFlag;
	crc = __make_crc16(buf,len);

	__exi_wait(drv_no);

	if(EXI_Select(drv_no,EXI_DEVICE_0,_ioCardFreq[drv_no])==0) {
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_NOCARD;
	}

	dummy[0] = 0xfc;
	if(EXI_ImmEx(drv_no,dummy,1,EXI_WRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	if(EXI_ImmEx(drv_no,buf,len,EXI_WRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	/* sleep 1us*/
	usleep(1);

	ret = CARDIO_ERROR_READY;
	if(EXI_ImmEx(drv_no,&crc,2,EXI_WRITE)==0) ret = CARDIO_ERROR_IOERROR;

	EXI_Deselect(drv_no);
	EXI_Unlock(drv_no);

	return ret;
}

static s32 __card_multiwritestop(s32 drv_no)
{
	s32 ret,cnt,startT;
	u8 dummy[32];

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	for(cnt=0;cnt<32;cnt++) dummy[cnt] = _ioClrFlag;

	__exi_wait(drv_no);

	if(EXI_Select(drv_no,EXI_DEVICE_0,_ioCardFreq[drv_no])==0) {
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_NOCARD;
	}

	ret = CARDIO_ERROR_READY;
	dummy[0] = 0xfd;
	if(_ioWPFlag) dummy[0] = 0x02;		//!0xfd
	if(EXI_ImmEx(drv_no,dummy,1,EXI_WRITE)==0) {
		EXI_Deselect(drv_no);
		EXI_Unlock(drv_no);
		return CARDIO_ERROR_IOERROR;
	}

	for(cnt=0;cnt<4;cnt++) {
		dummy[0] = _ioClrFlag;
		if(EXI_ImmEx(drv_no,dummy,1,EXI_READWRITE)==0) {
			EXI_Deselect(drv_no);
			EXI_Unlock(drv_no);
			return CARDIO_ERROR_IOERROR;
		}
	}

	startT = gettick();
	ret = CARDIO_ERROR_READY;
	while(dummy[0]==0) {
		dummy[0] = _ioClrFlag;
		if(EXI_ImmEx(drv_no,dummy,1,EXI_READWRITE)==0) {
			EXI_Deselect(drv_no);
			EXI_Unlock(drv_no);
			return CARDIO_ERROR_IOERROR;
		}
		if(dummy[0]) break;
		if(__card_checktimeout(drv_no,startT,1500)!=0) {
			dummy[0] = _ioClrFlag;
			if(EXI_ImmEx(drv_no,dummy,1,EXI_READWRITE)==0) {
				EXI_Deselect(drv_no);
				EXI_Unlock(drv_no);
				return CARDIO_ERROR_IOERROR;
			}
			if(!dummy[0]) ret = CARDIO_ERROR_IOTIMEOUT;
			break;
		}
	}

	EXI_Deselect(drv_no);
	EXI_Unlock(drv_no);
	return ret;
}

static s32 __card_response1(s32 drv_no)
{
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],1))!=0) return ret;
	return __check_response(drv_no,_ioResponse[drv_no][0]);
}

static s32 __card_response2(s32 drv_no)
{
	u32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],2))!=0) return ret;
	if(!(_ioResponse[drv_no][0]&0x7c) && !(_ioResponse[drv_no][1]&0x9e)) return CARDIO_ERROR_READY;
	return CARDIO_ERROR_FATALERROR;
}

static s32 __card_sendappcmd(s32 drv_no)
{
	s32 ret;
	u8 ccmd[5] = {0,0,0,0,0};

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ccmd[0] = 0x37;
	if((ret=__card_writecmd(drv_no,ccmd,5))!=0) return ret;
	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],1))!=0) return ret;
	ret = __check_response(drv_no,_ioResponse[drv_no][0]);

	return ret;
}

static s32 __card_sendopcond(s32 drv_no)
{
	u8 ccmd[5] = {0,0,0,0,0};
	s32 ret;
	s32 startT;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ret = 0;
	startT = gettick();
	do {
		if(_initType[drv_no]==TYPE_SDHC) {
			__card_sendappcmd(drv_no);
			ccmd[0] = 0x29;
			ccmd[1] = 0x40;
		} else
			ccmd[0] = 0x01;

		if((ret=__card_writecmd(drv_no,ccmd,5))!=0) return ret;
		if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],1))!=0) return ret;
		if((ret=__check_response(drv_no,_ioResponse[drv_no][0]))!=0) return ret;
		if(!(_ioError[drv_no]&CARDIO_OP_IOERR_IDLE)) return CARDIO_ERROR_READY;

		ret = __card_checktimeout(drv_no,startT,1500);
	} while(ret==0);

	if(_initType[drv_no]==TYPE_SDHC) {
		__card_sendappcmd(drv_no);
		ccmd[0] = 0x29;
		ccmd[1] = 0x40;
	} else
		ccmd[0] = 0x01;

	if((ret=__card_writecmd(drv_no,ccmd,5))!=0) return ret;
	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],1))!=0) return ret;
	if((ret=__check_response(drv_no,_ioResponse[drv_no][0]))!=0) return ret;
	if(_ioError[drv_no]&CARDIO_OP_IOERR_IDLE) return CARDIO_ERROR_IOERROR;

	return CARDIO_ERROR_READY;
}

static s32 __card_sendCMD8(s32 drv_no)
{
	s32 ret;
	u8 ccmd[5] = {0,0,0,0,0};

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ccmd[0] = 0x08;
	ccmd[3] = 0x01;
	ccmd[4] = 0xAA;
	if((ret=__card_writecmd(drv_no,ccmd,5))!=0) return ret;
	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],5))!=0) return ret;
	ret = __check_response(drv_no,_ioResponse[drv_no][0]);

	return ret;
}

static s32 __card_sendCMD58(s32 drv_no)
{
	s32 ret;
	u8 ccmd[5] = {0,0,0,0,0};

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ccmd[0]= 0x3A;
	if((ret=__card_writecmd(drv_no,ccmd,5))!=0) return ret;
	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],5))!=0) return ret;
	ret = __check_response(drv_no,_ioResponse[drv_no][0]);

	return ret;
}

static s32 __card_sendcmd(s32 drv_no,u8 cmd,u8 *arg)
{
	u8 ccmd[5] = {0,0,0,0,0};

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ccmd[0] = cmd;
	if(arg) {
		ccmd[1] = arg[0];
		ccmd[2] = arg[1];
		ccmd[3] = arg[2];
		ccmd[4] = arg[3];
	}
	return __card_writecmd(drv_no,ccmd,5);
}

static s32 __card_setblocklen(s32 drv_no,u32 block_len)
{
	u8 cmd[5];
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;
	if(block_len>PAGE_SIZE512) block_len = PAGE_SIZE512;

	cmd[0] = 0x10;
	cmd[1] = (block_len>>24)&0xff;
	cmd[2] = (block_len>>16)&0xff;
	cmd[3] = (block_len>>8)&0xff;
	cmd[4] = block_len&0xff;
	if((ret=__card_writecmd(drv_no,cmd,5))!=0) return ret;
	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],1))<0) return ret;
	ret = __check_response(drv_no,_ioResponse[drv_no][0]);

	return ret;
}

static s32 __card_readcsd(s32 drv_no)
{
	u8 ccmd[5] = {0,0,0,0,0};
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ret = 0;
	ccmd[0] = 0x09;
	if((ret=__card_writecmd(drv_no,ccmd,5))!=0) return ret;
	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],1))!=0) return ret;
	ret = __check_response(drv_no,_ioResponse[drv_no][0]);
	if(ret==0) {
		if((ret=__card_dataread(drv_no,_ioCSD[drv_no],16))!=0) return ret;
	}
	return ret;
}

static s32 __card_readcid(s32 drv_no)
{
	u8 ccmd[5] = {0,0,0,0,0};
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ret = 0;
	ccmd[0] = 0x0A;
	if((ret=__card_writecmd(drv_no,ccmd,5))!=0) return ret;
	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],1))!=0) return ret;
	ret = __check_response(drv_no,_ioResponse[drv_no][0]);
	if(ret==0) {
		if((ret=__card_dataread(drv_no,_ioCID[drv_no],16))!=0) return ret;
	}
	return ret;
}

static s32 __card_sd_status(s32 drv_no)
{
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	if(_ioPageSize[drv_no]!=64) {
		_ioPageSize[drv_no] = 64;
		if((ret=__card_setblocklen(drv_no,_ioPageSize[drv_no]))!=0) return ret;
	}
	if((ret=__card_sendappcmd(drv_no))!=0) return ret;
	if((ret=__card_sendcmd(drv_no,0x0d,NULL))!=0) return ret;
	if((ret=__card_response2(drv_no))!=0) return ret;
	ret = __card_dataread(drv_no,_ioCardStatus[drv_no],64);

	return ret;
}

static s32 __card_softreset(s32 drv_no)
{
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ret = 0;
	if((ret=__card_writecmd0(drv_no))!=0) return ret;
	if((ret=__card_readresponse(drv_no,_ioResponse[drv_no],1))!=0) return ret;
	return __check_response(drv_no,_ioResponse[drv_no][0]);
}

static bool __card_check(s32 drv_no)
{
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return FALSE;
	if(drv_no==2) return TRUE;
	while((ret=EXI_ProbeEx(drv_no))==0);
	if(ret!=1) return FALSE;

	if(!(EXI_GetState(drv_no)&EXI_FLAG_ATTACH)) {
		if(EXI_Attach(drv_no,__card_exthandler)==0) return FALSE;
	}
	return TRUE;
}

static s32 __card_retrycb(s32 drv_no)
{
	_ioRetryCB = NULL;
	_ioRetryCnt++;
	return __card_initIO(drv_no);
}

static void __convert_sector(s32 drv_no,u32 sector_no,u8 *arg)
{
	if(_ioAddressingType[drv_no] == CARD_IO_BYTE_ADDRESSING) {
		arg[0] = (sector_no>>15)&0xff;
		arg[1] = (sector_no>>7)&0xff;
		arg[2] = (sector_no<<1)&0xff;
		arg[3] = (sector_no<<9)&0xff;
	} else if(_ioAddressingType[drv_no] == CARD_IO_SECTOR_ADDRESSING) {
		arg[0] = (sector_no>>24)&0xff;
		arg[1] = (sector_no>>16)&0xff;
		arg[2] = (sector_no>>8)&0xff;
		arg[3] = sector_no&0xff;
	}
}

static void __card_initIODefault(void)
{
	u32 i;

	__init_crc7();
	__init_crc16();
	for(i=0;i<MAX_DRIVE;++i) {
		_ioRetryCnt = 0;
		_ioError[i] = 0;
		_ioCardInserted[i] = FALSE;
		_ioFlag[i] = NOT_INITIALIZED;
		_ioAddressingType[i] = CARD_IO_BYTE_ADDRESSING;
		_initType[i] = TYPE_SD;
		_ioCardFreq[i] = SAFE_FREQ;
		_ioUseDma[i] = false;
		_ioLevel[i] = 0;
		_ioFailStage[i] = "";
		_ioFailBlock[i] = 0;
		LWP_InitQueue(&_ioEXILock[i]);
		LWP_InitQueue(&_ioDmaQueue[i]);
	}
}

static s32 __card_initIO(s32 drv_no)
{
	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	u32 id = 0;
	EXI_GetID(drv_no,EXI_DEVICE_0,&id);
	if ( id != -1 ) return CARDIO_ERROR_NOCARD;

	if(_ioRetryCnt>5) {
		_ioRetryCnt = 0;
		return CARDIO_ERROR_IOERROR;
	}

	_ioCardInserted[drv_no] = __card_check(drv_no);

	if(_ioCardInserted[drv_no]==TRUE) {
		_ioWPFlag = 0;
		_ioCardFreq[drv_no] = SAFE_FREQ;
		_ioUseDma[drv_no] = false;
		_ioSemiPassive[drv_no] = false;
		_initType[drv_no] = TYPE_SD;
		_ioFlag[drv_no] = INITIALIZING;
		_ioAddressingType[drv_no] = CARD_IO_BYTE_ADDRESSING;

		// While a DMA setting is being tried, reset the card with receives done under
		// EXI_SelectSD first. Only a semi-passive adapter keeps the card selected that
		// way, so if the card doesn't answer, the adapter is treated as passive: normal
		// selects and PIO (the DMA settings get skipped).
		bool probed = (drv_no!=0 && _ioLevels[_ioLevel[drv_no]].dma);
		if(probed) {
			_ioSemiPassive[drv_no] = true;
			if(__card_softreset(drv_no)!=0) _ioSemiPassive[drv_no] = false;
		}
		if(!_ioSemiPassive[drv_no] && __card_softreset(drv_no)!=0) {
			_ioWPFlag = 1;
			if(__card_softreset(drv_no)!=0) goto exit;
		}
		// Reported only once the card has answered, so an empty port stays quiet.
		if(probed)
			__sd_event("ch%d adapter: %s",drv_no,_ioSemiPassive[drv_no]?"semi-passive (DMA)":"passive (PIO only)");

		if(__card_sendCMD8(drv_no)!=0) goto exit;
		if((_ioResponse[drv_no][3]==1) && (_ioResponse[drv_no][4]==0xAA)) _initType[drv_no] = TYPE_SDHC;

		if(__card_sendopcond(drv_no)!=0) goto exit;
		if(__card_readcsd(drv_no)!=0) goto exit;
		if(__card_readcid(drv_no)!=0) goto exit;

		if(_initType[drv_no]==TYPE_SDHC) {
			if(__card_sendCMD58(drv_no)!=0) goto exit;
			if(_ioResponse[drv_no][1] & 0x40) {
				_ioAddressingType[drv_no] = CARD_IO_SECTOR_ADDRESSING;
			}
		}

		_ioPageSize[drv_no] = 1<<WRITE_BL_LEN(drv_no);
		if(__card_setblocklen(drv_no,_ioPageSize[drv_no])!=0) goto exit;

		if(__card_sd_status(drv_no)!=0) goto exit;

		_ioRetryCnt = 0;
		_ioFlag[drv_no] = INITIALIZED;

		__card_applyLevel(drv_no);
		return CARDIO_ERROR_READY;
exit:
		_ioRetryCB = __card_retrycb;
		return __card_doUnmount(drv_no);
	}
	return CARDIO_ERROR_NOCARD;
}

static s32 __card_preIO(s32 drv_no)
{
	s32 ret;

	if(_ioFlag[drv_no]!=INITIALIZED) {
		ret = __card_initIO(drv_no);
		if(ret!=CARDIO_ERROR_READY) return ret;
	}
	return CARDIO_ERROR_READY;
}

static s32 __card_readStatus(s32 drv_no)
{
	s32 ret;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ret = __card_preIO(drv_no);
	if(ret!=0) return ret;

	return __card_sd_status(drv_no);
}

static s32 __card_readSectorsOnce(s32 drv_no,u32 sector_no,u32 num_sectors,void *buf)
{
	u32 i;
	s32 ret;
	u8 arg[4] = {0};
	char *ptr = (char*)buf;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	_ioFailStage[drv_no] = "init";
	_ioFailBlock[drv_no] = 0;
	ret = __card_preIO(drv_no);
	if(ret!=0) return ret;

	if(num_sectors<1) return CARDIO_ERROR_INTERNAL;

	_ioFailStage[drv_no] = "setblocklen";
	if(PAGE_SIZE512!=_ioPageSize[drv_no]) {
		_ioPageSize[drv_no] = PAGE_SIZE512;
		if((ret=__card_setblocklen(drv_no,PAGE_SIZE512))!=0) return ret;
	}

	__convert_sector(drv_no,sector_no,arg);

	_ioFailStage[drv_no] = "cmd18";
	if((ret=__card_sendcmd(drv_no,0x12,arg))!=0) return ret;
	if((ret=__card_response1(drv_no))!=0) return ret;

	_ioFailStage[drv_no] = "data";
	for(i=0;i<num_sectors;i++) {
		_ioFailBlock[drv_no] = i;
		if((ret=__card_dataread(drv_no,ptr,_ioPageSize[drv_no]))!=0) return ret;
		ptr += _ioPageSize[drv_no];
	}

	_ioFailStage[drv_no] = "cmd12";
	if((ret=__card_sendcmd(drv_no,0x0C,NULL))!=0) return ret;
	return __card_stopresponse(drv_no);
}

static s32 __card_readSectors(s32 drv_no,u32 sector_no,u32 num_sectors,void *buf)
{
	s32 ret = __card_readSectorsOnce(drv_no,sector_no,num_sectors,buf);

	while(ret!=CARDIO_ERROR_READY && _ioLevel[drv_no]<LEVEL_COUNT-1) {
		__sd_event("ch%d read failed: stage=%s block=%u/%u sector=%u ret=%d mode=%s",
			drv_no,_ioFailStage[drv_no],_ioFailBlock[drv_no],num_sectors,sector_no,ret,_ioLevels[_ioLevel[drv_no]].name);

		// Step down and re-initialize the card: a failed read can leave it mid-transfer.
		_ioLevel[drv_no]++;
		_ioUseDma[drv_no] = false;
		_ioCardFreq[drv_no] = SAFE_FREQ;
		_ioFlag[drv_no] = NOT_INITIALIZED;
		ret = __card_readSectorsOnce(drv_no,sector_no,num_sectors,buf);
		if(ret==CARDIO_ERROR_READY)
			__sd_event("ch%d retry ok: mode=%s",drv_no,_ioLevels[_ioLevel[drv_no]].name);
	}
	if(ret!=CARDIO_ERROR_READY) {
		__sd_event("ch%d read failed: stage=%s block=%u/%u sector=%u ret=%d mode=%s (lowest)",
			drv_no,_ioFailStage[drv_no],_ioFailBlock[drv_no],num_sectors,sector_no,ret,_ioLevels[_ioLevel[drv_no]].name);
	}
	return ret;
}

static s32 __card_writeSectorsAt(s32 drv_no,u32 sector_no,u32 num_sectors,const void *buf)
{
	u32 i;
	s32 ret;
	u8 arg[4];
	char *ptr = (char*)buf;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	ret = __card_preIO(drv_no);
	if(ret!=0) return ret;

	if(num_sectors<1) return CARDIO_ERROR_INTERNAL;

	if(PAGE_SIZE512!=_ioPageSize[drv_no]) {
		_ioPageSize[drv_no] = PAGE_SIZE512;
		if((ret=__card_setblocklen(drv_no,_ioPageSize[drv_no]))!=0) return ret;
	}

	// send SET_WRITE_BLK_ERASE_CNT cmd
	arg[0] = (num_sectors>>24)&0xff;
	arg[1] = (num_sectors>>16)&0xff;
	arg[2] = (num_sectors>>8)&0xff;
	arg[3] = num_sectors&0xff;
	if((ret=__card_sendappcmd(drv_no))!=0) return ret;
	if((ret=__card_sendcmd(drv_no,0x17,arg))!=0) return ret;
	if((ret=__card_response1(drv_no))!=0) return ret;

	__convert_sector(drv_no,sector_no,arg);

	if((ret=__card_sendcmd(drv_no,0x19,arg))!=0) return ret;
	if((ret=__card_response1(drv_no))!=0) return ret;

	for(i=0;i<num_sectors;i++) {
		if((ret=__card_multidatawrite(drv_no,ptr,_ioPageSize[drv_no]))!=0) return ret;
		if((ret=__card_dataresponse(drv_no))!=0) {
			if((ret=__card_sendcmd(drv_no,0x0C,arg))!=0) return ret;
			return __card_stopresponse(drv_no);
		}
		ptr += _ioPageSize[drv_no];
	}

	if((ret=__card_multiwritestop(drv_no))!=0) return ret;
	if((ret=__card_sendcmd(drv_no,0x0D,NULL))!=0) return ret;
	return __card_response2(drv_no);
}

// Writes run at the stock 13.5 MHz clock, like libogc's driver: on the user's passive
// adapter, reads were fine at 27 MHz but written files (the SD log) never landed.
static s32 __card_writeSectors(s32 drv_no,u32 sector_no,u32 num_sectors,const void *buf)
{
	s32 ret;
	u32 freq;

	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	freq = _ioCardFreq[drv_no];
	_ioCardFreq[drv_no] = SAFE_FREQ;
	ret = __card_writeSectorsAt(drv_no,sector_no,num_sectors,buf);
	if(_ioFlag[drv_no]==INITIALIZED) _ioCardFreq[drv_no] = freq;
	return ret;
}

static s32 __card_doUnmount(s32 drv_no)
{
	if(drv_no<0 || drv_no>=MAX_DRIVE) return CARDIO_ERROR_NOCARD;

	if(_ioCardInserted[drv_no]==TRUE) {
		_ioCardInserted[drv_no] = FALSE;
		_ioFlag[drv_no] = NOT_INITIALIZED;
		if(drv_no!=2) EXI_Detach(drv_no);
	}
	if(_ioRetryCB)
		return _ioRetryCB(drv_no);

	return CARDIO_ERROR_READY;
}

/* DISC_INTERFACE wrappers */

static int __octsd_init = 0;
static int _octDefaultChan = -1;

const char *OctSd_GetModeName(int chan);

static bool __octsd_startup(int n)
{
	if(!__octsd_init) {
		__card_initIODefault();
		__octsd_init = 1;
	}
	return __card_preIO(n)==CARDIO_ERROR_READY;
}

static bool __octsd_isInserted(int n)
{
	return __card_readStatus(n)==CARDIO_ERROR_READY;
}

static bool __octsd_readSectors(int n,sec_t sector,sec_t numSectors,void *buffer)
{
	return __card_readSectors(n,sector,numSectors,buffer)==CARDIO_ERROR_READY;
}

static bool __octsd_writeSectors(int n,sec_t sector,sec_t numSectors,const void *buffer)
{
	return __card_writeSectors(n,sector,numSectors,buffer)==CARDIO_ERROR_READY;
}

static bool __octsd_shutdown(int n)
{
	__card_doUnmount(n);
	return true;
}

static bool __octsda_startup(void) { return __octsd_startup(0); }
static bool __octsda_isInserted(void) { return __octsd_isInserted(0); }
static bool __octsda_readSectors(sec_t s,sec_t n,void *b) { return __octsd_readSectors(0,s,n,b); }
static bool __octsda_writeSectors(sec_t s,sec_t n,const void *b) { return __octsd_writeSectors(0,s,n,b); }
static bool __octsda_clearStatus(void) { return true; }
static bool __octsda_shutdown(void) { return __octsd_shutdown(0); }

static bool __octsdb_startup(void) { return __octsd_startup(1); }
static bool __octsdb_isInserted(void) { return __octsd_isInserted(1); }
static bool __octsdb_readSectors(sec_t s,sec_t n,void *b) { return __octsd_readSectors(1,s,n,b); }
static bool __octsdb_writeSectors(sec_t s,sec_t n,const void *b) { return __octsd_writeSectors(1,s,n,b); }
static bool __octsdb_clearStatus(void) { return true; }
static bool __octsdb_shutdown(void) { return __octsd_shutdown(1); }

static bool __octsd2_startup(void) { return __octsd_startup(2); }
static bool __octsd2_isInserted(void) { return __octsd_isInserted(2); }
static bool __octsd2_readSectors(sec_t s,sec_t n,void *b) { return __octsd_readSectors(2,s,n,b); }
static bool __octsd2_writeSectors(sec_t s,sec_t n,const void *b) { return __octsd_writeSectors(2,s,n,b); }
static bool __octsd2_clearStatus(void) { return true; }
static bool __octsd2_shutdown(void) { return __octsd_shutdown(2); }

static const DISC_INTERFACE __io_octsda = {
	DEVICE_TYPE_GC_SD,
	FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_GAMECUBE_SLOTA,
	__octsda_startup, __octsda_isInserted, __octsda_readSectors,
	__octsda_writeSectors, __octsda_clearStatus, __octsda_shutdown
};
static const DISC_INTERFACE __io_octsdb = {
	DEVICE_TYPE_GC_SD,
	FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_GAMECUBE_SLOTB,
	__octsdb_startup, __octsdb_isInserted, __octsdb_readSectors,
	__octsdb_writeSectors, __octsdb_clearStatus, __octsdb_shutdown
};
static const DISC_INTERFACE __io_octsd2 = {
	DEVICE_TYPE_GC_SD,
	FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_GAMECUBE_PORT2,
	__octsd2_startup, __octsd2_isInserted, __octsd2_readSectors,
	__octsd2_writeSectors, __octsd2_clearStatus, __octsd2_shutdown
};

/* Mounts like libfat's fatInitDefault() on GameCube: "sd" (serial port 2), then
   "carda", "cardb"; the first one mounted becomes the working directory. libfat's
   GameCube defaults are 4 cache pages of 64 sectors. Returns the EXI channel that
   became the default device, or -1 if none mounted. */
int OctSd_MountAll(void)
{
	static const struct { const char *name; const DISC_INTERFACE *disc; int chan; } devices[] = {
		{ "sd", &__io_octsd2, 2 },
		{ "carda", &__io_octsda, 0 },
		{ "cardb", &__io_octsdb, 1 },
	};
	int defaultChan = -1;
	int i;

	for(i=0;i<3;i++) {
		if(!fatMount(devices[i].name,devices[i].disc,0,4,64)) continue;
		__sd_event("mounted %s: EXI ch%d mode=%s",devices[i].name,devices[i].chan,OctSd_GetModeName(devices[i].chan));
		if(defaultChan<0) {
			char path[16];
			strcpy(path,devices[i].name);
			strcat(path,":/");
			chdir(path);
			defaultChan = devices[i].chan;
		}
	}
	_octDefaultChan = defaultChan;
	return defaultChan;
}

/* Transfer mode of an EXI channel (-1 = the default device) for the diagnostic log:
   one of the _ioLevels names, "not-ready" while the card is (re)initializing, or
   "stock" when this driver mounted nothing (libogc's fatInitDefault was used). */
const char *OctSd_GetModeName(int chan)
{
	if(chan<0) chan = _octDefaultChan;
	if(chan<0 || chan>=MAX_DRIVE) return "stock";
	if(_ioFlag[chan]!=INITIALIZED) return "not-ready";
	return _ioLevels[_ioLevel[chan]].name;
}

/* Copies the oldest queued driver event into buffer. Returns 0 when there are none. */
int OctSd_TakeEvent(char *buffer,int size)
{
	u32 level;
	int taken = 0;

	if(buffer==NULL || size<=0) return 0;

	_CPU_ISR_Disable(level);
	if(_ioEventCount>0) {
		strncpy(buffer,_ioEvents[_ioEventHead],size-1);
		buffer[size-1] = '\0';
		_ioEventHead = (_ioEventHead+1)%EVENT_COUNT;
		_ioEventCount--;
		taken = 1;
	}
	_CPU_ISR_Restore(level);
	return taken;
}

#endif
