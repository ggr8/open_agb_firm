/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2021 derrek, profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include "drivers/mmc/sdmmc.h"
#include "drivers/toshsd.h"
#include "drivers/toshsd_config.h"
#ifdef _3DS
#ifdef ARM9
#include "arm9/drivers/timer.h"
#include "util.h" // wait_cycles()
#elif ARM11
#include "arm11/drivers/timer.h"
#endif // #ifdef ARM9
#elif TWL
	// TODO
#endif // #ifdef _3DS
#include "drivers/mmc/sd_spec.h"
#include "drivers/mmc/mmc_spec.h"


// Note on INIT_CLOCK:
// 400 kHz is allowed by the specs. 523 kHz has been proven to work reliably
// for SD cards and eMMC but very early MMCs can fail at init.
// We lose about 5 ms of time on init by using 261 kHz.
#ifdef _3DS
#ifdef ARM9
// TODO: Use a timer instead? The delay is only a few hundred us though.
#define INIT_DELAY_FUNC()  wait_cycles(2 * 256 * 74)      // ARM9 timer clock = controller clock. CPU is x2 timer clock.
#elif ARM11
#define INIT_DELAY_FUNC()  TIMER_sleepTicks(2 * 256 * 74) // ARM11 timer is x2 controller clock.
#endif // #ifdef ARM9

#define INIT_CLOCK     (SD_CLK_DIV_256)                 // 261 kHz (maximum 400 kHz).
#define DEFAULT_CLOCK  (SD_CLK_AUTO_OFF | SD_CLK_DIV_4) // 16.756991 MHz (maximum 20 MHz).
#define HS_CLOCK       (SD_CLK_AUTO_OFF | SD_CLK_DIV_2) // 33.513982 MHz (maximum 50 MHz).

#elif TWL

//#define INIT_DELAY_FUNC()  TIMER_sleepTicks(1 * 128 * 74) // ARM9 timer clock = controller clock.
#error "SD/MMC necessary delay unimplemented."

#define INIT_CLOCK     (SD_CLK_DIV_128) // 261 kHz (maximum 400 kHz).
#define DEFAULT_CLOCK  (SD_CLK_DIV_2)   // 16.756991 MHz (maximum 20 MHz).
#endif // #ifdef _3DS


#define SD_IF_COND_ARG     (SD_CMD8_VHS_2_7_3_6V | SD_CMD8_CHK_PATT)
#define SD_OP_COND_ARG     (SD_ACMD41_XPC | SD_OCR_3_2_3_3V)          // We support 150 mA and 3.3V. Without HCS bit.
#define MMC_OP_COND_ARG    (/*MMC_OCR_SECT_MODE |*/ MMC_OCR_3_2_3_3V) // We support s̶e̶c̶t̶o̶r̶ a̶d̶r̶e̶s̶s̶i̶n̶g̶ a̶n̶d̶ 3.3V.
#define SD_OCR_VOLT_MASK   (SD_OCR_3_2_3_3V)                          // We support 3.3V only.
#define MMC_OCR_VOLT_MASK  (MMC_OCR_3_2_3_3V)                         // We support 3.3V only.

enum
{
	// Card types.
	CTYPE_NONE  = 0u, // Unitialized/no card.
	CTYPE_MMC   = 1u, // (e)MMC.
	CTYPE_MMCHC = 2u, // High capacity (e)MMC (>2 GB).
	CTYPE_SDSC  = 3u, // SDSC.
	CTYPE_SDHC  = 4u, // SDHC, SDXC.
	CTYPE_SDUC  = 5u  // SDUC.
};


typedef struct
{
	ToshsdPort port;
	u8 cardType;
	u8 spec_vers;   // (e)MMC only SPEC_VERS from CSD. 0 for SD.
	u16 rca;        // Relative Card Address (RCA).
	u16 ccc;        // SD/(e)MMC command class support from CSD. One per bit starting at 0.
	u32 sectors;    // Size in 512 byte units.

	// Cached card infos.
	u32 cid[4];     // Raw CID without the CRC.
} SdmmcDev;

static SdmmcDev g_devs[2] = {0};



/*static u32 sendCardStatus(ToshsdPort *const port, u32 rca, u32 *const statusOut)
{
	// Same CMD for SD/(e)MMC but the argument format differs slightly.
	const u32 res = TOSHSD_sendCommand(port, MMC_SEND_STATUS, rca);
	if(res == 0) *statusOut = port->resp[0];

	return res;
}*/

static u32 sdSendAppCmd(ToshsdPort *const port, const u16 cmd, const u32 arg, const u32 rca)
{
	u32 res = TOSHSD_sendCommand(port, SD_APP_CMD, rca);
	if(res == 0)
	{
		res = TOSHSD_sendCommand(port, cmd, arg);
	}

	return res;
}

static u32 goIdleState(ToshsdPort *const port)
{
	// Enter idle state before we start the init procedure.
	// Works from all but inactive state. CMD is the same for SD/(e)MMC.
	// For (e)MMC there are optional init paths:
	// arg = 0x00000000 -> GO_IDLE_STATE.
	// arg = 0xF0F0F0F0 -> GO_PRE_IDLE_STATE.
	// arg = 0xFFFFFFFA -> BOOT_INITIATION.
	u32 res = TOSHSD_sendCommand(port, MMC_GO_IDLE_STATE, 0);
	if(res != 0) return SDMMC_ERR_GO_IDLE_STATE;

	return SDMMC_ERR_NONE;
}

static u32 initIdleState(ToshsdPort *const port, u8 *const cardTypeOut)
{
	// Tell the card what interfaces and voltages we support.
	// Only SD v2 and up will respond. (e)MMC won't respond.
	u32 res = TOSHSD_sendCommand(port, SD_SEND_IF_COND, SD_IF_COND_ARG);
	if(res == 0)
	{
		// If the card supports the interfaces and voltages
		// it should echo back the check pattern and set the
		// support bits.
		// Since we don't support anything but the
		// standard SD interface at 3.3V we can check
		// the whole response at once.
		if(port->resp[0] != SD_IF_COND_ARG) return SDMMC_ERR_IF_COND_RESP;
	}
	else if(res != STATUS_ERR_CMD_TIMEOUT) return SDMMC_ERR_SEND_IF_COND; // Card responded but an error occured.

	// Send the first app CMD. If this times out it's (e)MMC.
	// If SEND_IF_COND timed out tell the SD card we are a v1 host.
	const u32 opCondArg = SD_OP_COND_ARG | (res<<8 ^ SD_ACMD41_HCS); // Caution! Controller specific hack.
	u8 cardType = CTYPE_SDSC;
	res = sdSendAppCmd(port, SD_APP_SD_SEND_OP_COND, opCondArg, 0);
	if(res != 0)
	{
		if(res == STATUS_ERR_CMD_TIMEOUT) cardType = CTYPE_MMC;          // Continue with (e)MMC init.
		else                              return SDMMC_ERR_SEND_OP_COND; // Unknown error.
	}

	if(cardType == CTYPE_MMC) // (e)MMC.
	{
		// Loop until a timeout of 1 second or the card is ready.
		u32 tries = 200;
		u32 ocr;
		while(1)
		{
			res = TOSHSD_sendCommand(port, MMC_SEND_OP_COND, MMC_OP_COND_ARG);
			if(res != 0) return SDMMC_ERR_SEND_OP_COND;

			ocr = port->resp[0];
			if(!--tries || (ocr & MMC_OCR_NOT_BUSY)) break;

			// Linux uses 10 ms but the card doesn't become ready faster
			// when polling with delay. Use 5 ms as compromise so not much
			// time is wasted when the card becomes ready in the middle of the delay.
			TIMER_sleepMs(5);
		}

		// (e)MMC didn't finish init within 1 second.
		if(tries == 0) return SDMMC_ERR_OP_COND_TMOUT;

		// Check if the (e)MMC supports the voltage and if it's high capacity.
		if(!(ocr & MMC_OCR_VOLT_MASK)) return SDMMC_ERR_VOLT_SUPPORT; // Voltage not supported.
		// TODO: High capacity (e)MMC check.
	}
	else // SD card.
	{
		// Loop until a timeout of 1 second or the card is ready.
		u32 tries = 200;
		u32 ocr;
		while(1)
		{
			ocr = port->resp[0];
			if(!--tries || (ocr & SD_OCR_NOT_BUSY)) break;

			// Linux uses 10 ms but the card doesn't become ready faster
			// when polling with delay. Use 5 ms as compromise so not much
			// time is wasted when the card becomes ready in the middle of the delay.
			TIMER_sleepMs(5);

			res = sdSendAppCmd(port, SD_APP_SD_SEND_OP_COND, opCondArg, 0);
			if(res != 0) return SDMMC_ERR_SEND_OP_COND;
		}

		// SD card didn't finish init within 1 second.
		if(tries == 0) return SDMMC_ERR_OP_COND_TMOUT;

		// TODO: From sd.c in Linux:
		// "Some SD cards claims an out of spec VDD voltage range.
		//  Let's treat these bits as being in-valid and especially also bit7."
		if(!(ocr & SD_OCR_VOLT_MASK)) return SDMMC_ERR_VOLT_SUPPORT; // Voltage not supported.
		if(ocr & SD_OCR_CCS)          cardType = CTYPE_SDHC;
	}

	*cardTypeOut = cardType;

	return SDMMC_ERR_NONE;
}

static u32 initReadyState(SdmmcDev *const dev)
{
	ToshsdPort *const port = &dev->port;

	// SD card voltage switch sequence goes here if supported.

	// Get the CID. CMD is the same for SD/(e)MMC.
	u32 res = TOSHSD_sendCommand(port, MMC_ALL_SEND_CID, 0);
	if(res != 0) return SDMMC_ERR_ALL_SEND_CID;
	memcpy(dev->cid, port->resp, 16);

	return SDMMC_ERR_NONE;
}

static u32 initIdentState(SdmmcDev *const dev, const u8 cardType, u32 *const rcaOut)
{
	ToshsdPort *const port = &dev->port;

	u32 rca;
	if(cardType < CTYPE_SDSC) // (e)MMC.
	{
		// Set the RCA of the (e)MMC to 1. 0 is reserved.
		// A few extremely old, unbranded (but Nokia?) MMC's will time
		// out here for unknown reason. They won't work on DSi anyway (FAT12).
		// The RCA is in the upper 16 bits of the argument.
		u32 res = TOSHSD_sendCommand(port, MMC_SET_RELATIVE_ADDR, 1u<<16); // TODO: Should we check the R1 response?
		if(res != 0) return SDMMC_ERR_SET_SEND_RCA;

		rca = 1;
	}
	else // SD card.
	{
		// Ask the SD card to send its RCA.
		u32 res = TOSHSD_sendCommand(port, SD_SEND_RELATIVE_ADDR, 0);
		if(res != 0) return SDMMC_ERR_SET_SEND_RCA;

		rca = port->resp[0]>>16; // RCA in upper 16 bits.
	}

	dev->rca = rca;
	*rcaOut = rca<<16;

	return SDMMC_ERR_NONE;
}

// Based on code from linux/drivers/mmc/core/sd.c.
// Works only with u32[4] response.
#define UNSTUFF_BITS(resp, start, size)                     \
({                                                          \
	const u32 __size = size;                                \
	const u32 __mask = (__size < 32 ? 1u<<__size : 0u) - 1; \
	const u32 __off = 3 - ((start) / 32u);                  \
	const u32 __shift = (start) & 31u;                      \
	u32 __res;                                              \
	                                                        \
	__res = resp[__off]>>__shift;                           \
	if(__size + __shift > 32)                               \
		__res |= resp[__off - 1]<<((32 - __shift) % 32u);   \
	__res & __mask;                                         \
})

static void parseCsd(SdmmcDev *const dev, const u8 cardType)
{
	// Note: The MSBs are in csd[0].
	const u32 *const csd = dev->port.resp;

	// structure = 0 is CSD version 1.0.
	const u8 structure = UNSTUFF_BITS(csd, 126, 2); // [127:126]
	dev->spec_vers = UNSTUFF_BITS(csd, 122, 4);     // [125:122] All 0 for SD cards.
	u32 sectors;
	if(structure == 0 || cardType == CTYPE_MMC)
	{
		// Same calculation for SDSC and (e)MMC <=2 GB.
		const u32 read_bl_len = UNSTUFF_BITS(csd, 80, 4);  // [83:80]
		const u32 c_size      = UNSTUFF_BITS(csd, 62, 12); // [73:62]
		const u32 c_size_mult = UNSTUFF_BITS(csd, 47, 3);  // [49:47]

		// Note: READ_BL_LEN is at least 9.
		// Slightly modified to calculate sectors instead of bytes.
		sectors = (c_size + 1) * (1u<<(c_size_mult + 2)) * (1u<<(read_bl_len - 9));
	}
	else
	{
		// SD CSD version 3.0 format.
		// For version 2.0 this is 22 bits however the upper bits
		// are reserved and zero filled so this is fine.
		const u32 c_size = UNSTUFF_BITS(csd, 48, 28); // [75:48]

		sectors = (c_size + 1) * 1024u;
	}
	// TODO: High capacity (e)MMC encodes the size in the ext CSD. We have to implement this for N2DS XL.
	dev->sectors = sectors;

	dev->ccc = UNSTUFF_BITS(csd, 84, 12); // [95:84]
}

static u32 initStandbyState(SdmmcDev *const dev, const u8 cardType, const u32 rca)
{
	ToshsdPort *const port = &dev->port;

	// Get the CSD. CMD is the same for SD/(e)MMC.
	u32 res = TOSHSD_sendCommand(port, MMC_SEND_CSD, rca);
	if(res != 0) return SDMMC_ERR_SEND_CSD;
	parseCsd(dev, cardType);

	// Select card and switch to transfer state.
	const u16 selCardCmd = (cardType < CTYPE_SDSC ? MMC_SELECT_CARD : SD_SELECT_CARD);
	res = TOSHSD_sendCommand(port, selCardCmd, rca); // TODO: Should we check the R1 response?
	if(res != 0) return SDMMC_ERR_SELECT_CARD;

	// The SD card spec mentions that we should check the lock bit in the
	// response to CMD7 to identify cards requiring a password
	// to unlock which we don't support. Same seems to apply for (e)MMC.
	// Same bit for SD/(e)MMC R1 card status.
	if(port->resp[0] & MMC_R1_CARD_IS_LOCKED)
		return SDMMC_ERR_LOCKED;

	return SDMMC_ERR_NONE;
}

// TODO: Set the timeout based on clock speed (Toshsd uses SDCLK for timeouts).
static u32 initTranState(SdmmcDev *const dev, const u8 cardType, const u32 rca)
{
	ToshsdPort *const port = &dev->port;

	if(cardType < CTYPE_SDSC) // (e)MMC.
	{
		// Very old 1 bit bus MMC will time out and set the SWITCH_ERROR bit
		// for these CMDs. Only try with (e)MMC spec >4.0.
		if(dev->spec_vers >= 4) // Version 4.1–4.2–4.3 or higher.
		{
			// Switch to 4 bit bus mode.
			u32 arg = MMC_SWITCH_ARG(MMC_SWITCH_ACC_WR_BYTE, 183, 1, 0);
			u32 res = TOSHSD_sendCommand(port, MMC_SWITCH, arg);
			if(res != 0) return SDMMC_ERR_SET_BUS_WIDTH;
			TOSHSD_setBusWidth(port, 4);

#ifndef TWL
			// Switch to high speed timing (max. 52 MHz).
			arg = MMC_SWITCH_ARG(MMC_SWITCH_ACC_WR_BYTE, 185, 1, 0);
			res = TOSHSD_sendCommand(port, MMC_SWITCH, arg);
			if(res != 0) return SDMMC_ERR_SWITCH_HS;
			TOSHSD_setClock(port, HS_CLOCK);
#endif

			// We also should check in the ext CSD the power budget for the card.
			// Nintendo seems to leave it on default (no change).
		}
	}
	else // SD card.
	{
		// Remove DAT3 pull-up.
		u32 res = sdSendAppCmd(port, SD_APP_SET_CLR_CARD_DETECT, 0, rca); // arg = 0 removes the pull-up.
		if(res != 0) return SDMMC_ERR_SET_CLR_CD;

		// Switch to 4 bit bus mode.
		res = sdSendAppCmd(port, SD_APP_SET_BUS_WIDTH, 2, rca); // arg = 2 is 4 bit bus width.
		if(res != 0) return SDMMC_ERR_SET_BUS_WIDTH;
		TOSHSD_setBusWidth(port, 4);

#ifndef TWL
		// TODO: Is it faster to double the clock earlier or to run this CMD with 4 bit bus width?
		if(dev->ccc & 1u<<10) // Class 10 command support.
		{
			TOSHSD_setBlockLen(port, 64);
			alignas(4) u8 switchStat[64]; // MSB first and big endian.
			TOSHSD_setBuffer(port, (u32*)switchStat, 1);
			const u32 arg = SD_SWITCH_FUNC_ARG(1, 0xF, 0xF, 0xF, 1);
			res = TOSHSD_sendCommand(port, SD_SWITCH_FUNC, arg);
			if(res != 0) return SDMMC_ERR_SWITCH_HS;

			// Restore default 512 bytes block length.
			TOSHSD_setBlockLen(port, 512);

			// [415:400] Support Bits of Functions in Function Group 1.
			if(switchStat[63u - 400 / 8] & 1u<<1) // Is group 1, function 1 "High-Speed" supported?
			{
				// High-Speed (max. 50 MHz at 3.3V) supported. Switch to highest supported clock.
				TOSHSD_setClock(port, HS_CLOCK);
			}
		}
#endif
	}

	// SD:     The description for CMD SET_BLOCKLEN says 512 bytes is the default.
	// (e)MMC: The description for READ_BL_LEN (CSD) says 512 bytes is the default.
	// So it's not required to set the block length?
	//u32 res = TOSHSD_sendCommand(port, MMC_SET_BLOCKLEN, 512);
	//if(res != 0) return SDMMC_ERR_SET_BLOCKLEN;

	return SDMMC_ERR_NONE;
}

ALWAYS_INLINE u8 dev2portNum(const u8 devNum)
{
	return (devNum == SDMMC_DEV_eMMC ? TOSHSD_eMMC_PORT : TOSHSD_CARD_PORT);
}

// TODO: In many places we also want to check the card's response.
u32 SDMMC_init(const u8 devNum)
{
	if(devNum > SDMMC_DEV_eMMC) return SDMMC_ERR_INVAL_PARAM;

	SdmmcDev *const dev = &g_devs[devNum];
	if(dev->cardType != CTYPE_NONE) return SDMMC_ERR_INITIALIZED;

	ToshsdPort *const port = &dev->port;
	TOSHSD_initPort(port, dev2portNum(devNum));
	TOSHSD_setClockImmediately(port, INIT_CLOCK); // Continuous init clock.
	INIT_DELAY_FUNC();

	u32 res = goIdleState(port);
	if(res != 0) return res;

	// SD/(e)MMC now in idle state (idle).
	u8 cardType;
	res = initIdleState(port, &cardType);
	if(res != 0) return res;

	// Stop clock at idle, init clock.
	TOSHSD_setClock(port, SD_CLK_AUTO_OFF | INIT_CLOCK);

	// SD/(e)MMC now in ready state (ready).
	res = initReadyState(dev);
	if(res != 0) return res;

	// SD/(e)MMC now in identification state (ident).
	u32 rca;
	res = initIdentState(dev, cardType, &rca);
	if(res != 0) return res;

	// Maximum at this point would be 20 MHz for (e)MMC and 25 for SD.
	// SD: We can increase the clock after end of identification state.
	// TODO: eMMC spec section 7.6
	// "Until the contents of the CSD register is known by the host,
	// the fPP clock rate must remain at fOD. (See Section 12.7 on page 176.)"
	// Since the absolute minimum clock rate is 20 MHz and we are in push-pull
	// mode already can we cheat and switch to <=20 MHz before getting the CSD?
	// Note: This seems to be working just fine in all tests.
	TOSHSD_setClock(port, DEFAULT_CLOCK);

	// SD/(e)MMC now in stand-by state (stby).
	res = initStandbyState(dev, cardType, rca);
	if(res != 0) return res;

	// SD/(e)MMC now in transfer state (tran).
	res = initTranState(dev, cardType, rca);
	if(res != 0) return res;

	dev->cardType = cardType;

	return SDMMC_ERR_NONE;
}

// TODO: Is there any "best practice" way of deinitializing cards?
//       Kick the card back into idle state maybe?
//       Linux seems to deselect cards on "suspend".
u32 SDMMC_deinit(const u8 devNum)
{
	if(devNum > SDMMC_DEV_eMMC) return SDMMC_ERR_INVAL_PARAM;

	g_devs[devNum].cardType = CTYPE_NONE;

	return SDMMC_ERR_NONE;
}

// TODO: Less controller dependent code.
u32 SDMMC_getDevInfo(const u8 devNum, SdmmcInfo *const infoOut)
{
	if(devNum > SDMMC_DEV_eMMC) return SDMMC_ERR_INVAL_PARAM;

	const SdmmcDev *const dev = &g_devs[devNum];
	const ToshsdPort *const port = &dev->port;

	infoOut->type      = dev->cardType;
	infoOut->spec_vers = dev->spec_vers;
	infoOut->rca       = dev->rca;
	infoOut->sectors   = dev->sectors;

	const u32 clkSetting = port->sd_clk_ctrl & 0xFFu;
	infoOut->clock     = TOSHSD_HCLK / (clkSetting ? clkSetting<<2 : 2u);

	memcpy(infoOut->cid, dev->cid, 16);
	infoOut->ccc       = dev->ccc;
	infoOut->busWidth  = (port->sd_option & OPTION_BUS_WIDTH1 ? 1u : 4u);

	return SDMMC_ERR_NONE;
}

u32 SDMMC_getCid(const u8 devNum, u32 cidOut[4])
{
	if(devNum > SDMMC_DEV_eMMC) return SDMMC_ERR_INVAL_PARAM;

	if(cidOut != NULL) memcpy(cidOut, g_devs[devNum].cid, 16);

	return SDMMC_ERR_NONE;
}

u32 SDMMC_getSectors(const u8 devNum)
{
	if(devNum > SDMMC_DEV_eMMC) return 0;

	return g_devs[devNum].sectors;
}

// TODO: The specs say on multi-block read an error can occur reading the last block (out of bounds).
//       This is normal and can be ignored.
u32 SDMMC_readSectors(const u8 devNum, u32 sect, u32 *const buf, const u16 count)
{
	if(devNum > SDMMC_DEV_eMMC || count == 0) return SDMMC_ERR_INVAL_PARAM;

	SdmmcDev *const dev = &g_devs[devNum];
	const u8 cardType = dev->cardType;
	if(cardType == CTYPE_NONE) return SDMMC_ERR_NO_CARD;

	ToshsdPort *const port = &dev->port;
	TOSHSD_setBuffer(port, buf, count);

	// Read a single 512 bytes block. Same CMD for SD/(e)MMC.
	// Read multiple 512 byte blocks. Same CMD for SD/(e)MMC.
	const u16 cmd = (count == 1 ? MMC_READ_SINGLE_BLOCK : MMC_READ_MULTIPLE_BLOCK);
	if(cardType == CTYPE_MMC || cardType == CTYPE_SDSC) sect *= 512; // Byte addressing.
	const u32 res = TOSHSD_sendCommand(port, cmd, sect);
	if(res != 0) return SDMMC_ERR_SECT_RW; // TODO: In case of errors check the card status.

	return SDMMC_ERR_NONE;
}

// TODO: The specs say on multi-block write an error can occur writing the last block (out of bounds).
//       This is normal and can be ignored.
u32 SDMMC_writeSectors(const u8 devNum, u32 sect, const u32 *const buf, const u16 count)
{
	if(devNum > SDMMC_DEV_eMMC || count == 0) return SDMMC_ERR_INVAL_PARAM;
	if(devNum == SDMMC_DEV_CARD && !TOSHSD_cardSliderUnlocked())
		return SDMMC_ERR_WRITE_PROT;

	SdmmcDev *const dev = &g_devs[devNum];
	const u8 cardType = dev->cardType;
	if(cardType == CTYPE_NONE) return SDMMC_ERR_NO_CARD;

	ToshsdPort *const port = &dev->port;
	TOSHSD_setBuffer(port, (u32*)buf, count);

	// Write a single 512 bytes block. Same CMD for SD/(e)MMC.
	// Write multiple 512 byte blocks. Same CMD for SD/(e)MMC.
	const u16 cmd = (count == 1 ? MMC_WRITE_BLOCK : MMC_WRITE_MULTIPLE_BLOCK);
	if(cardType == CTYPE_MMC || cardType == CTYPE_SDSC) sect *= 512; // Byte addressing.
	const u32 res = TOSHSD_sendCommand(port, cmd, sect);
	if(res != 0) return SDMMC_ERR_SECT_RW; // TODO: In case of errors check the card status.

	return SDMMC_ERR_NONE;
}
