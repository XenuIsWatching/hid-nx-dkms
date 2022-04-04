// SPDX-License-Identifier: GPL-2.0+
/*
 * HID driver for Nintendo Switch Controllers
 *
 * Copyright (c) 2019-2021 Daniel J. Ogorchock <djogorchock@gmail.com>
 * Portions Copyright (c) 2020 Nadia Holmquist Pedersen <nadia@nhp.sh>
 * Copyright (c) 2022 Emily Strickland <linux@emily.st>
 *
 * The following resources/projects were referenced for this driver:
 *   https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering
 *   https://gitlab.com/pjranki/joycon-linux-kernel (Peter Rankin)
 *   https://github.com/FrotBot/SwitchProConLinuxUSB
 *   https://github.com/MTCKC/ProconXInput
 *   https://github.com/Davidobot/BetterJoyForCemu
 *   hid-wiimote kernel hid driver
 *   hid-logitech-hidpp driver
 *   hid-sony driver
 *
 * This driver supports the Nintendo Switch Joy-Cons and Pro Controllers. The
 * Pro Controllers can either be used over USB or Bluetooth.
 *
 * This driver also incorporates support for Nintendo Switch Online controllers
 * for the NES, SNES, and Sega Genesis.
 *
 * The driver will retrieve the factory calibration info from the controllers,
 * so little to no user calibration should be required.
 *
 */

#include "hid-ids.h"
#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>

/*
 * Reference the url below for the following HID report defines:
 * https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering
 */

/* Output Reports */
#define NX_CTLR_OUTPUT_RUMBLE_AND_SUBCMD	0x01
#define NX_CTLR_OUTPUT_FW_UPDATE_PKT		0x03
#define NX_CTLR_OUTPUT_RUMBLE_ONLY		0x10
#define NX_CTLR_OUTPUT_MCU_DATA			0x11
#define NX_CTLR_OUTPUT_USB_CMD			0x80

/* Subcommand IDs */
#define NX_CTLR_SUBCMD_STATE			0x00
#define NX_CTLR_SUBCMD_MANUAL_BT_PAIRING	0x01
#define NX_CTLR_SUBCMD_REQ_DEV_INFO		0x02
#define NX_CTLR_SUBCMD_SET_REPORT_MODE		0x03
#define NX_CTLR_SUBCMD_TRIGGERS_ELAPSED		0x04
#define NX_CTLR_SUBCMD_GET_PAGE_LIST_STATE	0x05
#define NX_CTLR_SUBCMD_SET_HCI_STATE		0x06
#define NX_CTLR_SUBCMD_RESET_PAIRING_INFO	0x07
#define NX_CTLR_SUBCMD_LOW_POWER_MODE		0x08
#define NX_CTLR_SUBCMD_SPI_FLASH_READ		0x10
#define NX_CTLR_SUBCMD_SPI_FLASH_WRITE		0x11
#define NX_CTLR_SUBCMD_RESET_MCU		0x20
#define NX_CTLR_SUBCMD_SET_MCU_CONFIG		0x21
#define NX_CTLR_SUBCMD_SET_MCU_STATE		0x22
#define NX_CTLR_SUBCMD_SET_PLAYER_LIGHTS	0x30
#define NX_CTLR_SUBCMD_GET_PLAYER_LIGHTS	0x31
#define NX_CTLR_SUBCMD_SET_HOME_LIGHT		0x38
#define NX_CTLR_SUBCMD_ENABLE_IMU		0x40
#define NX_CTLR_SUBCMD_SET_IMU_SENSITIVITY	0x41
#define NX_CTLR_SUBCMD_WRITE_IMU_REG		0x42
#define NX_CTLR_SUBCMD_READ_IMU_REG		0x43
#define NX_CTLR_SUBCMD_ENABLE_VIBRATION		0x48
#define NX_CTLR_SUBCMD_GET_REGULATED_VOLTAGE	0x50

/* Input Reports */
#define NX_CTLR_INPUT_BUTTON_EVENT		0x3F
#define NX_CTLR_INPUT_SUBCMD_REPLY		0x21
#define NX_CTLR_INPUT_IMU_DATA			0x30
#define NX_CTLR_INPUT_MCU_DATA			0x31
#define NX_CTLR_INPUT_USB_RESPONSE		0x81

/* Feature Reports */
#define NX_CTLR_FEATURE_LAST_SUBCMD		0x02
#define NX_CTLR_FEATURE_OTA_FW_UPGRADE		0x70
#define NX_CTLR_FEATURE_SETUP_MEM_READ		0x71
#define NX_CTLR_FEATURE_MEM_READ		0x72
#define NX_CTLR_FEATURE_ERASE_MEM_SECTOR	0x73
#define NX_CTLR_FEATURE_MEM_WRITE		0x74
#define NX_CTLR_FEATURE_LAUNCH			0x75

/* USB Commands */
#define NX_CTLR_USB_CMD_CONN_STATUS		0x01
#define NX_CTLR_USB_CMD_HANDSHAKE		0x02
#define NX_CTLR_USB_CMD_BAUDRATE_3M		0x03
#define NX_CTLR_USB_CMD_NO_TIMEOUT		0x04
#define NX_CTLR_USB_CMD_EN_TIMEOUT		0x05
#define NX_CTLR_USB_RESET			0x06
#define NX_CTLR_USB_PRE_HANDSHAKE		0x91
#define NX_CTLR_USB_SEND_UART			0x92

/* Magic value denoting presence of user calibration */
#define NX_CTLR_CAL_USR_MAGIC_0			0xB2
#define NX_CTLR_CAL_USR_MAGIC_1			0xA1
#define NX_CTLR_CAL_USR_MAGIC_SIZE		2

/* SPI storage addresses of user calibration data */
#define NX_CTLR_CAL_USR_LEFT_MAGIC_ADDR		0x8010
#define NX_CTLR_CAL_USR_LEFT_DATA_ADDR		0x8012
#define NX_CTLR_CAL_USR_LEFT_DATA_END		0x801A
#define NX_CTLR_CAL_USR_RIGHT_MAGIC_ADDR	0x801B
#define NX_CTLR_CAL_USR_RIGHT_DATA_ADDR		0x801D
#define NX_CTLR_CAL_STICK_DATA_SIZE \
	(NX_CTLR_CAL_USR_LEFT_DATA_END - NX_CTLR_CAL_USR_LEFT_DATA_ADDR + 1)

/* SPI storage addresses of factory calibration data */
#define NX_CTLR_CAL_FCT_DATA_LEFT_ADDR		0x603d
#define NX_CTLR_CAL_FCT_DATA_RIGHT_ADDR		0x6046

/* SPI storage addresses of IMU factory calibration data */
#define NX_CTLR_IMU_CAL_FCT_DATA_ADDR		0x6020
#define NX_CTLR_IMU_CAL_FCT_DATA_END		0x6037
#define NX_CTLR_IMU_CAL_DATA_SIZE \
	(NX_CTLR_IMU_CAL_FCT_DATA_END - NX_CTLR_IMU_CAL_FCT_DATA_ADDR + 1)
/* SPI storage addresses of IMU user calibration data */
#define NX_CTLR_IMU_CAL_USR_MAGIC_ADDR		0x8026
#define NX_CTLR_IMU_CAL_USR_DATA_ADDR		0x8028

/* The raw analog joystick values will be mapped in terms of this magnitude */
#define NX_CTLR_MAX_STICK_MAG			32767
#define NX_CTLR_STICK_FUZZ			250
#define NX_CTLR_STICK_FLAT			500

/* Hat values for pro controller's d-pad */
#define NX_CTLR_MAX_DPAD_MAG			1
#define NX_CTLR_DPAD_FUZZ			0
#define NX_CTLR_DPAD_FLAT			0

/* Under most circumstances IMU reports are pushed every 15ms; use as default */
#define NX_CTLR_IMU_DFLT_AVG_DELTA_MS		15
/* How many samples to sum before calculating average IMU report delta */
#define NX_CTLR_IMU_SAMPLES_PER_DELTA_AVG	300
/* Controls how many dropped IMU packets at once trigger a warning message */
#define NX_CTLR_IMU_DROPPED_PKT_WARNING		3

/*
 * The controller's accelerometer has a sensor resolution of 16bits and is
 * configured with a range of +-8000 milliGs. Therefore, the resolution can be
 * calculated thus: (2^16-1)/(8000 * 2) = 4.096 digits per milliG
 * Resolution per G (rather than per millliG): 4.096 * 1000 = 4096 digits per G
 * Alternatively: 1/4096 = .0002441 Gs per digit
 */
#define NX_CTLR_IMU_MAX_ACCEL_MAG		32767
#define NX_CTLR_IMU_ACCEL_RES_PER_G		4096
#define NX_CTLR_IMU_ACCEL_FUZZ			10
#define NX_CTLR_IMU_ACCEL_FLAT			0

/*
 * The controller's gyroscope has a sensor resolution of 16bits and is
 * configured with a range of +-2000 degrees/second.
 * Digits per dps: (2^16 -1)/(2000*2) = 16.38375
 * dps per digit: 16.38375E-1 = .0610
 *
 * STMicro recommends in the datasheet to add 15% to the dps/digit. This allows
 * the full sensitivity range to be saturated without clipping. This yields more
 * accurate results, so it's the technique this driver uses.
 * dps per digit (corrected): .0610 * 1.15 = .0702
 * digits per dps (corrected): .0702E-1 = 14.247
 *
 * Now, 14.247 truncating to 14 loses a lot of precision, so we rescale the
 * min/max range by 1000.
 */
#define NX_CTLR_IMU_PREC_RANGE_SCALE		1000
/* Note: change mag and res_per_dps if prec_range_scale is ever altered */
#define NX_CTLR_IMU_MAX_GYRO_MAG		32767000 /* (2^16-1)*1000 */
#define NX_CTLR_IMU_GYRO_RES_PER_DPS		14247 /* (14.247*1000) */
#define NX_CTLR_IMU_GYRO_FUZZ			10
#define NX_CTLR_IMU_GYRO_FLAT			0

/* frequency/amplitude tables for rumble */
struct nx_ctlr_rumble_freq_data {
	u16 high;
	u8 low;
	u16 freq; /* Hz*/
};

struct nx_ctlr_rumble_amp_data {
	u8 high;
	u16 low;
	u16 amp;
};

#if IS_ENABLED(CONFIG_NINTENDO_FF)
/*
 * These tables are from
 * https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/rumble_data_table.md
 */
static const struct nx_ctlr_rumble_freq_data nx_ctlr_rumble_frequencies[] = {
	/* high, low, freq */
	{ 0x0000, 0x01,   41 }, { 0x0000, 0x02,   42 }, { 0x0000, 0x03,   43 },
	{ 0x0000, 0x04,   44 }, { 0x0000, 0x05,   45 }, { 0x0000, 0x06,   46 },
	{ 0x0000, 0x07,   47 }, { 0x0000, 0x08,   48 }, { 0x0000, 0x09,   49 },
	{ 0x0000, 0x0A,   50 }, { 0x0000, 0x0B,   51 }, { 0x0000, 0x0C,   52 },
	{ 0x0000, 0x0D,   53 }, { 0x0000, 0x0E,   54 }, { 0x0000, 0x0F,   55 },
	{ 0x0000, 0x10,   57 }, { 0x0000, 0x11,   58 }, { 0x0000, 0x12,   59 },
	{ 0x0000, 0x13,   60 }, { 0x0000, 0x14,   62 }, { 0x0000, 0x15,   63 },
	{ 0x0000, 0x16,   64 }, { 0x0000, 0x17,   66 }, { 0x0000, 0x18,   67 },
	{ 0x0000, 0x19,   69 }, { 0x0000, 0x1A,   70 }, { 0x0000, 0x1B,   72 },
	{ 0x0000, 0x1C,   73 }, { 0x0000, 0x1D,   75 }, { 0x0000, 0x1e,   77 },
	{ 0x0000, 0x1f,   78 }, { 0x0000, 0x20,   80 }, { 0x0400, 0x21,   82 },
	{ 0x0800, 0x22,   84 }, { 0x0c00, 0x23,   85 }, { 0x1000, 0x24,   87 },
	{ 0x1400, 0x25,   89 }, { 0x1800, 0x26,   91 }, { 0x1c00, 0x27,   93 },
	{ 0x2000, 0x28,   95 }, { 0x2400, 0x29,   97 }, { 0x2800, 0x2a,   99 },
	{ 0x2c00, 0x2b,  102 }, { 0x3000, 0x2c,  104 }, { 0x3400, 0x2d,  106 },
	{ 0x3800, 0x2e,  108 }, { 0x3c00, 0x2f,  111 }, { 0x4000, 0x30,  113 },
	{ 0x4400, 0x31,  116 }, { 0x4800, 0x32,  118 }, { 0x4c00, 0x33,  121 },
	{ 0x5000, 0x34,  123 }, { 0x5400, 0x35,  126 }, { 0x5800, 0x36,  129 },
	{ 0x5c00, 0x37,  132 }, { 0x6000, 0x38,  135 }, { 0x6400, 0x39,  137 },
	{ 0x6800, 0x3a,  141 }, { 0x6c00, 0x3b,  144 }, { 0x7000, 0x3c,  147 },
	{ 0x7400, 0x3d,  150 }, { 0x7800, 0x3e,  153 }, { 0x7c00, 0x3f,  157 },
	{ 0x8000, 0x40,  160 }, { 0x8400, 0x41,  164 }, { 0x8800, 0x42,  167 },
	{ 0x8c00, 0x43,  171 }, { 0x9000, 0x44,  174 }, { 0x9400, 0x45,  178 },
	{ 0x9800, 0x46,  182 }, { 0x9c00, 0x47,  186 }, { 0xa000, 0x48,  190 },
	{ 0xa400, 0x49,  194 }, { 0xa800, 0x4a,  199 }, { 0xac00, 0x4b,  203 },
	{ 0xb000, 0x4c,  207 }, { 0xb400, 0x4d,  212 }, { 0xb800, 0x4e,  217 },
	{ 0xbc00, 0x4f,  221 }, { 0xc000, 0x50,  226 }, { 0xc400, 0x51,  231 },
	{ 0xc800, 0x52,  236 }, { 0xcc00, 0x53,  241 }, { 0xd000, 0x54,  247 },
	{ 0xd400, 0x55,  252 }, { 0xd800, 0x56,  258 }, { 0xdc00, 0x57,  263 },
	{ 0xe000, 0x58,  269 }, { 0xe400, 0x59,  275 }, { 0xe800, 0x5a,  281 },
	{ 0xec00, 0x5b,  287 }, { 0xf000, 0x5c,  293 }, { 0xf400, 0x5d,  300 },
	{ 0xf800, 0x5e,  306 }, { 0xfc00, 0x5f,  313 }, { 0x0001, 0x60,  320 },
	{ 0x0401, 0x61,  327 }, { 0x0801, 0x62,  334 }, { 0x0c01, 0x63,  341 },
	{ 0x1001, 0x64,  349 }, { 0x1401, 0x65,  357 }, { 0x1801, 0x66,  364 },
	{ 0x1c01, 0x67,  372 }, { 0x2001, 0x68,  381 }, { 0x2401, 0x69,  389 },
	{ 0x2801, 0x6a,  397 }, { 0x2c01, 0x6b,  406 }, { 0x3001, 0x6c,  415 },
	{ 0x3401, 0x6d,  424 }, { 0x3801, 0x6e,  433 }, { 0x3c01, 0x6f,  443 },
	{ 0x4001, 0x70,  453 }, { 0x4401, 0x71,  462 }, { 0x4801, 0x72,  473 },
	{ 0x4c01, 0x73,  483 }, { 0x5001, 0x74,  494 }, { 0x5401, 0x75,  504 },
	{ 0x5801, 0x76,  515 }, { 0x5c01, 0x77,  527 }, { 0x6001, 0x78,  538 },
	{ 0x6401, 0x79,  550 }, { 0x6801, 0x7a,  562 }, { 0x6c01, 0x7b,  574 },
	{ 0x7001, 0x7c,  587 }, { 0x7401, 0x7d,  600 }, { 0x7801, 0x7e,  613 },
	{ 0x7c01, 0x7f,  626 }, { 0x8001, 0x00,  640 }, { 0x8401, 0x00,  654 },
	{ 0x8801, 0x00,  668 }, { 0x8c01, 0x00,  683 }, { 0x9001, 0x00,  698 },
	{ 0x9401, 0x00,  713 }, { 0x9801, 0x00,  729 }, { 0x9c01, 0x00,  745 },
	{ 0xa001, 0x00,  761 }, { 0xa401, 0x00,  778 }, { 0xa801, 0x00,  795 },
	{ 0xac01, 0x00,  812 }, { 0xb001, 0x00,  830 }, { 0xb401, 0x00,  848 },
	{ 0xb801, 0x00,  867 }, { 0xbc01, 0x00,  886 }, { 0xc001, 0x00,  905 },
	{ 0xc401, 0x00,  925 }, { 0xc801, 0x00,  945 }, { 0xcc01, 0x00,  966 },
	{ 0xd001, 0x00,  987 }, { 0xd401, 0x00, 1009 }, { 0xd801, 0x00, 1031 },
	{ 0xdc01, 0x00, 1053 }, { 0xe001, 0x00, 1076 }, { 0xe401, 0x00, 1100 },
	{ 0xe801, 0x00, 1124 }, { 0xec01, 0x00, 1149 }, { 0xf001, 0x00, 1174 },
	{ 0xf401, 0x00, 1199 }, { 0xf801, 0x00, 1226 }, { 0xfc01, 0x00, 1253 }
};

#define nx_ctlr_max_rumble_amp	(1003)
static const struct nx_ctlr_rumble_amp_data nx_ctlr_rumble_amplitudes[] = {
	/* high, low, amp */
	{ 0x00, 0x0040,    0 },
	{ 0x02, 0x8040,   10 }, { 0x04, 0x0041,   12 }, { 0x06, 0x8041,   14 },
	{ 0x08, 0x0042,   17 }, { 0x0a, 0x8042,   20 }, { 0x0c, 0x0043,   24 },
	{ 0x0e, 0x8043,   28 }, { 0x10, 0x0044,   33 }, { 0x12, 0x8044,   40 },
	{ 0x14, 0x0045,   47 }, { 0x16, 0x8045,   56 }, { 0x18, 0x0046,   67 },
	{ 0x1a, 0x8046,   80 }, { 0x1c, 0x0047,   95 }, { 0x1e, 0x8047,  112 },
	{ 0x20, 0x0048,  117 }, { 0x22, 0x8048,  123 }, { 0x24, 0x0049,  128 },
	{ 0x26, 0x8049,  134 }, { 0x28, 0x004a,  140 }, { 0x2a, 0x804a,  146 },
	{ 0x2c, 0x004b,  152 }, { 0x2e, 0x804b,  159 }, { 0x30, 0x004c,  166 },
	{ 0x32, 0x804c,  173 }, { 0x34, 0x004d,  181 }, { 0x36, 0x804d,  189 },
	{ 0x38, 0x004e,  198 }, { 0x3a, 0x804e,  206 }, { 0x3c, 0x004f,  215 },
	{ 0x3e, 0x804f,  225 }, { 0x40, 0x0050,  230 }, { 0x42, 0x8050,  235 },
	{ 0x44, 0x0051,  240 }, { 0x46, 0x8051,  245 }, { 0x48, 0x0052,  251 },
	{ 0x4a, 0x8052,  256 }, { 0x4c, 0x0053,  262 }, { 0x4e, 0x8053,  268 },
	{ 0x50, 0x0054,  273 }, { 0x52, 0x8054,  279 }, { 0x54, 0x0055,  286 },
	{ 0x56, 0x8055,  292 }, { 0x58, 0x0056,  298 }, { 0x5a, 0x8056,  305 },
	{ 0x5c, 0x0057,  311 }, { 0x5e, 0x8057,  318 }, { 0x60, 0x0058,  325 },
	{ 0x62, 0x8058,  332 }, { 0x64, 0x0059,  340 }, { 0x66, 0x8059,  347 },
	{ 0x68, 0x005a,  355 }, { 0x6a, 0x805a,  362 }, { 0x6c, 0x005b,  370 },
	{ 0x6e, 0x805b,  378 }, { 0x70, 0x005c,  387 }, { 0x72, 0x805c,  395 },
	{ 0x74, 0x005d,  404 }, { 0x76, 0x805d,  413 }, { 0x78, 0x005e,  422 },
	{ 0x7a, 0x805e,  431 }, { 0x7c, 0x005f,  440 }, { 0x7e, 0x805f,  450 },
	{ 0x80, 0x0060,  460 }, { 0x82, 0x8060,  470 }, { 0x84, 0x0061,  480 },
	{ 0x86, 0x8061,  491 }, { 0x88, 0x0062,  501 }, { 0x8a, 0x8062,  512 },
	{ 0x8c, 0x0063,  524 }, { 0x8e, 0x8063,  535 }, { 0x90, 0x0064,  547 },
	{ 0x92, 0x8064,  559 }, { 0x94, 0x0065,  571 }, { 0x96, 0x8065,  584 },
	{ 0x98, 0x0066,  596 }, { 0x9a, 0x8066,  609 }, { 0x9c, 0x0067,  623 },
	{ 0x9e, 0x8067,  636 }, { 0xa0, 0x0068,  650 }, { 0xa2, 0x8068,  665 },
	{ 0xa4, 0x0069,  679 }, { 0xa6, 0x8069,  694 }, { 0xa8, 0x006a,  709 },
	{ 0xaa, 0x806a,  725 }, { 0xac, 0x006b,  741 }, { 0xae, 0x806b,  757 },
	{ 0xb0, 0x006c,  773 }, { 0xb2, 0x806c,  790 }, { 0xb4, 0x006d,  808 },
	{ 0xb6, 0x806d,  825 }, { 0xb8, 0x006e,  843 }, { 0xba, 0x806e,  862 },
	{ 0xbc, 0x006f,  881 }, { 0xbe, 0x806f,  900 }, { 0xc0, 0x0070,  920 },
	{ 0xc2, 0x8070,  940 }, { 0xc4, 0x0071,  960 }, { 0xc6, 0x8071,  981 },
	{ 0xc8, 0x0072, nx_ctlr_max_rumble_amp }
};
static const u16 NX_CTLR_RUMBLE_DFLT_LOW_FREQ 	= 160;
static const u16 NX_CTLR_RUMBLE_DFLT_HIGH_FREQ	= 320;
#endif /* IS_ENABLED(CONFIG_NINTENDO_FF) */
static const u16 NX_CTLR_RUMBLE_PERIOD_MS	= 50;

/* States for controller state machine */
enum nx_ctlr_state {
	NX_CTLR_STATE_INIT,
	NX_CTLR_STATE_READ,
	NX_CTLR_STATE_REMOVED,
};

/* Controller type received as part of device info */
enum nx_ctlr_type {
	NX_CTLR_TYPE_JCL	= 0x01,
	NX_CTLR_TYPE_JCR	= 0x02,
	NX_CTLR_TYPE_PRO	= 0x03,
	NX_CTLR_TYPE_NESL	= 0x09,
	NX_CTLR_TYPE_NESR	= 0x0A,
	NX_CTLR_TYPE_SNES	= 0x0B,
	NX_CTLR_TYPE_GEN	= 0x0D,
};

struct nx_ctlr_stick_cal {
	s32 max;
	s32 min;
	s32 center;
};

struct nx_ctlr_imu_cal {
	s16 offset[3];
	s16 scale[3];
};

/*
 * All the controller's button values are stored in a u32.
 * They can be accessed with bitwise ANDs.
 */
static const u32 NX_CTLR_BTN_Y		= BIT(0);
static const u32 NX_CTLR_BTN_X		= BIT(1);
static const u32 NX_CTLR_BTN_B		= BIT(2);
static const u32 NX_CTLR_BTN_A		= BIT(3);
static const u32 NX_CTLR_BTN_SR_R	= BIT(4);
static const u32 NX_CTLR_BTN_SL_R	= BIT(5);
static const u32 NX_CTLR_BTN_R		= BIT(6);
static const u32 NX_CTLR_BTN_ZR		= BIT(7);
static const u32 NX_CTLR_BTN_MINUS	= BIT(8);
static const u32 NX_CTLR_BTN_PLUS	= BIT(9);
static const u32 NX_CTLR_BTN_RSTICK	= BIT(10);
static const u32 NX_CTLR_BTN_LSTICK	= BIT(11);
static const u32 NX_CTLR_BTN_HOME	= BIT(12);
static const u32 NX_CTLR_BTN_CAP	= BIT(13); /* capture button */
static const u32 NX_CTLR_BTN_DOWN	= BIT(16);
static const u32 NX_CTLR_BTN_UP		= BIT(17);
static const u32 NX_CTLR_BTN_RIGHT	= BIT(18);
static const u32 NX_CTLR_BTN_LEFT	= BIT(19);
static const u32 NX_CTLR_BTN_SR_L	= BIT(20);
static const u32 NX_CTLR_BTN_SL_L	= BIT(21);
static const u32 NX_CTLR_BTN_L		= BIT(22);
static const u32 NX_CTLR_BTN_ZL		= BIT(23);

enum nx_ctlr_msg_type {
	NX_CTLR_MSG_TYPE_NONE,
	NX_CTLR_MSG_TYPE_USB,
	NX_CTLR_MSG_TYPE_SUBCMD,
};

struct nx_ctlr_rumble_output {
	u8 output_id;
	u8 packet_num;
	u8 rumble_data[8];
} __packed;

struct nx_ctlr_subcmd_request {
	u8 output_id; /* must be 0x01 for subcommand, 0x10 for rumble only */
	u8 packet_num; /* incremented every send */
	u8 rumble_data[8];
	u8 subcmd_id;
	u8 data[]; /* length depends on the subcommand */
} __packed;

struct nx_ctlr_subcmd_reply {
	u8 ack; /* MSB 1 for ACK, 0 for NACK */
	u8 id; /* id of requested subcmd */
	u8 data[]; /* will be at most 35 bytes */
} __packed;

struct nx_ctlr_imu_data {
	s16 accel_x;
	s16 accel_y;
	s16 accel_z;
	s16 gyro_x;
	s16 gyro_y;
	s16 gyro_z;
} __packed;

struct nx_ctlr_input_report {
	u8 id;
	u8 timer;
	u8 bat_con; /* battery and connection info */
	u8 button_status[3];
	u8 left_stick[3];
	u8 right_stick[3];
	u8 vibrator_report;

	union {
		struct nx_ctlr_subcmd_reply subcmd_reply;
		/* IMU input reports contain 3 samples */
		u8 imu_raw_bytes[sizeof(struct nx_ctlr_imu_data) * 3];
	};
} __packed;

#define NX_CTLR_MAX_RESP_SIZE		(sizeof(struct nx_ctlr_input_report) + 35)
#define NX_CTLR_RUMBLE_DATA_SIZE	8
#define NX_CTLR_RUMBLE_QUEUE_SIZE	8

static const unsigned short NX_CTLR_RUMBLE_ZERO_AMP_PKT_CNT = 5;

static const char * const nx_ctlr_player_led_names[] = {
	LED_FUNCTION_PLAYER1,
	LED_FUNCTION_PLAYER2,
	LED_FUNCTION_PLAYER3,
	LED_FUNCTION_PLAYER4,
};

#define NX_CTLR_NUM_LEDS ARRAY_SIZE(nx_ctlr_player_led_names)

/* Each physical controller is associated with a nx_ctlr struct */
struct nx_ctlr {
	struct hid_device *hdev;
	struct input_dev *input;
	struct led_classdev leds[NX_CTLR_NUM_LEDS]; /* player leds */
	struct led_classdev home_led;
	enum nx_ctlr_state ctlr_state;
	spinlock_t lock;
	u8 mac_addr[6];
	char *mac_addr_str;
	enum nx_ctlr_type type;

	/* The following members are used for synchronous sends/receives */
	enum nx_ctlr_msg_type msg_type;
	u8 subcmd_num;
	struct mutex output_mutex;
	u8 input_buf[NX_CTLR_MAX_RESP_SIZE];
	wait_queue_head_t wait;
	bool received_resp;
	u8 usb_ack_match;
	u8 subcmd_ack_match;
	bool received_input_report;
	unsigned int last_subcmd_sent_msecs;

	/* factory calibration data */
	struct nx_ctlr_stick_cal left_stick_cal_x;
	struct nx_ctlr_stick_cal left_stick_cal_y;
	struct nx_ctlr_stick_cal right_stick_cal_x;
	struct nx_ctlr_stick_cal right_stick_cal_y;

	struct nx_ctlr_imu_cal accel_cal;
	struct nx_ctlr_imu_cal gyro_cal;

	/* prevents needlessly recalculating these divisors every sample */
	s32 imu_cal_accel_divisor[3];
	s32 imu_cal_gyro_divisor[3];

	/* power supply data */
	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	u8 battery_capacity;
	bool battery_charging;
	bool host_powered;

	/* rumble */
	u8 rumble_data[NX_CTLR_RUMBLE_QUEUE_SIZE][NX_CTLR_RUMBLE_DATA_SIZE];
	int rumble_queue_head;
	int rumble_queue_tail;
	struct workqueue_struct *rumble_queue;
	struct work_struct rumble_worker;
	unsigned int rumble_msecs;
	u16 rumble_ll_freq;
	u16 rumble_lh_freq;
	u16 rumble_rl_freq;
	u16 rumble_rh_freq;
	unsigned short rumble_zero_countdown;

	/* imu */
	struct input_dev *imu_input;
	bool imu_first_packet_received; /* helps in initiating timestamp */
	unsigned int imu_timestamp_us; /* timestamp we report to userspace */
	unsigned int imu_last_pkt_ms; /* used to calc imu report delta */
	/* the following are used to track the average imu report time delta */
	unsigned int imu_delta_samples_count;
	unsigned int imu_delta_samples_sum;
	unsigned int imu_avg_delta_ms;
};

/*
 * Controller device helpers
 *
 * These look at the device ID known to the HID subsystem to identify a device,
 * but take caution: some NSO devices lie about themselves (NES Joy-Cons and
 * Sega Genesis controller). See type helpers below.
 *
 * These helpers are most useful early during the HID probe or in conjunction
 * with the capability helpers below.
 */
static inline bool nx_ctlr_device_is_left_joycon(struct nx_ctlr *ctlr)
{
	return ctlr->hdev->product == USB_DEVICE_ID_NINTENDO_JOYCONL;
}

static inline bool nx_ctlr_device_is_right_joycon(struct nx_ctlr *ctlr)
{
	return ctlr->hdev->product == USB_DEVICE_ID_NINTENDO_JOYCONR;
}

static inline bool nx_ctlr_device_is_procon(struct nx_ctlr *ctlr)
{
	return ctlr->hdev->product == USB_DEVICE_ID_NINTENDO_PROCON;
}

static inline bool nx_ctlr_device_is_chrggrip(struct nx_ctlr *ctlr)
{
	return ctlr->hdev->product == USB_DEVICE_ID_NINTENDO_CHRGGRIP;
}

static inline bool nx_ctlr_device_is_snescon(struct nx_ctlr *ctlr)
{
	return ctlr->hdev->product == USB_DEVICE_ID_NINTENDO_SNESCON;
}

static inline bool nx_ctlr_device_is_gencon(struct nx_ctlr *ctlr)
{
	return ctlr->hdev->product == USB_DEVICE_ID_NINTENDO_GENCON;
}

static inline bool nx_ctlr_device_is_any_joycon(struct nx_ctlr *ctlr)
{
	return nx_ctlr_device_is_right_joycon(ctlr) ||
	       nx_ctlr_device_is_left_joycon(ctlr) ||
	       nx_ctlr_device_is_chrggrip(ctlr);
}

static inline bool nx_ctlr_device_has_usb(struct nx_ctlr *ctlr)
{
	return nx_ctlr_device_is_procon(ctlr) ||
	       nx_ctlr_device_is_chrggrip(ctlr) ||
	       nx_ctlr_device_is_snescon(ctlr) ||
	       nx_ctlr_device_is_gencon(ctlr);
}

/*
 * Controller type helpers
 *
 * These are slightly different than the device-ID-based helpers above. They are
 * generally more reliable, since they can distinguish between, e.g., Genesis
 * versus SNES, or NES Joy-Cons versus regular Switch Joy-Cons. They're most
 * useful for reporting available inputs. For other kinds of distinctions, see
 * the capability helpers below.
 *
 * They have two major drawbacks: (1) they're not available until after we set
 * the reporting method and then request the device info; (2) they can't
 * distinguish all controllers (like the Charging Grip from the Pro controller.)
 */
static inline bool nx_ctlr_type_is_left_joycon(struct nx_ctlr *ctlr)
{
	return ctlr->type == NX_CTLR_TYPE_JCL;
}

static inline bool nx_ctlr_type_is_right_joycon(struct nx_ctlr *ctlr)
{
	return ctlr->type == NX_CTLR_TYPE_JCR;
}

static inline bool nx_ctlr_type_is_procon(struct nx_ctlr *ctlr)
{
	return ctlr->type == NX_CTLR_TYPE_PRO;
}

static inline bool nx_ctlr_type_is_snescon(struct nx_ctlr *ctlr)
{
	return ctlr->type == NX_CTLR_TYPE_SNES;
}

static inline bool nx_ctlr_type_is_gencon(struct nx_ctlr *ctlr)
{
	return ctlr->type == NX_CTLR_TYPE_GEN;
}

static inline bool nx_ctlr_type_is_left_nescon(struct nx_ctlr *ctlr)
{
	return ctlr->type == NX_CTLR_TYPE_NESL;
}

static inline bool nx_ctlr_type_is_right_nescon(struct nx_ctlr *ctlr)
{
	return ctlr->type == NX_CTLR_TYPE_NESR;
}

static inline bool nx_ctlr_type_has_left_controls(struct nx_ctlr *ctlr)
{
	return nx_ctlr_type_is_left_joycon(ctlr) ||
	       nx_ctlr_type_is_procon(ctlr);
}

static inline bool nx_ctlr_type_has_right_controls(struct nx_ctlr *ctlr)
{
	return nx_ctlr_type_is_right_joycon(ctlr) ||
	       nx_ctlr_type_is_procon(ctlr);
}

static inline bool nx_ctlr_type_is_any_joycon(struct nx_ctlr *ctlr)
{
	return nx_ctlr_type_is_left_joycon(ctlr) ||
	       nx_ctlr_type_is_right_joycon(ctlr) ||
	       nx_ctlr_device_is_chrggrip(ctlr);
}

static inline bool nx_ctlr_type_is_any_nescon(struct nx_ctlr *ctlr)
{
	return nx_ctlr_type_is_left_nescon(ctlr) ||
	       nx_ctlr_type_is_right_nescon(ctlr);
}

/*
 * Controller capability helpers
 *
 * These helpers combine the use of the helpers above to detect certain
 * capabilities during initialization. They are always accurate but (since they
 * use type helpers) cannot be used early in the HID probe.
 */
static inline bool nx_ctlr_has_imu(struct nx_ctlr *ctlr)
{
	return nx_ctlr_device_is_chrggrip(ctlr) ||
	       nx_ctlr_type_is_any_joycon(ctlr) ||
	       nx_ctlr_type_is_procon(ctlr);
}

static inline bool nx_ctlr_has_joysticks(struct nx_ctlr *ctlr)
{
	return nx_ctlr_device_is_chrggrip(ctlr) ||
	       nx_ctlr_type_is_any_joycon(ctlr) ||
	       nx_ctlr_type_is_procon(ctlr);
}

static inline bool nx_ctlr_has_rumble(struct nx_ctlr *ctlr)
{
	return nx_ctlr_device_is_chrggrip(ctlr) ||
	       nx_ctlr_type_is_any_joycon(ctlr) ||
	       nx_ctlr_type_is_procon(ctlr);
}

static int __nx_ctlr_hid_send(struct hid_device *hdev, u8 *data, size_t len)
{
	u8 *buf;
	int ret;

	if (!(buf = kmemdup(data, len, GFP_KERNEL)))
		return -ENOMEM;

	if ((ret = hid_hw_output_report(hdev, buf, len)) < 0)
		hid_dbg(hdev, "Failed to send output report ret=%d\n", ret);

	kfree(buf);
	return ret;
}

static void nx_ctlr_wait_for_input_report(struct nx_ctlr *ctlr)
{
	/*
	 * If we are in the proper reporting mode, wait for an input
	 * report prior to sending the subcommand. This improves
	 * reliability considerably.
	 */
	if (ctlr->ctlr_state == NX_CTLR_STATE_READ) {
		unsigned long flags;

		spin_lock_irqsave(&ctlr->lock, flags);
		ctlr->received_input_report = false;
		spin_unlock_irqrestore(&ctlr->lock, flags);

		/* We will still proceed, even with a timeout here */
		if (!wait_event_timeout(ctlr->wait, ctlr->received_input_report, HZ / 4))
			hid_warn(ctlr->hdev, "timeout waiting for input report\n");
	}
}

/*
 * Sending subcommands and/or rumble data at too high a rate can cause bluetooth
 * controller disconnections.
 */
static void nx_ctlr_enforce_subcmd_rate(struct nx_ctlr *ctlr)
{
	static const unsigned int max_subcmd_rate_ms = 25;
	unsigned int current_ms = jiffies_to_msecs(jiffies);
	unsigned int delta_ms = current_ms - ctlr->last_subcmd_sent_msecs;

	while (delta_ms < max_subcmd_rate_ms &&
	       ctlr->ctlr_state == NX_CTLR_STATE_READ) {
		nx_ctlr_wait_for_input_report(ctlr);
		current_ms = jiffies_to_msecs(jiffies);
		delta_ms = current_ms - ctlr->last_subcmd_sent_msecs;
	}
	ctlr->last_subcmd_sent_msecs = current_ms;
}

static int nx_ctlr_hid_send_sync(struct nx_ctlr *ctlr, u8 *data, size_t len, u32 timeout)
{
	int ret;
	int tries = 2;

	/*
	 * The controller occasionally seems to drop subcommands. In testing,
	 * doing one retry after a timeout appears to always work.
	 */
	while (tries--) {
		nx_ctlr_enforce_subcmd_rate(ctlr);

		if ((ret = __nx_ctlr_hid_send(ctlr->hdev, data, len)) < 0) {
			memset(ctlr->input_buf, 0, NX_CTLR_MAX_RESP_SIZE);
			return ret;
		}

		if (!wait_event_timeout(ctlr->wait, ctlr->received_resp, timeout)) {
			hid_dbg(ctlr->hdev,
				"synchronous send/receive timed out\n");
			if (tries) {
				hid_dbg(ctlr->hdev, "retrying sync send after timeout\n");
			}
			memset(ctlr->input_buf, 0, NX_CTLR_MAX_RESP_SIZE);
			ret = -ETIMEDOUT;
		} else {
			ret = 0;
			break;
		}
	}

	ctlr->received_resp = false;
	return ret;
}

static int nx_ctlr_send_usb(struct nx_ctlr *ctlr, u8 cmd, u32 timeout)
{
	int ret;
	u8 buf[2] = {NX_CTLR_OUTPUT_USB_CMD};

	buf[1] = cmd;
	ctlr->usb_ack_match = cmd;
	ctlr->msg_type = NX_CTLR_MSG_TYPE_USB;
	if ((ret = nx_ctlr_hid_send_sync(ctlr, buf, sizeof(buf), timeout)))
		hid_dbg(ctlr->hdev, "send usb command failed; ret=%d\n", ret);
	return ret;
}

static int nx_ctlr_send_subcmd(struct nx_ctlr *ctlr,
			       struct nx_ctlr_subcmd_request *subcmd,
			       size_t data_len, u32 timeout)
{
	int ret;
	unsigned long flags;

	spin_lock_irqsave(&ctlr->lock, flags);
	/*
	 * If the controller has been removed, just return ENODEV so the LED
	 * subsystem doesn't print invalid errors on removal.
	 */
	if (ctlr->ctlr_state == NX_CTLR_STATE_REMOVED) {
		spin_unlock_irqrestore(&ctlr->lock, flags);
		return -ENODEV;
	}
	memcpy(subcmd->rumble_data, ctlr->rumble_data[ctlr->rumble_queue_tail],
	       NX_CTLR_RUMBLE_DATA_SIZE);
	spin_unlock_irqrestore(&ctlr->lock, flags);

	subcmd->output_id = NX_CTLR_OUTPUT_RUMBLE_AND_SUBCMD;
	subcmd->packet_num = ctlr->subcmd_num;
	if (++ctlr->subcmd_num > 0xF)
		ctlr->subcmd_num = 0;
	ctlr->subcmd_ack_match = subcmd->subcmd_id;
	ctlr->msg_type = NX_CTLR_MSG_TYPE_SUBCMD;

	if ((ret = nx_ctlr_hid_send_sync(ctlr, (u8 *)subcmd,
					 sizeof(*subcmd) + data_len,
					 timeout)) < 0)
		hid_dbg(ctlr->hdev, "send subcommand failed; ret=%d\n", ret);
	else
		ret = 0;
	return ret;
}

/* Supply nibbles for flash and on. Ones correspond to active */
static int nx_ctlr_set_player_leds(struct nx_ctlr *ctlr, u8 flash, u8 on)
{
	struct nx_ctlr_subcmd_request *req;
	u8 buffer[sizeof(*req) + 1] = { 0 };

	req = (struct nx_ctlr_subcmd_request *)buffer;
	req->subcmd_id = NX_CTLR_SUBCMD_SET_PLAYER_LIGHTS;
	req->data[0] = (flash << 4) | on;

	hid_dbg(ctlr->hdev, "setting player leds\n");
	return nx_ctlr_send_subcmd(ctlr, req, 1, HZ/4);
}

static int nx_ctlr_request_spi_flash_read(struct nx_ctlr *ctlr,
					  u32 start_addr,
					  u8 size,
					  u8 **reply)
{
	struct nx_ctlr_subcmd_request *req;
	struct nx_ctlr_input_report *report;
	u8 buffer[sizeof(*req) + 5] = { 0 };
	u8 *data;
	int ret;

	if (!reply)
		return -EINVAL;

	req = (struct nx_ctlr_subcmd_request *)buffer;
	req->subcmd_id = NX_CTLR_SUBCMD_SPI_FLASH_READ;
	data = req->data;
	put_unaligned_le32(start_addr, data);
	data[4] = size;

	hid_dbg(ctlr->hdev, "requesting SPI flash data\n");
	if ((ret = nx_ctlr_send_subcmd(ctlr, req, 5, HZ))) {
		hid_err(ctlr->hdev, "failed reading SPI flash; ret=%d\n", ret);
	} else {
		report = (struct nx_ctlr_input_report *)ctlr->input_buf;
		/* The read data starts at the 6th byte */
		*reply = &report->subcmd_reply.data[5];
	}
	return ret;
}

/*
 * User calibration's presence is denoted with a magic byte preceding it.
 * returns 0 if magic val is present, 1 if not present, < 0 on error
 */
static int nx_ctlr_check_for_cal_magic(struct nx_ctlr *ctlr, u32 flash_addr)
{
	int ret;
	u8 *reply;

	if ((ret = nx_ctlr_request_spi_flash_read(ctlr,
						  flash_addr,
						  NX_CTLR_CAL_USR_MAGIC_SIZE,
						  &reply)))
		return ret;

	return reply[0] != NX_CTLR_CAL_USR_MAGIC_0 ||
	       reply[1] != NX_CTLR_CAL_USR_MAGIC_1;
}

static int nx_ctlr_read_stick_calibration(struct nx_ctlr *ctlr,
					  u16 cal_addr,
					  struct nx_ctlr_stick_cal *cal_x,
					  struct nx_ctlr_stick_cal *cal_y,
					  bool left_stick)
{
	s32 x_max_above;
	s32 x_min_below;
	s32 y_max_above;
	s32 y_min_below;
	u8 *raw;
	int ret;

	if ((ret = nx_ctlr_request_spi_flash_read(ctlr,
						  cal_addr,
						  NX_CTLR_CAL_STICK_DATA_SIZE,
						  &raw)))
		return ret;

	/* stick calibration parsing: note the order differs based on stick */
	if (left_stick) {
		x_max_above = hid_field_extract(ctlr->hdev, (raw + 0), 0, 12);
		y_max_above = hid_field_extract(ctlr->hdev, (raw + 1), 4, 12);
		cal_x->center = hid_field_extract(ctlr->hdev, (raw + 3), 0, 12);
		cal_y->center = hid_field_extract(ctlr->hdev, (raw + 4), 4, 12);
		x_min_below = hid_field_extract(ctlr->hdev, (raw + 6), 0, 12);
		y_min_below = hid_field_extract(ctlr->hdev, (raw + 7), 4, 12);
	} else {
		cal_x->center = hid_field_extract(ctlr->hdev, (raw + 0), 0, 12);
		cal_y->center = hid_field_extract(ctlr->hdev, (raw + 1), 4, 12);
		x_min_below = hid_field_extract(ctlr->hdev, (raw + 3), 0, 12);
		y_min_below = hid_field_extract(ctlr->hdev, (raw + 4), 4, 12);
		x_max_above = hid_field_extract(ctlr->hdev, (raw + 6), 0, 12);
		y_max_above = hid_field_extract(ctlr->hdev, (raw + 7), 4, 12);
	}

	cal_x->max = cal_x->center + x_max_above;
	cal_x->min = cal_x->center - x_min_below;
	cal_y->max = cal_y->center + y_max_above;
	cal_y->min = cal_y->center - y_min_below;

	return 0;
}

static const u16 DFLT_STICK_CAL_CEN = 2000;
static const u16 DFLT_STICK_CAL_MAX = 3500;
static const u16 DFLT_STICK_CAL_MIN = 500;

static int nx_ctlr_request_calibration(struct nx_ctlr *ctlr)
{
	u16 left_stick_addr = NX_CTLR_CAL_FCT_DATA_LEFT_ADDR;
	u16 right_stick_addr = NX_CTLR_CAL_FCT_DATA_RIGHT_ADDR;
	int ret;

	hid_dbg(ctlr->hdev, "requesting cal data\n");

	/* check if user stick calibrations are present */
	if (!nx_ctlr_check_for_cal_magic(ctlr, NX_CTLR_CAL_USR_LEFT_MAGIC_ADDR)) {
		left_stick_addr = NX_CTLR_CAL_USR_LEFT_DATA_ADDR;
		hid_info(ctlr->hdev, "using user cal for left stick\n");
	} else {
		hid_info(ctlr->hdev, "using factory cal for left stick\n");
	}
	if (!nx_ctlr_check_for_cal_magic(ctlr, NX_CTLR_CAL_USR_RIGHT_MAGIC_ADDR)) {
		right_stick_addr = NX_CTLR_CAL_USR_RIGHT_DATA_ADDR;
		hid_info(ctlr->hdev, "using user cal for right stick\n");
	} else {
		hid_info(ctlr->hdev, "using factory cal for right stick\n");
	}

	if ((ret = nx_ctlr_read_stick_calibration(ctlr, left_stick_addr,
					     	  &ctlr->left_stick_cal_x,
					     	  &ctlr->left_stick_cal_y,
					     	  true))) {
		hid_warn(ctlr->hdev,
			 "Failed to read left stick cal, using dflts; e=%d\n",
			 ret);

		ctlr->left_stick_cal_x.center = DFLT_STICK_CAL_CEN;
		ctlr->left_stick_cal_x.max = DFLT_STICK_CAL_MAX;
		ctlr->left_stick_cal_x.min = DFLT_STICK_CAL_MIN;

		ctlr->left_stick_cal_y.center = DFLT_STICK_CAL_CEN;
		ctlr->left_stick_cal_y.max = DFLT_STICK_CAL_MAX;
		ctlr->left_stick_cal_y.min = DFLT_STICK_CAL_MIN;
	}

	if ((ret = nx_ctlr_read_stick_calibration(ctlr, right_stick_addr,
						  &ctlr->right_stick_cal_x,
					          &ctlr->right_stick_cal_y,
					          false))) {
		hid_warn(ctlr->hdev,
			 "Failed to read right stick cal, using dflts; e=%d\n",
			 ret);

		ctlr->right_stick_cal_x.center = DFLT_STICK_CAL_CEN;
		ctlr->right_stick_cal_x.max = DFLT_STICK_CAL_MAX;
		ctlr->right_stick_cal_x.min = DFLT_STICK_CAL_MIN;

		ctlr->right_stick_cal_y.center = DFLT_STICK_CAL_CEN;
		ctlr->right_stick_cal_y.max = DFLT_STICK_CAL_MAX;
		ctlr->right_stick_cal_y.min = DFLT_STICK_CAL_MIN;
	}

	hid_dbg(ctlr->hdev, "calibration:\n"
			    "l_x_c=%d l_x_max=%d l_x_min=%d\n"
			    "l_y_c=%d l_y_max=%d l_y_min=%d\n"
			    "r_x_c=%d r_x_max=%d r_x_min=%d\n"
			    "r_y_c=%d r_y_max=%d r_y_min=%d\n",
			    ctlr->left_stick_cal_x.center,
			    ctlr->left_stick_cal_x.max,
			    ctlr->left_stick_cal_x.min,
			    ctlr->left_stick_cal_y.center,
			    ctlr->left_stick_cal_y.max,
			    ctlr->left_stick_cal_y.min,
			    ctlr->right_stick_cal_x.center,
			    ctlr->right_stick_cal_x.max,
			    ctlr->right_stick_cal_x.min,
			    ctlr->right_stick_cal_y.center,
			    ctlr->right_stick_cal_y.max,
			    ctlr->right_stick_cal_y.min);

	return 0;
}

/*
 * These divisors are calculated once rather than for each sample. They are only
 * dependent on the IMU calibration values. They are used when processing the
 * IMU input reports.
 */
static void nx_ctlr_calc_imu_cal_divisors(struct nx_ctlr *ctlr)
{
	int i;

	for (i = 0; i < 3; i++) {
		ctlr->imu_cal_accel_divisor[i] = ctlr->accel_cal.scale[i] -
						ctlr->accel_cal.offset[i];
		ctlr->imu_cal_gyro_divisor[i] = ctlr->gyro_cal.scale[i] -
						ctlr->gyro_cal.offset[i];
	}
}

static const s16 DFLT_ACCEL_OFFSET /*= 0*/;
static const s16 DFLT_ACCEL_SCALE = 16384;
static const s16 DFLT_GYRO_OFFSET /*= 0*/;
static const s16 DFLT_GYRO_SCALE  = 13371;

static int nx_ctlr_request_imu_calibration(struct nx_ctlr *ctlr)
{
	u16 imu_cal_addr = NX_CTLR_IMU_CAL_FCT_DATA_ADDR;
	u8 *raw_cal;
	int ret;
	int i;

	if (!nx_ctlr_check_for_cal_magic(ctlr, NX_CTLR_IMU_CAL_USR_MAGIC_ADDR)) {
		imu_cal_addr = NX_CTLR_IMU_CAL_USR_DATA_ADDR;
		hid_info(ctlr->hdev, "using user cal for IMU\n");
	} else {
		hid_info(ctlr->hdev, "using factory cal for IMU\n");
	}

	hid_dbg(ctlr->hdev, "requesting IMU cal data\n");
	if ((ret = nx_ctlr_request_spi_flash_read(ctlr,
						  imu_cal_addr,
						  NX_CTLR_IMU_CAL_DATA_SIZE,
						  &raw_cal))) {
		hid_warn(ctlr->hdev,
			 "Failed to read IMU cal, using defaults; ret=%d\n",
			 ret);

		for (i = 0; i < 3; i++) {
			ctlr->accel_cal.offset[i] = DFLT_ACCEL_OFFSET;
			ctlr->accel_cal.scale[i] = DFLT_ACCEL_SCALE;
			ctlr->gyro_cal.offset[i] = DFLT_GYRO_OFFSET;
			ctlr->gyro_cal.scale[i] = DFLT_GYRO_SCALE;
		}
		nx_ctlr_calc_imu_cal_divisors(ctlr);
		return ret;
	}

	/* IMU calibration parsing */
	for (i = 0; i < 3; i++) {
		int j = i * 2;

		ctlr->accel_cal.offset[i] = get_unaligned_le16(raw_cal + j);
		ctlr->accel_cal.scale[i] = get_unaligned_le16(raw_cal + j + 6);
		ctlr->gyro_cal.offset[i] = get_unaligned_le16(raw_cal + j + 12);
		ctlr->gyro_cal.scale[i] = get_unaligned_le16(raw_cal + j + 18);
	}

	nx_ctlr_calc_imu_cal_divisors(ctlr);

	hid_dbg(ctlr->hdev, "IMU calibration:\n"
			    "a_o[0]=%d a_o[1]=%d a_o[2]=%d\n"
			    "a_s[0]=%d a_s[1]=%d a_s[2]=%d\n"
			    "g_o[0]=%d g_o[1]=%d g_o[2]=%d\n"
			    "g_s[0]=%d g_s[1]=%d g_s[2]=%d\n",
			    ctlr->accel_cal.offset[0],
			    ctlr->accel_cal.offset[1],
			    ctlr->accel_cal.offset[2],
			    ctlr->accel_cal.scale[0],
			    ctlr->accel_cal.scale[1],
			    ctlr->accel_cal.scale[2],
			    ctlr->gyro_cal.offset[0],
			    ctlr->gyro_cal.offset[1],
			    ctlr->gyro_cal.offset[2],
			    ctlr->gyro_cal.scale[0],
			    ctlr->gyro_cal.scale[1],
			    ctlr->gyro_cal.scale[2]);

	return 0;
}

static int nx_ctlr_set_report_mode(struct nx_ctlr *ctlr)
{
	struct nx_ctlr_subcmd_request *req;
	u8 buffer[sizeof(*req) + 1] = { 0 };

	req = (struct nx_ctlr_subcmd_request *)buffer;
	req->subcmd_id = NX_CTLR_SUBCMD_SET_REPORT_MODE;
	req->data[0] = 0x30; /* standard, full report mode */

	hid_dbg(ctlr->hdev, "setting controller report mode\n");
	return nx_ctlr_send_subcmd(ctlr, req, 1, HZ);
}

static int nx_ctlr_enable_rumble(struct nx_ctlr *ctlr)
{
	struct nx_ctlr_subcmd_request *req;
	u8 buffer[sizeof(*req) + 1] = { 0 };

	req = (struct nx_ctlr_subcmd_request *)buffer;
	req->subcmd_id = NX_CTLR_SUBCMD_ENABLE_VIBRATION;
	req->data[0] = 0x01; /* note: 0x00 would disable */

	hid_dbg(ctlr->hdev, "enabling rumble\n");
	return nx_ctlr_send_subcmd(ctlr, req, 1, HZ/4);
}

static int nx_ctlr_enable_imu(struct nx_ctlr *ctlr)
{
	struct nx_ctlr_subcmd_request *req;
	u8 buffer[sizeof(*req) + 1] = { 0 };

	req = (struct nx_ctlr_subcmd_request *)buffer;
	req->subcmd_id = NX_CTLR_SUBCMD_ENABLE_IMU;
	req->data[0] = 0x01; /* note: 0x00 would disable */

	hid_dbg(ctlr->hdev, "enabling IMU\n");
	return nx_ctlr_send_subcmd(ctlr, req, 1, HZ);
}

static s32 nx_ctlr_map_stick_val(struct nx_ctlr_stick_cal *cal, s32 val)
{
	s32 center = cal->center;
	s32 min = cal->min;
	s32 max = cal->max;
	s32 new_val;

	if (val > center) {
		new_val = (val - center) * NX_CTLR_MAX_STICK_MAG;
		new_val /= (max - center);
	} else {
		new_val = (center - val) * -NX_CTLR_MAX_STICK_MAG;
		new_val /= (center - min);
	}
	new_val = clamp(new_val,
			(s32)-NX_CTLR_MAX_STICK_MAG,
			(s32)NX_CTLR_MAX_STICK_MAG);
	return new_val;
}

static void nx_ctlr_input_report_parse_imu_data(struct nx_ctlr *ctlr,
						struct nx_ctlr_input_report *rep,
						struct nx_ctlr_imu_data *imu_data)
{
	u8 *raw = rep->imu_raw_bytes;
	int i;

	for (i = 0; i < 3; i++) {
		struct nx_ctlr_imu_data *data = &imu_data[i];

		data->accel_x = get_unaligned_le16(raw + 0);
		data->accel_y = get_unaligned_le16(raw + 2);
		data->accel_z = get_unaligned_le16(raw + 4);
		data->gyro_x = get_unaligned_le16(raw + 6);
		data->gyro_y = get_unaligned_le16(raw + 8);
		data->gyro_z = get_unaligned_le16(raw + 10);
		/* point to next imu sample */
		raw += sizeof(struct nx_ctlr_imu_data);
	}
}

static void nx_ctlr_parse_imu_report(struct nx_ctlr *ctlr,
				     struct nx_ctlr_input_report *rep)
{
	struct nx_ctlr_imu_data imu_data[3] = {0}; /* 3 reports per packet */
	struct input_dev *idev = ctlr->imu_input;
	unsigned int msecs = jiffies_to_msecs(jiffies);
	unsigned int last_msecs = ctlr->imu_last_pkt_ms;
	int i;
	int value[6];

	nx_ctlr_input_report_parse_imu_data(ctlr, rep, imu_data);

	/*
	 * There are complexities surrounding how we determine the timestamps we
	 * associate with the samples we pass to userspace. The IMU input
	 * reports do not provide us with a good timestamp. There's a quickly
	 * incrementing 8-bit counter per input report, but it is not very
	 * useful for this purpose (it is not entirely clear what rate it
	 * increments at or if it varies based on packet push rate - more on
	 * the push rate below...).
	 *
	 * The reverse engineering work done on the joy-cons and pro controllers
	 * by the community seems to indicate the following:
	 * - The controller samples the IMU every 1.35ms. It then does some of
	 *   its own processing, probably averaging the samples out.
	 * - Each imu input report contains 3 IMU samples, (usually 5ms apart).
	 * - In the standard reporting mode (which this driver uses exclusively)
	 *   input reports are pushed from the controller as follows:
	 *      * joy-con (bluetooth): every 15 ms
	 *      * joy-cons (in charging grip via USB): every 15 ms
	 *      * pro controller (USB): every 15 ms
	 *      * pro controller (bluetooth): every 8 ms (this is the wildcard)
	 *
	 * Further complicating matters is that some bluetooth stacks are known
	 * to alter the controller's packet rate by hardcoding the bluetooth
	 * SSR for the switch controllers (android's stack currently sets the
	 * SSR to 11ms for both the joy-cons and pro controllers).
	 *
	 * In my own testing, I've discovered that my pro controller either
	 * reports IMU sample batches every 11ms or every 15ms. This rate is
	 * stable after connecting. It isn't 100% clear what determines this
	 * rate. Importantly, even when sending every 11ms, none of the samples
	 * are duplicates. This seems to indicate that the time deltas between
	 * reported samples can vary based on the input report rate.
	 *
	 * The solution employed in this driver is to keep track of the average
	 * time delta between IMU input reports. In testing, this value has
	 * proven to be stable, staying at 15ms or 11ms, though other hardware
	 * configurations and bluetooth stacks could potentially see other rates
	 * (hopefully this will become more clear as more people use the
	 * driver).
	 *
	 * Keeping track of the average report delta allows us to submit our
	 * timestamps to userspace based on that. Each report contains 3
	 * samples, so the IMU sampling rate should be avg_time_delta/3. We can
	 * also use this average to detect events where we have dropped a
	 * packet. The userspace timestamp for the samples will be adjusted
	 * accordingly to prevent unwanted behvaior.
	 */
	if (!ctlr->imu_first_packet_received) {
		ctlr->imu_timestamp_us = 0;
		ctlr->imu_delta_samples_count = 0;
		ctlr->imu_delta_samples_sum = 0;
		ctlr->imu_avg_delta_ms = NX_CTLR_IMU_DFLT_AVG_DELTA_MS;
		ctlr->imu_first_packet_received = true;
	} else {
		unsigned int delta = msecs - last_msecs;
		unsigned int dropped_pkts;
		unsigned int dropped_threshold;

		/* avg imu report delta housekeeping */
		ctlr->imu_delta_samples_sum += delta;
		ctlr->imu_delta_samples_count++;

		if (ctlr->imu_delta_samples_count >= NX_CTLR_IMU_SAMPLES_PER_DELTA_AVG) {
			ctlr->imu_avg_delta_ms = ctlr->imu_delta_samples_sum /
						 ctlr->imu_delta_samples_count;
			/* don't ever want divide by zero shenanigans */
			if (ctlr->imu_avg_delta_ms == 0) {
				ctlr->imu_avg_delta_ms = 1;
				hid_warn(ctlr->hdev, "calculated avg imu delta of 0\n");
			}
			ctlr->imu_delta_samples_count = 0;
			ctlr->imu_delta_samples_sum = 0;
		}

		/* useful for debugging IMU sample rate */
		hid_dbg(ctlr->hdev,
			"imu_report: ms=%u last_ms=%u delta=%u avg_delta=%u\n",
			msecs,
			last_msecs,
			delta,
			ctlr->imu_avg_delta_ms);

		/* check if any packets have been dropped */
		dropped_threshold = ctlr->imu_avg_delta_ms * 3 / 2;
		dropped_pkts = (delta - min(delta, dropped_threshold)) /
				ctlr->imu_avg_delta_ms;
		ctlr->imu_timestamp_us += 1000 * ctlr->imu_avg_delta_ms;
		if (dropped_pkts > NX_CTLR_IMU_DROPPED_PKT_WARNING) {
			hid_warn(ctlr->hdev,
				 "compensating for %u dropped IMU reports\n",
				 dropped_pkts);
			hid_warn(ctlr->hdev,
				 "delta=%u avg_delta=%u\n",
				 delta,
				 ctlr->imu_avg_delta_ms);
		}
	}
	ctlr->imu_last_pkt_ms = msecs;

	/* Each IMU input report contains three samples */
	for (i = 0; i < 3; i++) {
		input_event(idev, EV_MSC, MSC_TIMESTAMP, ctlr->imu_timestamp_us);

		/*
		 * These calculations (which use the controller's calibration
		 * settings to improve the final values) are based on those
		 * found in the community's reverse-engineering repo (linked at
		 * top of driver). For hid-nintendo, we make sure that the final
		 * value given to userspace is always in terms of the axis
		 * resolution we provided.
		 *
		 * Currently only the gyro calculations subtract the calibration
		 * offsets from the raw value itself. In testing, doing the same
		 * for the accelerometer raw values decreased accuracy.
		 *
		 * Note that the gyro values are multiplied by the
		 * precision-saving scaling factor to prevent large inaccuracies
		 * due to truncation of the resolution value which would
		 * otherwise occur. To prevent overflow (without resorting to 64
		 * bit integer math), the mult_frac macro is used.
		 */
		value[0] = mult_frac((NX_CTLR_IMU_PREC_RANGE_SCALE *
				      (imu_data[i].gyro_x -
				       ctlr->gyro_cal.offset[0])),
				     ctlr->gyro_cal.scale[0],
				     ctlr->imu_cal_gyro_divisor[0]);
		value[1] = mult_frac((NX_CTLR_IMU_PREC_RANGE_SCALE *
				      (imu_data[i].gyro_y -
				       ctlr->gyro_cal.offset[1])),
				     ctlr->gyro_cal.scale[1],
				     ctlr->imu_cal_gyro_divisor[1]);
		value[2] = mult_frac((NX_CTLR_IMU_PREC_RANGE_SCALE *
				      (imu_data[i].gyro_z -
				       ctlr->gyro_cal.offset[2])),
				     ctlr->gyro_cal.scale[2],
				     ctlr->imu_cal_gyro_divisor[2]);

		value[3] = ((s32)imu_data[i].accel_x *
			    ctlr->accel_cal.scale[0]) /
			    ctlr->imu_cal_accel_divisor[0];
		value[4] = ((s32)imu_data[i].accel_y *
			    ctlr->accel_cal.scale[1]) /
			    ctlr->imu_cal_accel_divisor[1];
		value[5] = ((s32)imu_data[i].accel_z *
			    ctlr->accel_cal.scale[2]) /
			    ctlr->imu_cal_accel_divisor[2];

		hid_dbg(ctlr->hdev,
			"raw_gyro: g_x=%d g_y=%d g_z=%d\n",
			imu_data[i].gyro_x,
			imu_data[i].gyro_y,
			imu_data[i].gyro_z);
		hid_dbg(ctlr->hdev,
			"raw_accel: a_x=%d a_y=%d a_z=%d\n",
			imu_data[i].accel_x,
			imu_data[i].accel_y,
			imu_data[i].accel_z);

		/*
		 * The right joy-con has 2 axes negated, Y and Z. This is due to
		 * the orientation of the IMU in the controller. We negate those
		 * axes' values in order to be consistent with the left joy-con
		 * and the pro controller:
		 *   X: positive is pointing toward the triggers
		 *   Y: positive is pointing to the left
		 *   Z: positive is pointing up (out of the buttons/sticks)
		 * The axes follow the right-hand rule.
		 */
		if (nx_ctlr_type_is_right_joycon(ctlr)) {
			int j;

			/* negate all but x axis */
			for (j = 1; j < 6; ++j) {
				if (j == 3)
					continue;
				value[j] *= -1;
			}
		}

		input_report_abs(idev, ABS_RX, value[0]);
		input_report_abs(idev, ABS_RY, value[1]);
		input_report_abs(idev, ABS_RZ, value[2]);
		input_report_abs(idev, ABS_X, value[3]);
		input_report_abs(idev, ABS_Y, value[4]);
		input_report_abs(idev, ABS_Z, value[5]);
		input_sync(idev);

		/* convert to micros and divide by 3 (3 samples per report). */
		ctlr->imu_timestamp_us += ctlr->imu_avg_delta_ms * 1000 / 3;
	}
}

static void nx_ctlr_parse_report(struct nx_ctlr *ctlr, struct nx_ctlr_input_report *rep)
{
	struct input_dev *dev = ctlr->input;
	unsigned long flags;
	u8 tmp;
	u32 btns;
	unsigned long msecs = jiffies_to_msecs(jiffies);

	spin_lock_irqsave(&ctlr->lock, flags);
	if (IS_ENABLED(CONFIG_NINTENDO_FF) && rep->vibrator_report &&
	    (msecs - ctlr->rumble_msecs) >= NX_CTLR_RUMBLE_PERIOD_MS &&
	    (ctlr->rumble_queue_head != ctlr->rumble_queue_tail ||
	     ctlr->rumble_zero_countdown > 0)) {
		/*
		 * When this value reaches 0, we know we've sent multiple
		 * packets to the controller instructing it to disable rumble.
		 * We can safely stop sending periodic rumble packets until the
		 * next ff effect.
		 */
		if (ctlr->rumble_zero_countdown > 0)
			ctlr->rumble_zero_countdown--;
		queue_work(ctlr->rumble_queue, &ctlr->rumble_worker);
	}

	/* Parse the battery status */
	tmp = rep->bat_con;
	ctlr->host_powered = tmp & BIT(0);
	ctlr->battery_charging = tmp & BIT(4);
	tmp = tmp >> 5;
	switch (tmp) {
	case 0: /* empty */
		ctlr->battery_capacity = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		break;
	case 1: /* low */
		ctlr->battery_capacity = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		break;
	case 2: /* medium */
		ctlr->battery_capacity = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		break;
	case 3: /* high */
		ctlr->battery_capacity = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
		break;
	case 4: /* full */
		ctlr->battery_capacity = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		break;
	default:
		ctlr->battery_capacity = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
		hid_warn(ctlr->hdev, "Invalid battery status\n");
		break;
	}
	spin_unlock_irqrestore(&ctlr->lock, flags);

	btns = hid_field_extract(ctlr->hdev, rep->button_status, 0, 24);

	if (nx_ctlr_type_has_left_controls(ctlr)) {
		u16 raw_x;
		u16 raw_y;
		s32 x;
		s32 y;

		raw_x = hid_field_extract(ctlr->hdev, rep->left_stick, 0, 12);
		raw_y = hid_field_extract(ctlr->hdev, rep->left_stick + 1, 4, 12);

		x = nx_ctlr_map_stick_val(&ctlr->left_stick_cal_x, raw_x);
		y = -nx_ctlr_map_stick_val(&ctlr->left_stick_cal_y, raw_y);

		input_report_abs(dev, ABS_X, x);
		input_report_abs(dev, ABS_Y, y);

		input_report_key(dev, BTN_TL, btns & NX_CTLR_BTN_L);
		input_report_key(dev, BTN_TL2, btns & NX_CTLR_BTN_ZL);
		input_report_key(dev, BTN_SELECT, btns & NX_CTLR_BTN_MINUS);
		input_report_key(dev, BTN_THUMBL, btns & NX_CTLR_BTN_LSTICK);
		input_report_key(dev, BTN_Z, btns & NX_CTLR_BTN_CAP);

		if (nx_ctlr_type_is_any_joycon(ctlr)) {
			/* Report the S buttons as the non-existent triggers */
			input_report_key(dev, BTN_TR, btns & NX_CTLR_BTN_SL_L);
			input_report_key(dev, BTN_TR2, btns & NX_CTLR_BTN_SR_L);

			/* Report d-pad as digital buttons for the joy-cons */
			input_report_key(dev, BTN_DPAD_DOWN, btns & NX_CTLR_BTN_DOWN);
			input_report_key(dev, BTN_DPAD_UP, btns & NX_CTLR_BTN_UP);
			input_report_key(dev, BTN_DPAD_RIGHT, btns & NX_CTLR_BTN_RIGHT);
			input_report_key(dev, BTN_DPAD_LEFT, btns & NX_CTLR_BTN_LEFT);
		} else {
			int hatx = 0;
			int haty = 0;

			/* d-pad x */
			if (btns & NX_CTLR_BTN_LEFT)
				hatx = -1;
			else if (btns & NX_CTLR_BTN_RIGHT)
				hatx = 1;
			input_report_abs(dev, ABS_HAT0X, hatx);

			/* d-pad y */
			if (btns & NX_CTLR_BTN_UP)
				haty = -1;
			else if (btns & NX_CTLR_BTN_DOWN)
				haty = 1;
			input_report_abs(dev, ABS_HAT0Y, haty);
		}
	}
	if (nx_ctlr_type_has_right_controls(ctlr)) {
		u16 raw_x;
		u16 raw_y;
		s32 x;
		s32 y;

		/* get raw stick values */
		raw_x = hid_field_extract(ctlr->hdev, rep->right_stick, 0, 12);
		raw_y = hid_field_extract(ctlr->hdev, rep->right_stick + 1, 4, 12);

		x = nx_ctlr_map_stick_val(&ctlr->right_stick_cal_x, raw_x);
		y = -nx_ctlr_map_stick_val(&ctlr->right_stick_cal_y, raw_y);

		input_report_abs(dev, ABS_RX, x);
		input_report_abs(dev, ABS_RY, y);

		input_report_key(dev, BTN_TR, btns & NX_CTLR_BTN_R);
		input_report_key(dev, BTN_TR2, btns & NX_CTLR_BTN_ZR);
		if (nx_ctlr_type_is_any_joycon(ctlr)) {
			/* Report the S buttons as the non-existent triggers */
			input_report_key(dev, BTN_TL, btns & NX_CTLR_BTN_SL_R);
			input_report_key(dev, BTN_TL2, btns & NX_CTLR_BTN_SR_R);
		}
		input_report_key(dev, BTN_START, btns & NX_CTLR_BTN_PLUS);
		input_report_key(dev, BTN_THUMBR, btns & NX_CTLR_BTN_RSTICK);
		input_report_key(dev, BTN_MODE, btns & NX_CTLR_BTN_HOME);
		input_report_key(dev, BTN_WEST, btns & NX_CTLR_BTN_Y);
		input_report_key(dev, BTN_NORTH, btns & NX_CTLR_BTN_X);
		input_report_key(dev, BTN_EAST, btns & NX_CTLR_BTN_A);
		input_report_key(dev, BTN_SOUTH, btns & NX_CTLR_BTN_B);
	}
	if (nx_ctlr_type_is_any_nescon(ctlr) ||
	    nx_ctlr_type_is_snescon(ctlr) ||
	    nx_ctlr_type_is_gencon(ctlr)) {
		s8 x;
		s8 y;

		if (btns & NX_CTLR_BTN_LEFT)
			x = -1;
		else if (btns & NX_CTLR_BTN_RIGHT)
			x = 1;

		if (btns & NX_CTLR_BTN_UP)
			y = -1;
		else if (btns & NX_CTLR_BTN_DOWN)
			y = 1;

		input_report_abs(dev, ABS_HAT0X, x);
		input_report_abs(dev, ABS_HAT0Y, y);

		if (nx_ctlr_type_is_any_nescon(ctlr)) {
			input_report_key(dev, BTN_SELECT, btns & NX_CTLR_BTN_MINUS);
			input_report_key(dev, BTN_START, btns & NX_CTLR_BTN_PLUS);
			input_report_key(dev, BTN_EAST, btns & NX_CTLR_BTN_B);
			input_report_key(dev, BTN_SOUTH, btns & NX_CTLR_BTN_A);
			input_report_key(dev, BTN_TL, btns & NX_CTLR_BTN_L);
			input_report_key(dev, BTN_TR, btns & NX_CTLR_BTN_R);
		} else if (nx_ctlr_type_is_snescon(ctlr)) {
			input_report_key(dev, BTN_SELECT, btns & NX_CTLR_BTN_MINUS);
			input_report_key(dev, BTN_START, btns & NX_CTLR_BTN_PLUS);

			/* these two are mixed up for some reason */
			input_report_key(dev, BTN_EAST, btns & NX_CTLR_BTN_B);
			input_report_key(dev, BTN_SOUTH, btns & NX_CTLR_BTN_A);

			input_report_key(dev, BTN_NORTH, btns & NX_CTLR_BTN_X);
			input_report_key(dev, BTN_WEST, btns & NX_CTLR_BTN_Y);
			input_report_key(dev, BTN_TL, btns & NX_CTLR_BTN_L);
			input_report_key(dev, BTN_TR, btns & NX_CTLR_BTN_R);
			input_report_key(dev, BTN_TL2, btns & NX_CTLR_BTN_ZL);
			input_report_key(dev, BTN_TR2, btns & NX_CTLR_BTN_ZR);
		} else if (nx_ctlr_type_is_gencon(ctlr)) {
			input_report_key(dev, BTN_SELECT, btns & NX_CTLR_BTN_ZR);
			input_report_key(dev, BTN_START, btns & NX_CTLR_BTN_PLUS);
			input_report_key(dev, BTN_EAST, btns & NX_CTLR_BTN_B);
			input_report_key(dev, BTN_SOUTH, btns & NX_CTLR_BTN_A);
			input_report_key(dev, BTN_WEST, btns & NX_CTLR_BTN_R);
			input_report_key(dev, BTN_MODE, btns & NX_CTLR_BTN_HOME);
		}
	}

	input_sync(dev);

	/*
	 * Immediately after receiving a report is the most reliable time to
	 * send a subcommand to the controller. Wake any subcommand senders
	 * waiting for a report.
	 */
	if (unlikely(mutex_is_locked(&ctlr->output_mutex))) {
		spin_lock_irqsave(&ctlr->lock, flags);
		ctlr->received_input_report = true;
		spin_unlock_irqrestore(&ctlr->lock, flags);
		wake_up(&ctlr->wait);
	}

	/* parse IMU data if present */
	if (rep->id == NX_CTLR_INPUT_IMU_DATA && nx_ctlr_has_imu(ctlr))
		nx_ctlr_parse_imu_report(ctlr, rep);
}

static int nx_ctlr_send_rumble_data(struct nx_ctlr *ctlr)
{
	int ret;
	unsigned long flags;
	struct nx_ctlr_rumble_output rumble_output = { 0 };

	spin_lock_irqsave(&ctlr->lock, flags);
	/*
	 * If the controller has been removed, just return ENODEV so the LED
	 * subsystem doesn't print invalid errors on removal.
	 */
	if (ctlr->ctlr_state == NX_CTLR_STATE_REMOVED) {
		spin_unlock_irqrestore(&ctlr->lock, flags);
		return -ENODEV;
	}
	memcpy(rumble_output.rumble_data,
	       ctlr->rumble_data[ctlr->rumble_queue_tail],
	       NX_CTLR_RUMBLE_DATA_SIZE);
	spin_unlock_irqrestore(&ctlr->lock, flags);

	rumble_output.output_id = NX_CTLR_OUTPUT_RUMBLE_ONLY;
	rumble_output.packet_num = ctlr->subcmd_num;
	if (++ctlr->subcmd_num > 0xF)
		ctlr->subcmd_num = 0;

	nx_ctlr_enforce_subcmd_rate(ctlr);

	ret = __nx_ctlr_hid_send(ctlr->hdev,
				 (u8 *)&rumble_output,
				 sizeof(rumble_output));
	return ret;
}

static void nx_ctlr_rumble_worker(struct work_struct *work)
{
	struct nx_ctlr *ctlr = container_of(work, struct nx_ctlr, rumble_worker);
	unsigned long flags;
	bool again = true;
	int ret;

	while (again) {
		mutex_lock(&ctlr->output_mutex);
		ret = nx_ctlr_send_rumble_data(ctlr);
		mutex_unlock(&ctlr->output_mutex);

		/* -ENODEV means the controller was just unplugged */
		spin_lock_irqsave(&ctlr->lock, flags);
		if (ret < 0 && ret != -ENODEV && ctlr->ctlr_state != NX_CTLR_STATE_REMOVED)
			hid_warn(ctlr->hdev, "Failed to set rumble; e=%d", ret);

		ctlr->rumble_msecs = jiffies_to_msecs(jiffies);
		if (ctlr->rumble_queue_tail != ctlr->rumble_queue_head) {
			if (++ctlr->rumble_queue_tail >= NX_CTLR_RUMBLE_QUEUE_SIZE)
				ctlr->rumble_queue_tail = 0;
		} else {
			again = false;
		}
		spin_unlock_irqrestore(&ctlr->lock, flags);
	}
}

#if IS_ENABLED(CONFIG_NINTENDO_FF)
static struct nx_ctlr_rumble_freq_data nx_ctlr_find_rumble_freq(u16 freq)
{
	const size_t length = ARRAY_SIZE(nx_ctlr_rumble_frequencies);
	const struct nx_ctlr_rumble_freq_data *data = nx_ctlr_rumble_frequencies;
	int i = 0;

	if (freq > data[0].freq) {
		for (i = 1; i < length - 1; i++) {
			if (freq > data[i - 1].freq && freq <= data[i].freq)
				break;
		}
	}

	return data[i];
}

static struct nx_ctlr_rumble_amp_data nx_ctlr_find_rumble_amp(u16 amp)
{
	const size_t length = ARRAY_SIZE(nx_ctlr_rumble_amplitudes);
	const struct nx_ctlr_rumble_amp_data *data = nx_ctlr_rumble_amplitudes;
	int i = 0;

	if (amp > data[0].amp) {
		for (i = 1; i < length - 1; i++) {
			if (amp > data[i - 1].amp && amp <= data[i].amp)
				break;
		}
	}

	return data[i];
}

static void nx_ctlr_encode_rumble(u8 *data, u16 freq_low, u16 freq_high, u16 amp)
{
	struct nx_ctlr_rumble_freq_data freq_data_low;
	struct nx_ctlr_rumble_freq_data freq_data_high;
	struct nx_ctlr_rumble_amp_data amp_data;

	freq_data_low = nx_ctlr_find_rumble_freq(freq_low);
	freq_data_high = nx_ctlr_find_rumble_freq(freq_high);
	amp_data = nx_ctlr_find_rumble_amp(amp);

	data[0] = (freq_data_high.high >> 8) & 0xFF;
	data[1] = (freq_data_high.high & 0xFF) + amp_data.high;
	data[2] = freq_data_low.low + ((amp_data.low >> 8) & 0xFF);
	data[3] = amp_data.low & 0xFF;
}

static const u16 NX_CTLR_MAX_RUMBLE_HIGH_FREQ	= 1253;
static const u16 NX_CTLR_MIN_RUMBLE_HIGH_FREQ	= 82;
static const u16 NX_CTLR_MAX_RUMBLE_LOW_FREQ	= 626;
static const u16 NX_CTLR_MIN_RUMBLE_LOW_FREQ	= 41;

static void nx_ctlr_clamp_rumble_freqs(struct nx_ctlr *ctlr)
{
	unsigned long flags;

	spin_lock_irqsave(&ctlr->lock, flags);
	ctlr->rumble_ll_freq = clamp(ctlr->rumble_ll_freq,
				     NX_CTLR_MIN_RUMBLE_LOW_FREQ,
				     NX_CTLR_MAX_RUMBLE_LOW_FREQ);
	ctlr->rumble_lh_freq = clamp(ctlr->rumble_lh_freq,
				     NX_CTLR_MIN_RUMBLE_HIGH_FREQ,
				     NX_CTLR_MAX_RUMBLE_HIGH_FREQ);
	ctlr->rumble_rl_freq = clamp(ctlr->rumble_rl_freq,
				     NX_CTLR_MIN_RUMBLE_LOW_FREQ,
				     NX_CTLR_MAX_RUMBLE_LOW_FREQ);
	ctlr->rumble_rh_freq = clamp(ctlr->rumble_rh_freq,
				     NX_CTLR_MIN_RUMBLE_HIGH_FREQ,
				     NX_CTLR_MAX_RUMBLE_HIGH_FREQ);
	spin_unlock_irqrestore(&ctlr->lock, flags);
}

static int nx_ctlr_set_rumble(struct nx_ctlr *ctlr,
			      u16 amp_r,
			      u16 amp_l,
			      bool schedule_now)
{
	u8 data[NX_CTLR_RUMBLE_DATA_SIZE];
	u16 amp;
	u16 freq_r_low;
	u16 freq_r_high;
	u16 freq_l_low;
	u16 freq_l_high;
	unsigned long flags;

	spin_lock_irqsave(&ctlr->lock, flags);
	freq_r_low = ctlr->rumble_rl_freq;
	freq_r_high = ctlr->rumble_rh_freq;
	freq_l_low = ctlr->rumble_ll_freq;
	freq_l_high = ctlr->rumble_lh_freq;
	/* limit number of silent rumble packets to reduce traffic */
	if (amp_l != 0 || amp_r != 0)
		ctlr->rumble_zero_countdown = NX_CTLR_RUMBLE_ZERO_AMP_PKT_CNT;
	spin_unlock_irqrestore(&ctlr->lock, flags);

	/* right joy-con */
	amp = amp_r * (u32)nx_ctlr_max_rumble_amp / 65535;
	nx_ctlr_encode_rumble(data + 4, freq_r_low, freq_r_high, amp);

	/* left joy-con */
	amp = amp_l * (u32)nx_ctlr_max_rumble_amp / 65535;
	nx_ctlr_encode_rumble(data, freq_l_low, freq_l_high, amp);

	spin_lock_irqsave(&ctlr->lock, flags);
	if (++ctlr->rumble_queue_head >= NX_CTLR_RUMBLE_QUEUE_SIZE)
		ctlr->rumble_queue_head = 0;
	memcpy(ctlr->rumble_data[ctlr->rumble_queue_head], data,
	       NX_CTLR_RUMBLE_DATA_SIZE);
	spin_unlock_irqrestore(&ctlr->lock, flags);

	/* don't wait for the periodic send (reduces latency) */
	if (schedule_now)
		queue_work(ctlr->rumble_queue, &ctlr->rumble_worker);

	return 0;
}

static int nx_ctlr_play_effect(struct input_dev *dev,
			       void *data,
			       struct ff_effect *effect)
{
	struct nx_ctlr *ctlr = input_get_drvdata(dev);

	if (effect->type != FF_RUMBLE)
		return 0;

	return nx_ctlr_set_rumble(ctlr,
				  effect->u.rumble.weak_magnitude,
				  effect->u.rumble.strong_magnitude,
				  true);
}
#endif /* IS_ENABLED(CONFIG_NINTENDO_FF) */

static const unsigned int nx_ctlr_button_inputs_l[] = {
	BTN_SELECT, BTN_Z, BTN_THUMBL,
	BTN_TL, BTN_TL2,
	0 /* 0 signals end of array */
};

static const unsigned int nx_ctlr_button_inputs_r[] = {
	BTN_START, BTN_MODE, BTN_THUMBR,
	BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
	BTN_TR, BTN_TR2,
	0 /* 0 signals end of array */
};

/* We report joy-con d-pad inputs as buttons and pro controller as a hat. */
static const unsigned int nx_ctlr_dpad_inputs_jc[] = {
	BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
};

static const unsigned int nescon_button_inputs[] = {
	BTN_SELECT,	/* "Select" */
	BTN_START, 	/* "Start" */
	BTN_SOUTH, 	/* "B" */
	BTN_EAST,  	/* "A" */
	BTN_TL,    	/* "L" */
	BTN_TR,    	/* "R" */
	0          	/* 0 signals end of array */
};

static const unsigned int snescon_button_inputs[] = {
	BTN_SELECT,	/* "Select" */
	BTN_START, 	/* "Start" */
	BTN_SOUTH, 	/* "B" */
	BTN_EAST,  	/* "A" */
	BTN_NORTH, 	/* "X" */
	BTN_WEST,  	/* "Y" */
	BTN_TL,    	/* "L" */
	BTN_TL2,   	/* "ZL" */
	BTN_TR,    	/* "R" */
	BTN_TR2,   	/* "ZR" */
	0          	/* 0 signals end of array */
};

/*
 * Note: Button order is determined by the numerical order of the constants (see
 * `uapi/linux/input-event-codes.h`).
 *
 * A, B, and C are mapped positionally, rather than by label (e.g., "A" gets
 * assigned to BTN_EAST instead of BTN_A).
 */
static const unsigned int gencon_button_inputs[] = {
	BTN_SELECT,	/* "Mode" */
	BTN_START,	/* "Start" */
	BTN_EAST,	/* "A" */
	BTN_SOUTH,	/* "B" */
	BTN_WEST,	/* "C" */
	BTN_MODE,	/* Home */
	/* BTN_???,	/* Capture */
	0		/* 0 signals end of array */
};

static int nx_ctlr_input_create(struct nx_ctlr *ctlr)
{
	struct hid_device *hdev;
	const char *name;
	const char *imu_name;
	int ret;
	int i;

	hdev = ctlr->hdev;

	if (nx_ctlr_device_is_procon(ctlr)) {
		name = "Nintendo Switch Pro Controller";
		imu_name = "Nintendo Switch Pro Controller IMU";
	} else if (nx_ctlr_device_is_chrggrip(ctlr)) {
		if (nx_ctlr_type_has_left_controls(ctlr)) {
			name = "Nintendo Switch Left Joy-Con (Grip)";
			imu_name = "Nintendo Switch Left Joy-Con IMU (Grip)";
		} else {
			name = "Nintendo Switch Right Joy-Con (Grip)";
			imu_name = "Nintendo Switch Right Joy-Con IMU (Grip)";
		}
	} else if (nx_ctlr_type_is_left_joycon(ctlr)) {
		name = "Nintendo Switch Left Joy-Con";
		imu_name = "Nintendo Switch Left Joy-Con IMU";
	} else if (nx_ctlr_type_is_right_joycon(ctlr)) {
		name = "Nintendo Switch Right Joy-Con";
		imu_name = "Nintendo Switch Right Joy-Con IMU";
	} else if (nx_ctlr_type_is_left_nescon(ctlr)) {
		name = "Nintendo Controller for Nintendo Switch Online (L)";
		imu_name = NULL;
	} else if (nx_ctlr_type_is_right_nescon(ctlr)) {
		name = "Nintendo Controller for Nintendo Switch Online (R)";
		imu_name = NULL;
	} else if (nx_ctlr_type_is_snescon(ctlr)) {
		name = "Super Nintendo Controller for Nintendo Switch Online";
		imu_name = NULL;
	} else if (nx_ctlr_type_is_gencon(ctlr)) {
		name = "SEGA Genesis Control Pad for Nintendo Switch Online";
		imu_name = NULL;
	} else {
		hid_err(hdev, "Invalid device\n");
		return -EINVAL;
	}

	if (!(ctlr->input = devm_input_allocate_device(&hdev->dev)))
		return -ENOMEM;
	ctlr->input->id.bustype = hdev->bus;
	ctlr->input->id.vendor = hdev->vendor;
	ctlr->input->id.product = hdev->product;
	ctlr->input->id.version = hdev->version;
	ctlr->input->uniq = ctlr->mac_addr_str;
	ctlr->input->name = name;
	input_set_drvdata(ctlr->input, ctlr);

	/* set up sticks and buttons */
	if (nx_ctlr_type_has_left_controls(ctlr)) {
		input_set_abs_params(ctlr->input,
				     ABS_X,
				     -NX_CTLR_MAX_STICK_MAG,
				     NX_CTLR_MAX_STICK_MAG,
				     NX_CTLR_STICK_FUZZ,
				     NX_CTLR_STICK_FLAT);
		input_set_abs_params(ctlr->input,
				     ABS_Y,
				     -NX_CTLR_MAX_STICK_MAG,
				     NX_CTLR_MAX_STICK_MAG,
				     NX_CTLR_STICK_FUZZ,
				     NX_CTLR_STICK_FLAT);

		for (i = 0; nx_ctlr_button_inputs_l[i] > 0; i++)
			input_set_capability(ctlr->input, EV_KEY,
					     nx_ctlr_button_inputs_l[i]);

		/* configure d-pad differently for joy-con vs pro controller */
		if (!(nx_ctlr_device_is_procon(ctlr))) {
			for (i = 0; nx_ctlr_dpad_inputs_jc[i] > 0; i++)
				input_set_capability(ctlr->input,
						     EV_KEY,
						     nx_ctlr_dpad_inputs_jc[i]);
		} else {
			input_set_abs_params(ctlr->input,
					     ABS_HAT0X,
					     -NX_CTLR_MAX_DPAD_MAG,
					     NX_CTLR_MAX_DPAD_MAG,
					     NX_CTLR_DPAD_FUZZ,
					     NX_CTLR_DPAD_FLAT);
			input_set_abs_params(ctlr->input,
					     ABS_HAT0Y,
					     -NX_CTLR_MAX_DPAD_MAG,
					     NX_CTLR_MAX_DPAD_MAG,
					     NX_CTLR_DPAD_FUZZ,
					     NX_CTLR_DPAD_FLAT);
		}
	}
	if (nx_ctlr_type_has_right_controls(ctlr)) {
		input_set_abs_params(ctlr->input,
				     ABS_RX,
				     -NX_CTLR_MAX_STICK_MAG,
				     NX_CTLR_MAX_STICK_MAG,
				     NX_CTLR_STICK_FUZZ,
				     NX_CTLR_STICK_FLAT);
		input_set_abs_params(ctlr->input,
				     ABS_RY,
				     -NX_CTLR_MAX_STICK_MAG,
				     NX_CTLR_MAX_STICK_MAG,
				     NX_CTLR_STICK_FUZZ,
				     NX_CTLR_STICK_FLAT);

		for (i = 0; nx_ctlr_button_inputs_r[i] > 0; i++)
			input_set_capability(ctlr->input,
					     EV_KEY,
					     nx_ctlr_button_inputs_r[i]);
	}
	if (nx_ctlr_type_is_any_nescon(ctlr)) {
		/* set up dpad hat */
		input_set_abs_params(ctlr->input, ABS_HAT0X, -1, 1, 0, 0);
		input_set_abs_params(ctlr->input, ABS_HAT0Y, -1, 1, 0, 0);

		/* set up buttons */
		for (i = 0; nescon_button_inputs[i] > 0; i++)
			input_set_capability(ctlr->input,
					     EV_KEY,
					     nescon_button_inputs[i]);

		/* register the device here, we don't need any more setup */
		if ((ret = input_register_device(ctlr->input)))
			return ret;

		return 0;
	}
	if (nx_ctlr_type_is_snescon(ctlr)) {
		/* set up dpad hat */
		input_set_abs_params(ctlr->input, ABS_HAT0X, -1, 1, 0, 0);
		input_set_abs_params(ctlr->input, ABS_HAT0Y, -1, 1, 0, 0);

		/* set up buttons */
		for (i = 0; snescon_button_inputs[i] > 0; i++)
			input_set_capability(ctlr->input,
					     EV_KEY,
					     snescon_button_inputs[i]);

		/* register the device here, we don't need any more setup */
		if ((ret = input_register_device(ctlr->input)))
			return ret;

		return 0;
	}
	if (nx_ctlr_type_is_gencon(ctlr)) {
		/* set up dpad hat */
		input_set_abs_params(ctlr->input, ABS_HAT0X, -1, 1, 0, 0);
		input_set_abs_params(ctlr->input, ABS_HAT0Y, -1, 1, 0, 0);

		/* set up buttons */
		for (i = 0; gencon_button_inputs[i] > 0; i++)
			input_set_capability(ctlr->input,
					     EV_KEY,
					     gencon_button_inputs[i]);

		/* register the device here, we don't need any more setup */
		if ((ret = input_register_device(ctlr->input)))
			return ret;

		return 0;
	}

	/* Let's report joy-con S triggers separately */
	if (nx_ctlr_type_is_left_joycon(ctlr)) {
		input_set_capability(ctlr->input, EV_KEY, BTN_TR);
		input_set_capability(ctlr->input, EV_KEY, BTN_TR2);
	} else if (nx_ctlr_type_is_left_joycon(ctlr)) {
		input_set_capability(ctlr->input, EV_KEY, BTN_TL);
		input_set_capability(ctlr->input, EV_KEY, BTN_TL2);
	}

#if IS_ENABLED(CONFIG_NINTENDO_FF)
	/* set up rumble */
	input_set_capability(ctlr->input, EV_FF, FF_RUMBLE);
	input_ff_create_memless(ctlr->input, NULL, nx_ctlr_play_effect);
	ctlr->rumble_ll_freq = NX_CTLR_RUMBLE_DFLT_LOW_FREQ;
	ctlr->rumble_lh_freq = NX_CTLR_RUMBLE_DFLT_HIGH_FREQ;
	ctlr->rumble_rl_freq = NX_CTLR_RUMBLE_DFLT_LOW_FREQ;
	ctlr->rumble_rh_freq = NX_CTLR_RUMBLE_DFLT_HIGH_FREQ;
	nx_ctlr_clamp_rumble_freqs(ctlr);
	nx_ctlr_set_rumble(ctlr, 0, 0, false);
	ctlr->rumble_msecs = jiffies_to_msecs(jiffies);
#endif

	if ((ret = input_register_device(ctlr->input)))
		return ret;

	/* configure the imu input device */
	if (!(ctlr->imu_input = devm_input_allocate_device(&hdev->dev)))
		return -ENOMEM;

	ctlr->imu_input->id.bustype = hdev->bus;
	ctlr->imu_input->id.vendor = hdev->vendor;
	ctlr->imu_input->id.product = hdev->product;
	ctlr->imu_input->id.version = hdev->version;
	ctlr->imu_input->uniq = ctlr->mac_addr_str;
	ctlr->imu_input->name = imu_name;
	input_set_drvdata(ctlr->imu_input, ctlr);

	/* configure imu axes */
	input_set_abs_params(ctlr->imu_input,
			     ABS_X,
			     -NX_CTLR_IMU_MAX_ACCEL_MAG,
			     NX_CTLR_IMU_MAX_ACCEL_MAG,
			     NX_CTLR_IMU_ACCEL_FUZZ,
			     NX_CTLR_IMU_ACCEL_FLAT);
	input_set_abs_params(ctlr->imu_input,
			     ABS_Y,
			     -NX_CTLR_IMU_MAX_ACCEL_MAG,
			     NX_CTLR_IMU_MAX_ACCEL_MAG,
			     NX_CTLR_IMU_ACCEL_FUZZ,
			     NX_CTLR_IMU_ACCEL_FLAT);
	input_set_abs_params(ctlr->imu_input,
			     ABS_Z,
			     -NX_CTLR_IMU_MAX_ACCEL_MAG,
			     NX_CTLR_IMU_MAX_ACCEL_MAG,
			     NX_CTLR_IMU_ACCEL_FUZZ,
			     NX_CTLR_IMU_ACCEL_FLAT);
	input_abs_set_res(ctlr->imu_input, ABS_X, NX_CTLR_IMU_ACCEL_RES_PER_G);
	input_abs_set_res(ctlr->imu_input, ABS_Y, NX_CTLR_IMU_ACCEL_RES_PER_G);
	input_abs_set_res(ctlr->imu_input, ABS_Z, NX_CTLR_IMU_ACCEL_RES_PER_G);

	input_set_abs_params(ctlr->imu_input,
			     ABS_RX,
			     -NX_CTLR_IMU_MAX_GYRO_MAG,
			     NX_CTLR_IMU_MAX_GYRO_MAG,
			     NX_CTLR_IMU_GYRO_FUZZ,
			     NX_CTLR_IMU_GYRO_FLAT);
	input_set_abs_params(ctlr->imu_input,
			     ABS_RY,
			     -NX_CTLR_IMU_MAX_GYRO_MAG,
			     NX_CTLR_IMU_MAX_GYRO_MAG,
			     NX_CTLR_IMU_GYRO_FUZZ,
			     NX_CTLR_IMU_GYRO_FLAT);
	input_set_abs_params(ctlr->imu_input,
			     ABS_RZ,
			     -NX_CTLR_IMU_MAX_GYRO_MAG,
			     NX_CTLR_IMU_MAX_GYRO_MAG,
			     NX_CTLR_IMU_GYRO_FUZZ,
			     NX_CTLR_IMU_GYRO_FLAT);

	input_abs_set_res(ctlr->imu_input, ABS_RX, NX_CTLR_IMU_GYRO_RES_PER_DPS);
	input_abs_set_res(ctlr->imu_input, ABS_RY, NX_CTLR_IMU_GYRO_RES_PER_DPS);
	input_abs_set_res(ctlr->imu_input, ABS_RZ, NX_CTLR_IMU_GYRO_RES_PER_DPS);

	__set_bit(EV_MSC, ctlr->imu_input->evbit);
	__set_bit(MSC_TIMESTAMP, ctlr->imu_input->mscbit);
	__set_bit(INPUT_PROP_ACCELEROMETER, ctlr->imu_input->propbit);

	if ((ret = input_register_device(ctlr->imu_input)))
		return ret;

	return 0;
}

static int nx_ctlr_player_led_brightness_set(struct led_classdev *led,
					     enum led_brightness brightness)
{
	struct device *dev = led->dev->parent;
	struct hid_device *hdev = to_hid_device(dev);
	struct nx_ctlr *ctlr;
	int val = 0;
	int i;
	int ret;
	int num;

	if (!(ctlr = hid_get_drvdata(hdev))) {
		hid_err(hdev, "No controller data\n");
		return -ENODEV;
	}

	/* determine which player led this is */
	for (num = 0; num < NX_CTLR_NUM_LEDS; num++) {
		if (&ctlr->leds[num] == led)
			break;
	}
	if (num >= NX_CTLR_NUM_LEDS)
		return -EINVAL;

	mutex_lock(&ctlr->output_mutex);
	for (i = 0; i < NX_CTLR_NUM_LEDS; i++) {
		if (i == num)
			val |= brightness << i;
		else
			val |= ctlr->leds[i].brightness << i;
	}
	ret = nx_ctlr_set_player_leds(ctlr, 0, val);
	mutex_unlock(&ctlr->output_mutex);

	return ret;
}

static int nx_ctlr_home_led_brightness_set(struct led_classdev *led,
					   enum led_brightness brightness)
{
	struct device *dev = led->dev->parent;
	struct hid_device *hdev = to_hid_device(dev);
	struct nx_ctlr *ctlr;
	struct nx_ctlr_subcmd_request *req;
	u8 buffer[sizeof(*req) + 5] = { 0 };
	u8 *data;
	int ret;

	if (!(ctlr = hid_get_drvdata(hdev))) {
		hid_err(hdev, "No controller data\n");
		return -ENODEV;
	}

	req = (struct nx_ctlr_subcmd_request *)buffer;
	req->subcmd_id = NX_CTLR_SUBCMD_SET_HOME_LIGHT;
	data = req->data;
	data[0] = 0x01;
	data[1] = brightness << 4;
	data[2] = brightness | (brightness << 4);
	data[3] = 0x11;
	data[4] = 0x11;

	hid_dbg(hdev, "setting home led brightness\n");
	mutex_lock(&ctlr->output_mutex);
	ret = nx_ctlr_send_subcmd(ctlr, req, 5, HZ/4);
	mutex_unlock(&ctlr->output_mutex);

	return ret;
}

static DEFINE_MUTEX(nx_ctlr_input_num_mutex);
static int nx_ctlr_leds_create(struct nx_ctlr *ctlr)
{
	struct hid_device *hdev = ctlr->hdev;
	struct device *dev = &hdev->dev;
	const char *d_name = dev_name(dev);
	struct led_classdev *led;
	char *name;
	int ret = 0;
	int i;
	static int input_num = 1;

	/* Set the default controller player leds based on controller number */
	mutex_lock(&nx_ctlr_input_num_mutex);
	mutex_lock(&ctlr->output_mutex);
	if ((ret = nx_ctlr_set_player_leds(ctlr, 0, 0xF >> (4 - input_num))))
		hid_warn(ctlr->hdev, "Failed to set leds; ret=%d\n", ret);
	mutex_unlock(&ctlr->output_mutex);

	/* configure the player LEDs */
	for (i = 0; i < NX_CTLR_NUM_LEDS; i++) {
		name = devm_kasprintf(dev, GFP_KERNEL, "%s:%s:%s",
				      d_name,
				      "green",
				      nx_ctlr_player_led_names[i]);
		if (!name) {
			mutex_unlock(&nx_ctlr_input_num_mutex);
			return -ENOMEM;
		}

		led = &ctlr->leds[i];
		led->name = name;
		led->brightness = ((i + 1) <= input_num) ? 1 : 0;
		led->max_brightness = 1;
		led->brightness_set_blocking =
					nx_ctlr_player_led_brightness_set;
		led->flags = LED_CORE_SUSPENDRESUME | LED_HW_PLUGGABLE;

		if ((ret = devm_led_classdev_register(&hdev->dev, led))) {
			hid_err(hdev, "Failed registering %s LED\n", led->name);
			mutex_unlock(&nx_ctlr_input_num_mutex);
			return ret;
		}
	}

	if (++input_num > 4)
		input_num = 1;
	mutex_unlock(&nx_ctlr_input_num_mutex);

	/* configure the home LED */
	if (nx_ctlr_type_has_right_controls(ctlr)) {
		name = devm_kasprintf(dev,
				      GFP_KERNEL,
				      "%s:%s:%s",
				      d_name,
				      "blue",
				      LED_FUNCTION_PLAYER5);
		if (!name)
			return -ENOMEM;

		led = &ctlr->home_led;
		led->name = name;
		led->brightness = 0;
		led->max_brightness = 0xF;
		led->brightness_set_blocking = nx_ctlr_home_led_brightness_set;
		led->flags = LED_CORE_SUSPENDRESUME | LED_HW_PLUGGABLE;

		if ((ret = devm_led_classdev_register(&hdev->dev, led))) {
			hid_err(hdev, "Failed registering home led\n");
			return ret;
		}
		/* Set the home LED to 0 as default state */
		if ((ret = nx_ctlr_home_led_brightness_set(led, 0))) {
			hid_err(hdev, "Failed to set home LED dflt; ret=%d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int nx_ctlr_battery_get_property(struct power_supply *supply,
					enum power_supply_property prop,
					union power_supply_propval *val)
{
	struct nx_ctlr *ctlr = power_supply_get_drvdata(supply);
	unsigned long flags;
	int ret = 0;
	u8 capacity;
	bool charging;
	bool powered;

	spin_lock_irqsave(&ctlr->lock, flags);
	capacity = ctlr->battery_capacity;
	charging = ctlr->battery_charging;
	powered = ctlr->host_powered;
	spin_unlock_irqrestore(&ctlr->lock, flags);

	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = capacity;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (charging)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (capacity == POWER_SUPPLY_CAPACITY_LEVEL_FULL &&
			 powered)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static enum power_supply_property nx_ctlr_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_STATUS,
};

static int nx_ctlr_power_supply_create(struct nx_ctlr *ctlr)
{
	struct hid_device *hdev = ctlr->hdev;
	struct power_supply_config supply_config = { .drv_data = ctlr, };
	const char * const name_fmt = "nintendo_switch_controller_battery_%s";
	int ret = 0;

	/* Set initially to unknown before receiving first input report */
	ctlr->battery_capacity = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;

	/* Configure the battery's description */
	ctlr->battery_desc.properties = nx_ctlr_battery_props;
	ctlr->battery_desc.num_properties = ARRAY_SIZE(nx_ctlr_battery_props);
	ctlr->battery_desc.get_property = nx_ctlr_battery_get_property;
	ctlr->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	ctlr->battery_desc.use_for_apm = 0;
	ctlr->battery_desc.name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
						 name_fmt,
						 dev_name(&hdev->dev));
	if (!ctlr->battery_desc.name)
		return -ENOMEM;

	ctlr->battery = devm_power_supply_register(&hdev->dev,
						   &ctlr->battery_desc,
						   &supply_config);
	if (IS_ERR(ctlr->battery)) {
		ret = PTR_ERR(ctlr->battery);
		hid_err(hdev, "Failed to register battery; ret=%d\n", ret);
		return ret;
	}

	return power_supply_powers(ctlr->battery, &hdev->dev);
}

static int nx_ctlr_request_device_info(struct nx_ctlr *ctlr)
{
	int ret;
	int i;
	int j;
	struct nx_ctlr_subcmd_request req = { 0 };
	struct nx_ctlr_input_report *report;

	req.subcmd_id = NX_CTLR_SUBCMD_REQ_DEV_INFO;
	if ((ret = nx_ctlr_send_subcmd(ctlr, &req, 0, HZ))) {
		hid_err(ctlr->hdev, "Failed to get NX controller info; ret=%d\n", ret);
		return ret;
	}

	report = (struct nx_ctlr_input_report *)ctlr->input_buf;

	for (i = 4, j = 0; j < 6; i++, j++)
		ctlr->mac_addr[j] = report->subcmd_reply.data[i];

	ctlr->mac_addr_str = devm_kasprintf(&ctlr->hdev->dev,
					    GFP_KERNEL,
					    "%02X:%02X:%02X:%02X:%02X:%02X",
					    ctlr->mac_addr[0],
					    ctlr->mac_addr[1],
					    ctlr->mac_addr[2],
					    ctlr->mac_addr[3],
					    ctlr->mac_addr[4],
					    ctlr->mac_addr[5]);
	if (!ctlr->mac_addr_str)
		return -ENOMEM;
	hid_info(ctlr->hdev, "controller MAC = %s\n", ctlr->mac_addr_str);

	/* Retrieve the type so we can distinguish for charging grip */
	ctlr->type = report->subcmd_reply.data[2];

	hid_dbg(ctlr->hdev, "ctlr->type = 0x%02X\n", ctlr->type);

	return 0;
}

/* Common handler for parsing inputs */
static int nx_ctlr_read_handler(struct nx_ctlr *ctlr, u8 *data, int size)
{
	if ((data[0] == NX_CTLR_INPUT_SUBCMD_REPLY ||
	     data[0] == NX_CTLR_INPUT_IMU_DATA ||
	     data[0] == NX_CTLR_INPUT_MCU_DATA) &&
	     size >= 12) { /* make sure it contains the input report */
		nx_ctlr_parse_report(ctlr, (struct nx_ctlr_input_report *)data);
	}

	return 0;
}

static int nx_ctlr_handle_event(struct nx_ctlr *ctlr, u8 *data, int size)
{
	int ret = 0;
	bool match = false;
	struct nx_ctlr_input_report *report;

	if (unlikely(mutex_is_locked(&ctlr->output_mutex)) &&
	    ctlr->msg_type != NX_CTLR_MSG_TYPE_NONE) {
		switch (ctlr->msg_type) {
		case NX_CTLR_MSG_TYPE_USB:
			if (size < 2)
				break;
			if (data[0] == NX_CTLR_INPUT_USB_RESPONSE &&
			    data[1] == ctlr->usb_ack_match)
				match = true;
			break;
		case NX_CTLR_MSG_TYPE_SUBCMD:
			if (size < sizeof(struct nx_ctlr_input_report) ||
			    data[0] != NX_CTLR_INPUT_SUBCMD_REPLY)
				break;
			report = (struct nx_ctlr_input_report *)data;
			if (report->subcmd_reply.id == ctlr->subcmd_ack_match)
				match = true;
			break;
		default:
			break;
		}

		if (match) {
			memcpy(ctlr->input_buf, data,
			       min(size, (int)NX_CTLR_MAX_RESP_SIZE));
			ctlr->msg_type = NX_CTLR_MSG_TYPE_NONE;
			ctlr->received_resp = true;
			wake_up(&ctlr->wait);

			/* This message has been handled */
			return 1;
		}
	}

	if (ctlr->ctlr_state == NX_CTLR_STATE_READ)
		ret = nx_ctlr_read_handler(ctlr, data, size);

	return ret;
}

static int nintendo_hid_event(struct hid_device *hdev,
			      struct hid_report *report,
			      u8 *raw_data,
			      int size)
{
	struct nx_ctlr *ctlr = hid_get_drvdata(hdev);

	if (size < 1)
		return -EINVAL;

	return nx_ctlr_handle_event(ctlr, raw_data, size);
}

static int nintendo_hid_probe(struct hid_device *hdev,
			      const struct hid_device_id *id)
{
	int ret;
	struct nx_ctlr *ctlr;

	hid_dbg(hdev, "probe - start\n");

	if (!(ctlr = devm_kzalloc(&hdev->dev, sizeof(*ctlr), GFP_KERNEL))) {
		ret = -ENOMEM;
		goto err;
	}

	ctlr->hdev = hdev;
	ctlr->ctlr_state = NX_CTLR_STATE_INIT;
	ctlr->rumble_queue_head = NX_CTLR_RUMBLE_QUEUE_SIZE - 1;
	ctlr->rumble_queue_tail = 0;
	hid_set_drvdata(hdev, ctlr);
	mutex_init(&ctlr->output_mutex);
	init_waitqueue_head(&ctlr->wait);
	spin_lock_init(&ctlr->lock);
	ctlr->rumble_queue = alloc_workqueue("hid-nintendo-rumble_wq",
					     WQ_FREEZABLE | WQ_MEM_RECLAIM,
					     0);
	if (!ctlr->rumble_queue) {
		ret = -ENOMEM;
		goto err;
	}
	INIT_WORK(&ctlr->rumble_worker, nx_ctlr_rumble_worker);

	if ((ret = hid_parse(hdev))) {
		hid_err(hdev, "HID parse failed\n");
		goto err_wq;
	}

	/*
	 * Patch the hw version of pro controller/joycons, so applications can
	 * distinguish between the default HID mappings and the mappings defined
	 * by the Linux game controller spec. This is important for the SDL2
	 * library, which has a game controller database, which uses device ids
	 * in combination with version as a key.
	 */
	hdev->version |= 0x8000;

	if ((ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW))) {
		hid_err(hdev, "HW start failed\n");
		goto err_wq;
	}

	if ((ret = hid_hw_open(hdev))) {
		hid_err(hdev, "cannot start hardware I/O\n");
		goto err_stop;
	}

	hid_device_io_start(hdev);

	/* Initialize the controller */
	mutex_lock(&ctlr->output_mutex);

	/* if handshake command fails, assume ble pro controller */
	if (nx_ctlr_device_has_usb(ctlr) &&
	    !nx_ctlr_send_usb(ctlr, NX_CTLR_USB_CMD_HANDSHAKE, HZ)) {
		hid_dbg(hdev, "detected USB controller\n");

		if ((ret = nx_ctlr_send_usb(ctlr, NX_CTLR_USB_CMD_BAUDRATE_3M, HZ))) {
			hid_err(hdev, "Failed to set baudrate; ret=%d\n", ret);
			goto err_mutex;
		}

		if ((ret = nx_ctlr_send_usb(ctlr, NX_CTLR_USB_CMD_HANDSHAKE, HZ))) {
			hid_err(hdev, "Failed handshake; ret=%d\n", ret);
			goto err_mutex;
		}
		/*
		 * Set no timeout (to keep controller in USB mode).
		 * This doesn't send a response, so ignore the timeout.
		 */
		nx_ctlr_send_usb(ctlr, NX_CTLR_USB_CMD_NO_TIMEOUT, HZ/10);
	} else if (nx_ctlr_device_is_chrggrip(ctlr)) {
		hid_err(hdev, "Failed charging grip handshake\n");
		ret = -ETIMEDOUT;
		goto err_mutex;
	}

	if ((ret = nx_ctlr_set_report_mode(ctlr))) {
		hid_err(hdev, "Failed to set report mode; ret=%d\n", ret);
		goto err_mutex;
	}

	/* needed for `ctlr->type` */
	if ((ret = nx_ctlr_request_device_info(ctlr))) {
		hid_err(hdev, "Failed to retrieve controller info; ret=%d\n", ret);
		goto err_mutex;
	}

	if (nx_ctlr_has_joysticks(ctlr)) {
		if (nx_ctlr_request_calibration(ctlr)) {
			/*
			* We can function with default calibration, but it may be
			* inaccurate. Provide a warning, and continue on.
			*/
			hid_warn(hdev, "Analog stick positions may be inaccurate\n");
		}
	}

	if (nx_ctlr_has_imu(ctlr)) {
		if (nx_ctlr_request_imu_calibration(ctlr)) {
			/*
			* We can function with default calibration, but it may be
			* inaccurate. Provide a warning, and continue on.
			*/
			hid_warn(hdev, "Unable to read IMU calibration data\n");

			if ((ret = nx_ctlr_enable_imu(ctlr))) {
				hid_err(hdev, "Failed to enable the IMU; ret=%d\n", ret);
				goto err_mutex;
			}
		}
	}

	if (nx_ctlr_has_rumble(ctlr)) {
		if ((ret = nx_ctlr_enable_rumble(ctlr))) {
			hid_err(hdev, "Failed to enable rumble; ret=%d\n", ret);
			goto err_mutex;
		}

	}

	mutex_unlock(&ctlr->output_mutex);

	if ((ret = nx_ctlr_leds_create(ctlr))) {
		hid_err(hdev, "Failed to create leds; ret=%d\n", ret);
		goto err_close;
	}

	if ((ret = nx_ctlr_power_supply_create(ctlr))) {
		hid_err(hdev, "Failed to create power_supply; ret=%d\n", ret);
		goto err_close;
	}

	if ((ret = nx_ctlr_input_create(ctlr))) {
		hid_err(hdev, "Failed to create input device; ret=%d\n", ret);
		goto err_close;
	}

	ctlr->ctlr_state = NX_CTLR_STATE_READ;

	hid_dbg(hdev, "probe - success\n");

	hid_dbg(hdev, "device_is_left_joycon   = %d\n", nx_ctlr_device_is_left_joycon(ctlr));
	hid_dbg(hdev, "device_is_right_joycon  = %d\n", nx_ctlr_device_is_right_joycon(ctlr));
	hid_dbg(hdev, "device_is_procon        = %d\n", nx_ctlr_device_is_procon(ctlr));
	hid_dbg(hdev, "device_is_chrggrip      = %d\n", nx_ctlr_device_is_chrggrip(ctlr));
	hid_dbg(hdev, "device_is_snescon       = %d\n", nx_ctlr_device_is_snescon(ctlr));
	hid_dbg(hdev, "device_is_gencon        = %d\n", nx_ctlr_device_is_gencon(ctlr));
	hid_dbg(hdev, "device_is_any_joycon    = %d\n", nx_ctlr_device_is_any_joycon(ctlr));
	hid_dbg(hdev, "device_has_usb          = %d\n", nx_ctlr_device_has_usb(ctlr));
	hid_dbg(hdev, "type_is_left_joycon     = %d\n", nx_ctlr_type_is_left_joycon(ctlr));
	hid_dbg(hdev, "type_is_right_joycon    = %d\n", nx_ctlr_type_is_right_joycon(ctlr));
	hid_dbg(hdev, "type_is_procon          = %d\n", nx_ctlr_type_is_procon(ctlr));
	hid_dbg(hdev, "type_is_snescon         = %d\n", nx_ctlr_type_is_snescon(ctlr));
	hid_dbg(hdev, "type_is_gencon          = %d\n", nx_ctlr_type_is_gencon(ctlr));
	hid_dbg(hdev, "type_is_left_nescon     = %d\n", nx_ctlr_type_is_left_nescon(ctlr));
	hid_dbg(hdev, "type_is_right_nescon    = %d\n", nx_ctlr_type_is_right_nescon(ctlr));
	hid_dbg(hdev, "type_has_left_controls  = %d\n", nx_ctlr_type_has_left_controls(ctlr));
	hid_dbg(hdev, "type_has_right_controls = %d\n", nx_ctlr_type_has_right_controls(ctlr));
	hid_dbg(hdev, "type_is_any_joycon      = %d\n", nx_ctlr_type_is_any_joycon(ctlr));
	hid_dbg(hdev, "type_is_any_nescon      = %d\n", nx_ctlr_type_is_any_nescon(ctlr));
	hid_dbg(hdev, "has_imu                 = %d\n", nx_ctlr_has_imu(ctlr));
	hid_dbg(hdev, "has_joysticks           = %d\n", nx_ctlr_has_joysticks(ctlr));
	hid_dbg(hdev, "has_rumble              = %d\n", nx_ctlr_has_rumble(ctlr));

	return 0;

err_mutex:
	mutex_unlock(&ctlr->output_mutex);
err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
err_wq:
	destroy_workqueue(ctlr->rumble_queue);
err:
	hid_err(hdev, "probe - fail = %d\n", ret);
	return ret;
}

static void nintendo_hid_remove(struct hid_device *hdev)
{
	struct nx_ctlr *ctlr = hid_get_drvdata(hdev);
	unsigned long flags;

	hid_dbg(hdev, "remove\n");

	/* Prevent further attempts at sending subcommands. */
	spin_lock_irqsave(&ctlr->lock, flags);
	ctlr->ctlr_state = NX_CTLR_STATE_REMOVED;
	spin_unlock_irqrestore(&ctlr->lock, flags);

	destroy_workqueue(ctlr->rumble_queue);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id nintendo_hid_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_PROCON)        },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_CHRGGRIP)      },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_SNESCON)       },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_GENCON)        },

	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_PROCON)  },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_JOYCONL) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_JOYCONR) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_SNESCON) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_GENCON)  },
	{ }
};
MODULE_DEVICE_TABLE(hid, nintendo_hid_devices);

static struct hid_driver nintendo_hid_driver = {
	.name		= "nintendo",
	.id_table	= nintendo_hid_devices,
	.probe		= nintendo_hid_probe,
	.remove		= nintendo_hid_remove,
	.raw_event	= nintendo_hid_event,
};
module_hid_driver(nintendo_hid_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel J. Ogorchock <djogorchock@gmail.com>");
MODULE_DESCRIPTION("Driver for Nintendo Switch Controllers");
