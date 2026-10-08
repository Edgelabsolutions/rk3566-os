// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kray-SoM-RKC OS: Raspberry Pi's imx335 driver (raspberrypi/linux rpi-6.12.y)
 * backported to the Rockchip 6.1 vendor kernel:
 *  - the CCI register helpers (drivers/media/v4l2-core/v4l2-cci.c, 6.8) and
 *    v4l2_link_freq_to_bitmap() are included here as static functions;
 *  - stream control through s_stream (no enable_streams and no
 *    v4l2_subdev_s_stream_helper() in 6.1), pad init_cfg instead of the
 *    internal init_state, v4l2_subdev_get_pad_format/crop() instead of
 *    v4l2_subdev_state_get_format/crop(), .probe_new instead of .probe;
 *  - the Rockchip CSI-2 D-PHY and ISP drivers call pad operations with a
 *    NULL state; they then use the sensor's active state;
 *  - get_mbus_config (2 or 4 lanes, from the endpoint), g_frame_interval,
 *    Rockchip module information and subdev naming, as in the other ports;
 *  - the I2C driver is called "rpi_imx335", so it can be built next to
 *    Rockchip's own imx335 driver.
 */
/*
 * Sony imx335 Camera Sensor Driver
 *
 * Copyright (C) 2021 Intel Corporation
 */
#include <asm/unaligned.h>

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/rk-camera-module.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* ---- CCI register access helpers, from Linux 6.8 v4l2-cci ---- */
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/types.h>

struct i2c_client;
struct regmap;

/**
 * struct cci_reg_sequence - An individual write from a sequence of CCI writes
 *
 * @reg: Register address, use CCI_REG#() macros to encode reg width
 * @val: Register value
 *
 * Register/value pairs for sequences of writes.
 */
struct cci_reg_sequence {
	u32 reg;
	u64 val;
};

/*
 * Macros to define register address with the register width encoded
 * into the higher bits.
 */
#define CCI_REG_ADDR_MASK		GENMASK(15, 0)
#define CCI_REG_WIDTH_SHIFT		16
#define CCI_REG_WIDTH_MASK		GENMASK(19, 16)
/*
 * Private CCI register flags, for the use of drivers.
 */
#define CCI_REG_PRIVATE_SHIFT		28U
#define CCI_REG_PRIVATE_MASK		GENMASK(31U, CCI_REG_PRIVATE_SHIFT)

#define CCI_REG_WIDTH_BYTES(x)		FIELD_GET(CCI_REG_WIDTH_MASK, x)
#define CCI_REG_WIDTH(x)		(CCI_REG_WIDTH_BYTES(x) << 3)
#define CCI_REG_ADDR(x)			FIELD_GET(CCI_REG_ADDR_MASK, x)
#define CCI_REG_LE			BIT(20)

#define CCI_REG8(x)			((1 << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG16(x)			((2 << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG24(x)			((3 << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG32(x)			((4 << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG64(x)			((8 << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG16_LE(x)			(CCI_REG_LE | (2U << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG24_LE(x)			(CCI_REG_LE | (3U << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG32_LE(x)			(CCI_REG_LE | (4U << CCI_REG_WIDTH_SHIFT) | (x))
#define CCI_REG64_LE(x)			(CCI_REG_LE | (8U << CCI_REG_WIDTH_SHIFT) | (x))

static int cci_read(struct regmap *map, u32 reg, u64 *val, int *err)
{
	bool little_endian;
	unsigned int len;
	u8 buf[8];
	int ret;

	/*
	 * TODO: Fix smatch. Assign *val to 0 here in order to avoid
	 * failing a smatch check on caller when the caller proceeds to
	 * read *val without initialising it on caller's side. *val is set
	 * to a valid value whenever this function returns 0 but smatch
	 * can't figure that out currently.
	 */
	*val = 0;

	if (err && *err)
		return *err;

	little_endian = reg & CCI_REG_LE;
	len = CCI_REG_WIDTH_BYTES(reg);
	reg = CCI_REG_ADDR(reg);

	ret = regmap_bulk_read(map, reg, buf, len);
	if (ret) {
		dev_err(regmap_get_device(map), "Error reading reg 0x%04x: %d\n",
			reg, ret);
		goto out;
	}

	switch (len) {
	case 1:
		*val = buf[0];
		break;
	case 2:
		if (little_endian)
			*val = get_unaligned_le16(buf);
		else
			*val = get_unaligned_be16(buf);
		break;
	case 3:
		if (little_endian)
			*val = get_unaligned_le24(buf);
		else
			*val = get_unaligned_be24(buf);
		break;
	case 4:
		if (little_endian)
			*val = get_unaligned_le32(buf);
		else
			*val = get_unaligned_be32(buf);
		break;
	case 8:
		if (little_endian)
			*val = get_unaligned_le64(buf);
		else
			*val = get_unaligned_be64(buf);
		break;
	default:
		dev_err(regmap_get_device(map), "Error invalid reg-width %u for reg 0x%04x\n",
			len, reg);
		ret = -EINVAL;
		break;
	}

out:
	if (ret && err)
		*err = ret;

	return ret;
}

static int cci_write(struct regmap *map, u32 reg, u64 val, int *err)
{
	bool little_endian;
	unsigned int len;
	u8 buf[8];
	int ret;

	if (err && *err)
		return *err;

	little_endian = reg & CCI_REG_LE;
	len = CCI_REG_WIDTH_BYTES(reg);
	reg = CCI_REG_ADDR(reg);

	switch (len) {
	case 1:
		buf[0] = val;
		break;
	case 2:
		if (little_endian)
			put_unaligned_le16(val, buf);
		else
			put_unaligned_be16(val, buf);
		break;
	case 3:
		if (little_endian)
			put_unaligned_le24(val, buf);
		else
			put_unaligned_be24(val, buf);
		break;
	case 4:
		if (little_endian)
			put_unaligned_le32(val, buf);
		else
			put_unaligned_be32(val, buf);
		break;
	case 8:
		if (little_endian)
			put_unaligned_le64(val, buf);
		else
			put_unaligned_be64(val, buf);
		break;
	default:
		dev_err(regmap_get_device(map), "Error invalid reg-width %u for reg 0x%04x\n",
			len, reg);
		ret = -EINVAL;
		goto out;
	}

	ret = regmap_bulk_write(map, reg, buf, len);
	if (ret)
		dev_err(regmap_get_device(map), "Error writing reg 0x%04x: %d\n",
			reg, ret);

out:
	if (ret && err)
		*err = ret;

	return ret;
}

static int __maybe_unused cci_update_bits(struct regmap *map, u32 reg, u64 mask, u64 val, int *err)
{
	u64 readval;
	int ret;

	ret = cci_read(map, reg, &readval, err);
	if (ret)
		return ret;

	val = (readval & ~mask) | (val & mask);

	return cci_write(map, reg, val, err);
}

static int cci_multi_reg_write(struct regmap *map, const struct cci_reg_sequence *regs,
			unsigned int num_regs, int *err)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_regs; i++) {
		ret = cci_write(map, regs[i].reg, regs[i].val, err);
		if (ret)
			return ret;
	}

	return 0;
}

static struct regmap *devm_cci_regmap_init_i2c(struct i2c_client *client,
					int reg_addr_bits)
{
	struct regmap_config config = {
		.reg_bits = reg_addr_bits,
		.val_bits = 8,
		.reg_format_endian = REGMAP_ENDIAN_BIG,
		.disable_locking = true,
	};

	return devm_regmap_init_i2c(client, &config);
}

/* ---- v4l2_link_freq_to_bitmap(), from Linux 6.8 v4l2-common ---- */
static int imx335_link_freq_to_bitmap(struct device *dev, const u64 *fw_link_freqs,
			     unsigned int num_of_fw_link_freqs,
			     const s64 *driver_link_freqs,
			     unsigned int num_of_driver_link_freqs,
			     unsigned long *bitmap)
{
	unsigned int i;

	*bitmap = 0;

	if (!num_of_fw_link_freqs) {
		dev_err(dev, "no link frequencies in firmware\n");
		return -ENODATA;
	}

	for (i = 0; i < num_of_fw_link_freqs; i++) {
		unsigned int j;

		for (j = 0; j < num_of_driver_link_freqs; j++) {
			if (fw_link_freqs[i] != driver_link_freqs[j])
				continue;

			dev_dbg(dev, "enabling link frequency %lld Hz\n",
				driver_link_freqs[j]);
			*bitmap |= BIT(j);
			break;
		}
	}

	if (!*bitmap) {
		dev_err(dev, "no matching link frequencies found\n");

		dev_dbg(dev, "specified in firmware:\n");
		for (i = 0; i < num_of_fw_link_freqs; i++)
			dev_dbg(dev, "\t%llu Hz\n", fw_link_freqs[i]);

		dev_dbg(dev, "driver supported:\n");
		for (i = 0; i < num_of_driver_link_freqs; i++)
			dev_dbg(dev, "\t%lld Hz\n", driver_link_freqs[i]);

		return -ENOENT;
	}

	return 0;
}

/* Streaming Mode */
#define IMX335_REG_MODE_SELECT		CCI_REG8(0x3000)
#define IMX335_MODE_STANDBY		0x01
#define IMX335_MODE_STREAMING		0x00

/* Group hold register */
#define IMX335_REG_HOLD			CCI_REG8(0x3001)

#define IMX335_REG_MASTER_MODE		CCI_REG8(0x3002)
#define IMX335_REG_BCWAIT_TIME		CCI_REG8(0x300c)
#define IMX335_REG_CPWAIT_TIME		CCI_REG8(0x300d)
#define IMX335_REG_WINMODE		CCI_REG8(0x3018)
#define IMX335_REG_HTRIMMING_START	CCI_REG16_LE(0x302c)
#define IMX335_REG_HNUM			CCI_REG16_LE(0x302e)

/* Lines per frame */
#define IMX335_REG_VMAX			CCI_REG24_LE(0x3030)
#define IMX335_REG_HMAX			CCI_REG16_LE(0x3034)

#define IMX335_REG_OPB_SIZE_V		CCI_REG8(0x304c)
#define IMX335_REG_ADBIT		CCI_REG8(0x3050)
#define IMX335_REG_Y_OUT_SIZE		CCI_REG16_LE(0x3056)

#define IMX335_REG_SHUTTER		CCI_REG24_LE(0x3058)
#define IMX335_EXPOSURE_MIN		1
#define IMX335_SHUTTER_MIN		9
#define IMX335_SHUTTER_MIN_BINNED	17
#define IMX335_EXPOSURE_STEP		1
#define IMX335_EXPOSURE_DEFAULT		0x0648

#define IMX335_REG_AREA2_WIDTH_1	CCI_REG16_LE(0x3072)

#define IMX335_REG_AREA3_ST_ADR_1	CCI_REG16_LE(0x3074)
#define IMX335_REG_AREA3_WIDTH_1	CCI_REG16_LE(0x3076)

/* Analog and Digital gain control */
#define IMX335_REG_GAIN			CCI_REG8(0x30e8)
#define IMX335_AGAIN_MIN		0
#define IMX335_AGAIN_MAX		100
#define IMX335_AGAIN_STEP		1
#define IMX335_AGAIN_DEFAULT		0

/* Vertical flip */
#define IMX335_REG_VREVERSE		CCI_REG8(0x304f)

#define IMX335_REG_TPG_TESTCLKEN	CCI_REG8(0x3148)

#define IMX335_REG_INCLKSEL1		CCI_REG16_LE(0x314c)
#define IMX335_REG_INCLKSEL2		CCI_REG8(0x315a)
#define IMX335_REG_INCLKSEL3		CCI_REG8(0x3168)
#define IMX335_REG_INCLKSEL4		CCI_REG8(0x316a)

#define IMX335_REG_MDBIT		CCI_REG8(0x319d)
#define IMX335_REG_SYSMODE		CCI_REG8(0x319e)

#define IMX335_REG_XVS_XHS_DRV		CCI_REG8(0x31a1)

/* Test pattern generator */
#define IMX335_REG_TPG_DIG_CLP_MODE	CCI_REG8(0x3280)
#define IMX335_REG_TPG_EN_DUOUT		CCI_REG8(0x329c)
#define IMX335_REG_TPG			CCI_REG8(0x329e)
#define IMX335_TPG_ALL_000		0
#define IMX335_TPG_ALL_FFF		1
#define IMX335_TPG_ALL_555		2
#define IMX335_TPG_ALL_AAA		3
#define IMX335_TPG_TOG_555_AAA		4
#define IMX335_TPG_TOG_AAA_555		5
#define IMX335_TPG_TOG_000_555		6
#define IMX335_TPG_TOG_555_000		7
#define IMX335_TPG_TOG_000_FFF		8
#define IMX335_TPG_TOG_FFF_000		9
#define IMX335_TPG_H_COLOR_BARS		10
#define IMX335_TPG_V_COLOR_BARS		11
#define IMX335_REG_TPG_COLORWIDTH	CCI_REG8(0x32a0)

#define IMX335_REG_BLKLEVEL		CCI_REG16_LE(0x3302)

#define IMX335_REG_WRJ_OPEN		CCI_REG8(0x336c)

#define IMX335_REG_ADBIT1		CCI_REG16_LE(0x341c)

/* Chip ID */
#define IMX335_REG_ID			CCI_REG8(0x3912)
#define IMX335_ID			0x00

/* Data Lanes */
#define IMX335_REG_LANEMODE		CCI_REG8(0x3a01)
#define IMX335_2LANE			1
#define IMX335_4LANE			3

#define IMX335_REG_TCLKPOST		CCI_REG16_LE(0x3a18)
#define IMX335_REG_TCLKPREPARE		CCI_REG16_LE(0x3a1a)
#define IMX335_REG_TCLK_TRAIL		CCI_REG16_LE(0x3a1c)
#define IMX335_REG_TCLK_ZERO		CCI_REG16_LE(0x3a1e)
#define IMX335_REG_THS_PREPARE		CCI_REG16_LE(0x3a20)
#define IMX335_REG_THS_ZERO		CCI_REG16_LE(0x3a22)
#define IMX335_REG_THS_TRAIL		CCI_REG16_LE(0x3a24)
#define IMX335_REG_THS_EXIT		CCI_REG16_LE(0x3a26)
#define IMX335_REG_TPLX			CCI_REG16_LE(0x3a28)

/* Input clock rate */
#define IMX335_INCLK_RATE		24000000

/* CSI2 HW configuration */
#define IMX335_LINK_FREQ_594MHz		594000000LL
#define IMX335_LINK_FREQ_445MHz		445500000LL

#define IMX335_NUM_DATA_LANES	4

/* IMX335 native and active pixel array size. */
static const struct v4l2_rect imx335_native_area = {
	.top = 0,
	.left = 0,
	.width = 2696,
	.height = 2044,
};

static const struct v4l2_rect imx335_active_area = {
	.top = 50,
	.left = 36,
	.width = 2624,
	.height = 1944,
};

/**
 * struct imx335_reg_list - imx335 sensor register list
 * @num_of_regs: Number of registers in the list
 * @regs: Pointer to register list
 */
struct imx335_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

static const char * const imx335_supply_name[] = {
	"avdd", /* Analog (2.9V) supply */
	"ovdd", /* Digital I/O (1.8V) supply */
	"dvdd", /* Digital Core (1.2V) supply */
};

enum imx335_scan_mode {
	IMX335_ALL_PIXEL,
	IMX335_2_2_BINNING,
};

/**
 * struct imx335_mode - imx335 sensor mode structure
 * @scan_mode: Configuration scan mode (All pixel / 2x2Binning)
 * @width: Frame width
 * @height: Frame height
 * @code: Format code
 * @hblank: Horizontal blanking in lines
 * @vblank: Vertical blanking in lines
 * @vblank_min: Minimum vertical blanking in lines
 * @vblank_max: Maximum vertical blanking in lines
 * @pclk: Sensor pixel clock
 * @reg_list: Register list for sensor mode
 * @vflip_normal: Register list vflip (normal readout)
 * @vflip_inverted: Register list vflip (inverted readout)
 */
struct imx335_mode {
	enum imx335_scan_mode scan_mode;
	u32 width;
	u32 height;
	u32 code;
	u32 hblank;
	u32 vblank;
	u32 vblank_min;
	u32 vblank_max;
	u64 pclk;
	struct imx335_reg_list reg_list;
	struct imx335_reg_list vflip_normal;
	struct imx335_reg_list vflip_inverted;
};

/**
 * struct imx335 - imx335 sensor device structure
 * @dev: Pointer to generic device
 * @client: Pointer to i2c client
 * @sd: V4L2 sub-device
 * @pad: Media pad. Only one pad supported
 * @reset_gpio: Sensor reset gpio
 * @supplies: Regulator supplies to handle power control
 * @cci: CCI register map
 * @inclk: Sensor input clock
 * @ctrl_handler: V4L2 control handler
 * @link_freq_ctrl: Pointer to link frequency control
 * @pclk_ctrl: Pointer to pixel clock control
 * @hblank_ctrl: Pointer to horizontal blanking control
 * @vblank_ctrl: Pointer to vertical blanking control
 * @vflip: Pointer to vertical flip control
 * @exp_ctrl: Pointer to exposure control
 * @again_ctrl: Pointer to analog gain control
 * @vblank: Vertical blanking in lines
 * @lane_mode: Mode for number of connected data lanes
 * @num_lanes: Number of connected data lanes
 * @cur_mode: Pointer to current selected sensor mode
 * @link_freq_bitmap: Menu bitmap for link_freq_ctrl
 * @cur_mbus_code: Currently selected media bus format code
 * @module_index: Rockchip camera module index
 * @module_facing: Rockchip camera module facing ("back"/"front")
 * @module_name: Rockchip camera module name
 * @len_name: Rockchip camera lens name
 */
struct imx335 {
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx335_supply_name)];
	struct regmap *cci;

	struct clk *inclk;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq_ctrl;
	struct v4l2_ctrl *pclk_ctrl;
	struct v4l2_ctrl *hblank_ctrl;
	struct v4l2_ctrl *vblank_ctrl;
	struct v4l2_ctrl *vflip;
	struct {
		struct v4l2_ctrl *exp_ctrl;
		struct v4l2_ctrl *again_ctrl;
	};
	u32 vblank;
	u32 lane_mode;
	u32 num_lanes;
	const struct imx335_mode *cur_mode;
	unsigned long link_freq_bitmap;
	u32 cur_mbus_code;

	u32 module_index;
	const char *module_facing;
	const char *module_name;
	const char *len_name;
};

static const char * const imx335_tpg_menu[] = {
	"Disabled",
	"All 000h",
	"All FFFh",
	"All 555h",
	"All AAAh",
	"Toggle 555/AAAh",
	"Toggle AAA/555h",
	"Toggle 000/555h",
	"Toggle 555/000h",
	"Toggle 000/FFFh",
	"Toggle FFF/000h",
	"Horizontal color bars",
	"Vertical color bars",
};

static const int imx335_tpg_val[] = {
	IMX335_TPG_ALL_000,
	IMX335_TPG_ALL_000,
	IMX335_TPG_ALL_FFF,
	IMX335_TPG_ALL_555,
	IMX335_TPG_ALL_AAA,
	IMX335_TPG_TOG_555_AAA,
	IMX335_TPG_TOG_AAA_555,
	IMX335_TPG_TOG_000_555,
	IMX335_TPG_TOG_555_000,
	IMX335_TPG_TOG_000_FFF,
	IMX335_TPG_TOG_FFF_000,
	IMX335_TPG_H_COLOR_BARS,
	IMX335_TPG_V_COLOR_BARS,
};

/* Sensor mode registers */
static const struct cci_reg_sequence mode_2592x1944_regs[] = {
	{ IMX335_REG_MODE_SELECT, IMX335_MODE_STANDBY },
	{ IMX335_REG_MASTER_MODE, 0x00 },
	{ IMX335_REG_WINMODE, 0x04 },
	{ IMX335_REG_HMAX, 550 },
	{ IMX335_REG_HTRIMMING_START, 48 },
	{ IMX335_REG_HNUM, 2592 },
	{ IMX335_REG_Y_OUT_SIZE, 1944 },
	{ IMX335_REG_AREA2_WIDTH_1, 40 },
	{ IMX335_REG_AREA3_WIDTH_1, 3928 },
	{ IMX335_REG_OPB_SIZE_V, 0 },
	{ IMX335_REG_XVS_XHS_DRV, 0x00 },
};

static const struct cci_reg_sequence mode_1312x972_regs[] = {
	{ IMX335_REG_MODE_SELECT, IMX335_MODE_STANDBY },
	{ IMX335_REG_MASTER_MODE, 0x00 },
	{ IMX335_REG_WINMODE, 0x01 },
	{ IMX335_REG_HMAX, 275 },
	{ IMX335_REG_HTRIMMING_START, 48 },
	{ IMX335_REG_HNUM, 2600 },
	{ IMX335_REG_Y_OUT_SIZE, 972 },
	{ IMX335_REG_AREA2_WIDTH_1, 48 },
	{ IMX335_REG_AREA3_WIDTH_1, 3936 },
	{ IMX335_REG_OPB_SIZE_V, 0 },
	{ IMX335_REG_XVS_XHS_DRV, 0x00 },
	{ CCI_REG8(0x3300), 1 }, /* TCYCLE */
	{ CCI_REG8(0x3199), 0x30 }, /* HADD/VADD */
};

static const struct cci_reg_sequence imx335_common_regs[] = {
	{ CCI_REG8(0x3288), 0x21 },
	{ CCI_REG8(0x328a), 0x02 },
	{ CCI_REG8(0x3414), 0x05 },
	{ CCI_REG8(0x3416), 0x18 },
	{ CCI_REG8(0x3648), 0x01 },
	{ CCI_REG8(0x364a), 0x04 },
	{ CCI_REG8(0x364c), 0x04 },
	{ CCI_REG8(0x3678), 0x01 },
	{ CCI_REG8(0x367c), 0x31 },
	{ CCI_REG8(0x367e), 0x31 },
	{ CCI_REG8(0x3706), 0x10 },
	{ CCI_REG8(0x3708), 0x03 },
	{ CCI_REG8(0x3714), 0x02 },
	{ CCI_REG8(0x3715), 0x02 },
	{ CCI_REG8(0x3716), 0x01 },
	{ CCI_REG8(0x3717), 0x03 },
	{ CCI_REG8(0x371c), 0x3d },
	{ CCI_REG8(0x371d), 0x3f },
	{ CCI_REG8(0x372c), 0x00 },
	{ CCI_REG8(0x372d), 0x00 },
	{ CCI_REG8(0x372e), 0x46 },
	{ CCI_REG8(0x372f), 0x00 },
	{ CCI_REG8(0x3730), 0x89 },
	{ CCI_REG8(0x3731), 0x00 },
	{ CCI_REG8(0x3732), 0x08 },
	{ CCI_REG8(0x3733), 0x01 },
	{ CCI_REG8(0x3734), 0xfe },
	{ CCI_REG8(0x3735), 0x05 },
	{ CCI_REG8(0x3740), 0x02 },
	{ CCI_REG8(0x375d), 0x00 },
	{ CCI_REG8(0x375e), 0x00 },
	{ CCI_REG8(0x375f), 0x11 },
	{ CCI_REG8(0x3760), 0x01 },
	{ CCI_REG8(0x3768), 0x1b },
	{ CCI_REG8(0x3769), 0x1b },
	{ CCI_REG8(0x376a), 0x1b },
	{ CCI_REG8(0x376b), 0x1b },
	{ CCI_REG8(0x376c), 0x1a },
	{ CCI_REG8(0x376d), 0x17 },
	{ CCI_REG8(0x376e), 0x0f },
	{ CCI_REG8(0x3776), 0x00 },
	{ CCI_REG8(0x3777), 0x00 },
	{ CCI_REG8(0x3778), 0x46 },
	{ CCI_REG8(0x3779), 0x00 },
	{ CCI_REG8(0x377a), 0x89 },
	{ CCI_REG8(0x377b), 0x00 },
	{ CCI_REG8(0x377c), 0x08 },
	{ CCI_REG8(0x377d), 0x01 },
	{ CCI_REG8(0x377e), 0x23 },
	{ CCI_REG8(0x377f), 0x02 },
	{ CCI_REG8(0x3780), 0xd9 },
	{ CCI_REG8(0x3781), 0x03 },
	{ CCI_REG8(0x3782), 0xf5 },
	{ CCI_REG8(0x3783), 0x06 },
	{ CCI_REG8(0x3784), 0xa5 },
	{ CCI_REG8(0x3788), 0x0f },
	{ CCI_REG8(0x378a), 0xd9 },
	{ CCI_REG8(0x378b), 0x03 },
	{ CCI_REG8(0x378c), 0xeb },
	{ CCI_REG8(0x378d), 0x05 },
	{ CCI_REG8(0x378e), 0x87 },
	{ CCI_REG8(0x378f), 0x06 },
	{ CCI_REG8(0x3790), 0xf5 },
	{ CCI_REG8(0x3792), 0x43 },
	{ CCI_REG8(0x3794), 0x7a },
	{ CCI_REG8(0x3796), 0xa1 },
	{ CCI_REG8(0x37b0), 0x36 },
	{ CCI_REG8(0x3a00), 0x00 },
};

static const struct cci_reg_sequence mode_2592x1944_vflip_normal[] = {
	{ IMX335_REG_AREA3_ST_ADR_1, 176 },

	/* Undocumented V-Flip related registers on Page 55 of datasheet. */
	{ CCI_REG8(0x3081), 0x02, },
	{ CCI_REG8(0x3083), 0x02, },
	{ CCI_REG16_LE(0x30b6), 0x00 },
	{ CCI_REG16_LE(0x3116), 0x08 },
};

static const struct cci_reg_sequence mode_2592x1944_vflip_inverted[] = {
	{ IMX335_REG_AREA3_ST_ADR_1, 4112 },

	/* Undocumented V-Flip related registers on Page 55 of datasheet. */
	{ CCI_REG8(0x3081), 0xfe, },
	{ CCI_REG8(0x3083), 0xfe, },
	{ CCI_REG16_LE(0x30b6), 0x1fa },
	{ CCI_REG16_LE(0x3116), 0x002 },
};

static const struct cci_reg_sequence mode_1312x972_vflip_normal[] = {
	{ IMX335_REG_AREA3_ST_ADR_1, 176 },

	/* Undocumented */
	{ CCI_REG8(0x3078), 0x04 },
	{ CCI_REG8(0x3079), 0xfd },
	{ CCI_REG8(0x307a), 0x04 },
	{ CCI_REG8(0x307b), 0xfe },
	{ CCI_REG8(0x307c), 0x04 },
	{ CCI_REG8(0x307d), 0xfb },
	{ CCI_REG8(0x307e), 0x04 },
	{ CCI_REG8(0x307f), 0x02 },
	{ CCI_REG8(0x3080), 0x04 },
	{ CCI_REG8(0x3081), 0xfd },
	{ CCI_REG8(0x3082), 0x04 },
	{ CCI_REG8(0x3083), 0xfe },
	{ CCI_REG8(0x3084), 0x04 },
	{ CCI_REG8(0x3085), 0xfb },
	{ CCI_REG8(0x3086), 0x04 },
	{ CCI_REG8(0x3087), 0x02 },
	{ CCI_REG8(0x30a4), 0x77 },
	{ CCI_REG8(0x30a8), 0x20 },
	{ CCI_REG8(0x30a9), 0x00 },
	{ CCI_REG8(0x30ac), 0x08 },
	{ CCI_REG8(0x30ad), 0x08 },
	{ CCI_REG8(0x30b0), 0x20 },
	{ CCI_REG8(0x30b1), 0x00 },
	{ CCI_REG8(0x30b4), 0x10 },
	{ CCI_REG8(0x30b5), 0x10 },
	{ CCI_REG16_LE(0x30b6), 0x00 },
	{ CCI_REG16_LE(0x3112), 0x10 },
	{ CCI_REG16_LE(0x3116), 0x10 },
};

static const struct cci_reg_sequence mode_1312x972_vflip_inverted[] = {
	{ IMX335_REG_AREA3_ST_ADR_1, 4112 },

	/* Undocumented */
	{ CCI_REG8(0x3078), 0x04 },
	{ CCI_REG8(0x3079), 0xfd },
	{ CCI_REG8(0x307a), 0x04 },
	{ CCI_REG8(0x307b), 0xfe },
	{ CCI_REG8(0x307c), 0x04 },
	{ CCI_REG8(0x307d), 0xfb },
	{ CCI_REG8(0x307e), 0x04 },
	{ CCI_REG8(0x307f), 0x02 },
	{ CCI_REG8(0x3080), 0xfc },
	{ CCI_REG8(0x3081), 0x05 },
	{ CCI_REG8(0x3082), 0xfc },
	{ CCI_REG8(0x3083), 0x02 },
	{ CCI_REG8(0x3084), 0xfc },
	{ CCI_REG8(0x3085), 0x03 },
	{ CCI_REG8(0x3086), 0xfc },
	{ CCI_REG8(0x3087), 0xfe },
	{ CCI_REG8(0x30a4), 0x77 },
	{ CCI_REG8(0x30a8), 0x20 },
	{ CCI_REG8(0x30a9), 0x00 },
	{ CCI_REG8(0x30ac), 0x08 },
	{ CCI_REG8(0x30ad), 0x78 },
	{ CCI_REG8(0x30b0), 0x20 },
	{ CCI_REG8(0x30b1), 0x00 },
	{ CCI_REG8(0x30b4), 0x10 },
	{ CCI_REG8(0x30b5), 0x70 },
	{ CCI_REG16_LE(0x30b6), 0x01f2 },
	{ CCI_REG16_LE(0x3112), 0x10 },
	{ CCI_REG16_LE(0x3116), 0x02 },
};

static const struct cci_reg_sequence mipi_data_rate_1188Mbps[] = {
	{ IMX335_REG_BCWAIT_TIME, 0x3b },
	{ IMX335_REG_CPWAIT_TIME, 0x2a },
	{ IMX335_REG_INCLKSEL1, 0x00c6 },
	{ IMX335_REG_INCLKSEL2, 0x02 },
	{ IMX335_REG_INCLKSEL3, 0xa0 },
	{ IMX335_REG_INCLKSEL4, 0x7e },
	{ IMX335_REG_SYSMODE, 0x01 },
	{ IMX335_REG_TCLKPOST, 0x8f },
	{ IMX335_REG_TCLKPREPARE, 0x4f },
	{ IMX335_REG_TCLK_TRAIL, 0x47 },
	{ IMX335_REG_TCLK_ZERO, 0x0137 },
	{ IMX335_REG_THS_PREPARE, 0x4f },
	{ IMX335_REG_THS_ZERO,  0x87 },
	{ IMX335_REG_THS_TRAIL, 0x4f },
	{ IMX335_REG_THS_EXIT, 0x7f },
	{ IMX335_REG_TPLX, 0x3f },
};

static const struct cci_reg_sequence mipi_data_rate_891Mbps[] = {
	{ IMX335_REG_BCWAIT_TIME, 0x3b },
	{ IMX335_REG_CPWAIT_TIME, 0x2a },
	{ IMX335_REG_INCLKSEL1, 0x0129 },
	{ IMX335_REG_INCLKSEL2, 0x06 },
	{ IMX335_REG_INCLKSEL3, 0xa0 },
	{ IMX335_REG_INCLKSEL4, 0x7e },
	{ IMX335_REG_SYSMODE, 0x02 },
	{ IMX335_REG_TCLKPOST, 0x7f },
	{ IMX335_REG_TCLKPREPARE, 0x37 },
	{ IMX335_REG_TCLK_TRAIL, 0x37 },
	{ IMX335_REG_TCLK_ZERO, 0xf7 },
	{ IMX335_REG_THS_PREPARE, 0x3f },
	{ IMX335_REG_THS_ZERO, 0x6f },
	{ IMX335_REG_THS_TRAIL, 0x3f },
	{ IMX335_REG_THS_EXIT, 0x5f },
	{ IMX335_REG_TPLX, 0x2f },
};

static const s64 link_freq[] = {
	/* Corresponds to 1188Mbps data lane rate */
	IMX335_LINK_FREQ_594MHz,
	/* Corresponds to 891Mbps data lane rate */
	IMX335_LINK_FREQ_445MHz,
};

static const struct imx335_reg_list link_freq_reglist[] = {
	{
		.num_of_regs = ARRAY_SIZE(mipi_data_rate_1188Mbps),
		.regs = mipi_data_rate_1188Mbps,
	},
	{
		.num_of_regs = ARRAY_SIZE(mipi_data_rate_891Mbps),
		.regs = mipi_data_rate_891Mbps,
	},
};

static const u32 imx335_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB12_1X12,
	MEDIA_BUS_FMT_SRGGB10_1X10,
};

/* Supported sensor mode configurations */
static const struct imx335_mode supported_modes[] = {
	{
		.scan_mode = IMX335_ALL_PIXEL,
		.width = 2592,
		.height = 1944,
		.hblank = 342,
		.vblank = 2556,
		.vblank_min = 2556,
		.vblank_max = 133060,
		.pclk = 396000000,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2592x1944_regs),
			.regs = mode_2592x1944_regs,
		},
		.vflip_normal = {
			.num_of_regs = ARRAY_SIZE(mode_2592x1944_vflip_normal),
			.regs = mode_2592x1944_vflip_normal,
		},
		.vflip_inverted = {
			.num_of_regs = ARRAY_SIZE(mode_2592x1944_vflip_inverted),
			.regs = mode_2592x1944_vflip_inverted,
		}
	}, {
		.scan_mode = IMX335_2_2_BINNING,
		.width = 1312,
		.height = 972,
		.hblank = 155,
		.vblank = 3528,
		.vblank_min = 3528,
		.vblank_max = 133060,
		.pclk = 396000000,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1312x972_regs),
			.regs = mode_1312x972_regs,
		},
		.vflip_normal = {
			.num_of_regs = ARRAY_SIZE(mode_1312x972_vflip_normal),
			.regs = mode_1312x972_vflip_normal,
		},
		.vflip_inverted = {
			.num_of_regs = ARRAY_SIZE(mode_1312x972_vflip_inverted),
			.regs = mode_1312x972_vflip_inverted,
		},
	},
};

/**
 * to_imx335() - imx335 V4L2 sub-device to imx335 device.
 * @subdev: pointer to imx335 V4L2 sub-device
 *
 * Return: pointer to imx335 device
 */
static inline struct imx335 *to_imx335(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct imx335, sd);
}

/**
 * imx335_update_controls() - Update control ranges based on streaming mode
 * @imx335: pointer to imx335 device
 * @mode: pointer to imx335_mode sensor mode
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_update_controls(struct imx335 *imx335,
				  const struct imx335_mode *mode)
{
	int ret;

	ret = __v4l2_ctrl_s_ctrl(imx335->link_freq_ctrl,
				 __ffs(imx335->link_freq_bitmap));
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(imx335->hblank_ctrl, mode->hblank,
				       mode->hblank, 1, mode->hblank);
	if (ret)
		return ret;

	return __v4l2_ctrl_modify_range(imx335->vblank_ctrl, mode->vblank_min,
					mode->vblank_max, 1, mode->vblank);
}

/**
 * imx335_update_exp_gain() - Set updated exposure and gain
 * @imx335: pointer to imx335 device
 * @exposure: updated exposure value
 * @gain: updated analog gain value
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_update_exp_gain(struct imx335 *imx335, u32 exposure, u32 gain)
{
	u32 lpfr, shutter;
	int ret_hold;
	int ret = 0;

	lpfr = imx335->vblank + imx335->cur_mode->height;
	shutter = lpfr - exposure;

	dev_dbg(imx335->dev, "Set exp %u, analog gain %u, shutter %u, lpfr %u\n",
		exposure, gain, shutter, lpfr);

	cci_write(imx335->cci, IMX335_REG_HOLD, 1, &ret);
	cci_write(imx335->cci, IMX335_REG_VMAX, lpfr, &ret);
	cci_write(imx335->cci, IMX335_REG_SHUTTER, shutter, &ret);
	cci_write(imx335->cci, IMX335_REG_GAIN, gain, &ret);
	/*
	 * Unconditionally attempt to release the hold, but track the
	 * error if the unhold itself fails.
	 */
	ret_hold = cci_write(imx335->cci, IMX335_REG_HOLD, 0, NULL);
	if (ret_hold)
		ret = ret_hold;

	return ret;
}

static int imx335_update_vertical_flip(struct imx335 *imx335, u32 vflip)
{
	int ret = 0;

	if (vflip)
		cci_multi_reg_write(imx335->cci,
				    imx335->cur_mode->vflip_inverted.regs,
				    imx335->cur_mode->vflip_inverted.num_of_regs,
				    &ret);
	else
		cci_multi_reg_write(imx335->cci,
				    imx335->cur_mode->vflip_normal.regs,
				    imx335->cur_mode->vflip_normal.num_of_regs,
				    &ret);
	if (ret)
		return ret;

	return cci_write(imx335->cci, IMX335_REG_VREVERSE, vflip, NULL);
}

static int imx335_update_test_pattern(struct imx335 *imx335, u32 pattern_index)
{
	int ret = 0;

	if (pattern_index >= ARRAY_SIZE(imx335_tpg_val))
		return -EINVAL;

	if (pattern_index) {
		const struct cci_reg_sequence tpg_enable_regs[] = {
			{ IMX335_REG_TPG_TESTCLKEN, 0x10 },
			{ IMX335_REG_TPG_DIG_CLP_MODE, 0x00 },
			{ IMX335_REG_TPG_EN_DUOUT, 0x01 },
			{ IMX335_REG_TPG_COLORWIDTH, 0x11 },
			{ IMX335_REG_BLKLEVEL, 0x00 },
			{ IMX335_REG_WRJ_OPEN, 0x00 },
		};

		cci_write(imx335->cci, IMX335_REG_TPG,
			  imx335_tpg_val[pattern_index], &ret);

		cci_multi_reg_write(imx335->cci, tpg_enable_regs,
				    ARRAY_SIZE(tpg_enable_regs), &ret);
	} else {
		const struct cci_reg_sequence tpg_disable_regs[] = {
			{ IMX335_REG_TPG_TESTCLKEN, 0x00 },
			{ IMX335_REG_TPG_DIG_CLP_MODE, 0x01 },
			{ IMX335_REG_TPG_EN_DUOUT, 0x00 },
			{ IMX335_REG_TPG_COLORWIDTH, 0x10 },
			{ IMX335_REG_BLKLEVEL, 0x32 },
			{ IMX335_REG_WRJ_OPEN, 0x01 },
		};

		cci_multi_reg_write(imx335->cci, tpg_disable_regs,
				    ARRAY_SIZE(tpg_disable_regs), &ret);
	}

	return ret;
}

/**
 * imx335_set_ctrl() - Set subdevice control
 * @ctrl: pointer to v4l2_ctrl structure
 *
 * Supported controls:
 * - V4L2_CID_VBLANK
 * - cluster controls:
 *   - V4L2_CID_ANALOGUE_GAIN
 *   - V4L2_CID_EXPOSURE
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx335 *imx335 =
		container_of(ctrl->handler, struct imx335, ctrl_handler);
	u32 analog_gain;
	u32 exposure;
	int ret;

	/* Propagate change of current control to all related controls */
	if (ctrl->id == V4L2_CID_VBLANK) {
		u32 shutter_min = IMX335_SHUTTER_MIN;
		u32 lpfr;

		imx335->vblank = imx335->vblank_ctrl->val;
		lpfr = imx335->vblank + imx335->cur_mode->height;

		dev_dbg(imx335->dev, "Received vblank %u, new lpfr %u\n",
			imx335->vblank, lpfr);

		if (imx335->cur_mode->scan_mode == IMX335_2_2_BINNING)
			shutter_min = IMX335_SHUTTER_MIN_BINNED;

		ret = __v4l2_ctrl_modify_range(imx335->exp_ctrl,
					       IMX335_EXPOSURE_MIN,
					       lpfr - shutter_min, 1,
					       IMX335_EXPOSURE_DEFAULT);
		if (ret)
			return ret;
	}

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming.
	 */
	if (pm_runtime_get_if_in_use(imx335->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		exposure = imx335->exp_ctrl->val;
		analog_gain = imx335->again_ctrl->val;

		ret = imx335_update_exp_gain(imx335, exposure, analog_gain);

		break;
	case V4L2_CID_EXPOSURE:
		exposure = ctrl->val;
		analog_gain = imx335->again_ctrl->val;

		dev_dbg(imx335->dev, "Received exp %u, analog gain %u\n",
			exposure, analog_gain);

		ret = imx335_update_exp_gain(imx335, exposure, analog_gain);

		break;
	case V4L2_CID_VFLIP:
		ret = imx335_update_vertical_flip(imx335, ctrl->val);

		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx335_update_test_pattern(imx335, ctrl->val);

		break;
	default:
		dev_err(imx335->dev, "Invalid control %d\n", ctrl->id);
		ret = -EINVAL;
	}

	pm_runtime_put(imx335->dev);

	return ret;
}

/* V4l2 subdevice control ops*/
static const struct v4l2_ctrl_ops imx335_ctrl_ops = {
	.s_ctrl = imx335_set_ctrl,
};

static int imx335_get_format_code(struct imx335 *imx335, u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(imx335_mbus_codes); i++) {
		if (imx335_mbus_codes[i] == code)
			return imx335_mbus_codes[i];
	}

	return imx335_mbus_codes[0];
}

/**
 * imx335_enum_mbus_code() - Enumerate V4L2 sub-device mbus codes
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @code: V4L2 sub-device code enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(imx335_mbus_codes))
		return -EINVAL;

	code->code = imx335_mbus_codes[code->index];

	return 0;
}

/**
 * imx335_enum_frame_size() - Enumerate V4L2 sub-device frame sizes
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fsize: V4L2 sub-device size enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fsize)
{
	struct imx335 *imx335 = to_imx335(sd);
	u32 code;

	if (fsize->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	code = imx335_get_format_code(imx335, fsize->code);
	if (fsize->code != code)
		return -EINVAL;

	fsize->min_width = supported_modes[fsize->index].width;
	fsize->max_width = fsize->min_width;
	fsize->min_height = supported_modes[fsize->index].height;
	fsize->max_height = fsize->min_height;

	return 0;
}

/**
 * imx335_fill_pad_format() - Fill subdevice pad format
 *                            from selected sensor mode
 * @imx335: pointer to imx335 device
 * @mode: pointer to imx335_mode sensor mode
 * @fmt: V4L2 sub-device format need to be filled
 */
static void imx335_fill_pad_format(struct imx335 *imx335,
				   const struct imx335_mode *mode,
				   struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = imx335->cur_mbus_code;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;
}

/**
 * imx335_set_pad_format() - Set subdevice pad format
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx335 *imx335 = to_imx335(sd);
	struct v4l2_mbus_framefmt *format;
	const struct imx335_mode *mode;
	struct v4l2_rect *crop;
	int i, ret = 0;

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	for (i = 0; i < ARRAY_SIZE(imx335_mbus_codes); i++) {
		if (imx335_mbus_codes[i] == fmt->format.code)
			imx335->cur_mbus_code = imx335_mbus_codes[i];
	}

	imx335_fill_pad_format(imx335, mode, fmt);

	format = v4l2_subdev_get_pad_format(sd, sd_state, fmt->pad);
	*format = fmt->format;

	crop = v4l2_subdev_get_pad_crop(sd, sd_state, fmt->pad);
	crop->width = fmt->format.width;
	crop->height = fmt->format.height;
	if (mode->scan_mode == IMX335_2_2_BINNING) {
		crop->width *= 2;
		crop->height *= 2;
	}
	crop->left = (imx335_native_area.width - crop->width) / 2;
	crop->top = (imx335_native_area.height - crop->height) / 2;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ret = imx335_update_controls(imx335, mode);
		if (!ret)
			imx335->cur_mode = mode;
	}

	return ret;
}

/**
 * imx335_init_state() - Initialize sub-device state
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct imx335 *imx335 = to_imx335(sd);
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	imx335_fill_pad_format(imx335, &supported_modes[0], &fmt);

	__v4l2_ctrl_modify_range(imx335->link_freq_ctrl, 0,
				 __fls(imx335->link_freq_bitmap),
				 ~(imx335->link_freq_bitmap),
				 __ffs(imx335->link_freq_bitmap));

	return imx335_set_pad_format(sd, sd_state, &fmt);
}

/**
 * imx335_get_selection() - Selection API
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @sel: V4L2 selection info
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_get_pad_crop(sd, sd_state, 0);

		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = imx335_native_area;
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = imx335_active_area;
		return 0;
	}

	return -EINVAL;
}

static int imx335_set_framefmt(struct imx335 *imx335)
{
	/*
	 * In the all-pixel scan mode the AD conversion shall match the output
	 * bit width requested.
	 *
	 * However, when 2/2 binning is enabled, the AD conversion is always
	 * 10-bit, so we ensure ADBIT is clear and ADBIT1 is assigned 0x1ff.
	 * That's as much as the documentation gives us...
	 */
	int ret = 0;
	u8 bpp = imx335->cur_mbus_code == MEDIA_BUS_FMT_SRGGB10_1X10 ? 10 : 12;
	u8 ad_conv = bpp;

	/* Start with the output mode */
	cci_write(imx335->cci, IMX335_REG_MDBIT, bpp == 12, &ret);

	/* Enforce 10 bit AD on binning modes */
	if (imx335->cur_mode->scan_mode == IMX335_2_2_BINNING)
		ad_conv = 10;

	/* AD Conversion configuration */
	if (ad_conv == 10) {
		cci_write(imx335->cci, IMX335_REG_ADBIT, 0x00, &ret);
		cci_write(imx335->cci, IMX335_REG_ADBIT1, 0x1ff, &ret);
	} else { /* 12 bit AD Conversion */
		cci_write(imx335->cci, IMX335_REG_ADBIT, 0x01, &ret);
		cci_write(imx335->cci, IMX335_REG_ADBIT1, 0x47, &ret);
	}

	return ret;
}

/**
 * imx335_enable_streams() - Enable sensor streams
 * @sd: V4L2 subdevice
 * @state: V4L2 subdevice state
 * @pad: The pad to enable
 * @streams_mask: Bitmask of streams to enable
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx335 *imx335 = to_imx335(sd);
	const struct imx335_reg_list *reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(imx335->dev);
	if (ret < 0)
		return ret;

	/* Setup PLL */
	reg_list = &link_freq_reglist[__ffs(imx335->link_freq_bitmap)];
	ret = cci_multi_reg_write(imx335->cci, reg_list->regs,
				  reg_list->num_of_regs, NULL);
	if (ret) {
		dev_err(imx335->dev, "%s failed to set plls\n", __func__);
		goto err_rpm_put;
	}

	/* Write sensor mode registers */
	reg_list = &imx335->cur_mode->reg_list;
	ret = cci_multi_reg_write(imx335->cci, reg_list->regs,
				  reg_list->num_of_regs, NULL);
	if (ret) {
		dev_err(imx335->dev, "fail to write initial registers\n");
		goto err_rpm_put;
	}

	/* Write sensor common registers */
	ret = cci_multi_reg_write(imx335->cci, imx335_common_regs,
				  ARRAY_SIZE(imx335_common_regs), NULL);
	if (ret) {
		dev_err(imx335->dev, "fail to write initial registers\n");
		goto err_rpm_put;
	}

	ret = imx335_set_framefmt(imx335);
	if (ret) {
		dev_err(imx335->dev, "%s failed to set frame format: %d\n",
			__func__, ret);
		goto err_rpm_put;
	}

	/* Configure lanes */
	ret = cci_write(imx335->cci, IMX335_REG_LANEMODE,
			imx335->lane_mode, NULL);
	if (ret)
		goto err_rpm_put;

	/* Setup handler will write actual exposure and gain */
	ret =  __v4l2_ctrl_handler_setup(imx335->sd.ctrl_handler);
	if (ret) {
		dev_err(imx335->dev, "fail to setup handler\n");
		goto err_rpm_put;
	}

	/* Start streaming */
	ret = cci_write(imx335->cci, IMX335_REG_MODE_SELECT,
			IMX335_MODE_STREAMING, NULL);
	if (ret) {
		dev_err(imx335->dev, "fail to start streaming\n");
		goto err_rpm_put;
	}

	/* Initial regulator stabilization period */
	usleep_range(18000, 20000);

	return 0;

err_rpm_put:
	pm_runtime_put(imx335->dev);

	return ret;
}

/**
 * imx335_disable_streams() - Disable sensor streams
 * @sd: V4L2 subdevice
 * @state: V4L2 subdevice state
 * @pad: The pad to disable
 * @streams_mask: Bitmask of streams to disable
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx335 *imx335 = to_imx335(sd);
	int ret;

	ret = cci_write(imx335->cci, IMX335_REG_MODE_SELECT,
			IMX335_MODE_STANDBY, NULL);
	pm_runtime_put(imx335->dev);

	return ret;
}

/**
 * imx335_detect() - Detect imx335 sensor
 * @imx335: pointer to imx335 device
 *
 * Return: 0 if successful, -EIO if sensor id does not match
 */
static int imx335_detect(struct imx335 *imx335)
{
	int ret;
	u64 val;

	ret = cci_read(imx335->cci, IMX335_REG_ID, &val, NULL);
	if (ret)
		return ret;

	if (val != IMX335_ID) {
		dev_err(imx335->dev, "chip id mismatch: %x!=%llx\n",
			IMX335_ID, val);
		return -ENXIO;
	}

	return 0;
}

/**
 * imx335_parse_hw_config() - Parse HW configuration and check if supported
 * @imx335: pointer to imx335 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_parse_hw_config(struct imx335 *imx335)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx335->dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep;
	unsigned long rate;
	unsigned int i;
	int ret;

	if (!fwnode)
		return -ENXIO;

	/* Request optional reset pin */
	imx335->reset_gpio = devm_gpiod_get_optional(imx335->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx335->reset_gpio)) {
		dev_err(imx335->dev, "failed to get reset gpio %ld\n",
			PTR_ERR(imx335->reset_gpio));
		return PTR_ERR(imx335->reset_gpio);
	}

	for (i = 0; i < ARRAY_SIZE(imx335_supply_name); i++)
		imx335->supplies[i].supply = imx335_supply_name[i];

	ret = devm_regulator_bulk_get(imx335->dev,
				      ARRAY_SIZE(imx335_supply_name),
				      imx335->supplies);
	if (ret) {
		dev_err(imx335->dev, "Failed to get regulators\n");
		return ret;
	}

	/* Get sensor input clock */
	imx335->inclk = devm_clk_get(imx335->dev, NULL);
	if (IS_ERR(imx335->inclk)) {
		dev_err(imx335->dev, "could not get inclk\n");
		return PTR_ERR(imx335->inclk);
	}

	rate = clk_get_rate(imx335->inclk);
	if (rate != IMX335_INCLK_RATE) {
		dev_err(imx335->dev, "inclk frequency mismatch\n");
		return -EINVAL;
	}

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep) {
		dev_err(imx335->dev, "Failed to get next endpoint\n");
		return -ENXIO;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	switch (bus_cfg.bus.mipi_csi2.num_data_lanes) {
	case 2:
		imx335->lane_mode = IMX335_2LANE;
		imx335->num_lanes = 2;
		break;
	case 4:
		imx335->lane_mode = IMX335_4LANE;
		imx335->num_lanes = 4;
		break;
	default:
		dev_err(imx335->dev,
			"number of CSI2 data lanes %d is not supported\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	ret = imx335_link_freq_to_bitmap(imx335->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       link_freq, ARRAY_SIZE(link_freq),
				       &imx335->link_freq_bitmap);

done_endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

/* ---- Rockchip glue ---- */

/* Linux 6.1 has no enable_streams: s_stream on the active state. */
static int imx335_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct v4l2_subdev_state *state;
	int ret;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	ret = enable ? imx335_enable_streams(sd, state, 0, BIT(0)) :
		       imx335_disable_streams(sd, state, 0, BIT(0));
	v4l2_subdev_unlock_state(state);

	return ret;
}

/* The Rockchip CSI-2 D-PHY and ISP drivers pass a NULL state. */
#define IMX335_RK_STATE_WRAP(name, fn, type)				\
static int name(struct v4l2_subdev *sd,					\
		struct v4l2_subdev_state *state, type *arg)		\
{									\
	int ret;							\
									\
	if (state)							\
		return fn(sd, state, arg);				\
	state = v4l2_subdev_lock_and_get_active_state(sd);		\
	ret = fn(sd, state, arg);					\
	v4l2_subdev_unlock_state(state);				\
	return ret;							\
}

IMX335_RK_STATE_WRAP(imx335_get_fmt_rk, v4l2_subdev_get_fmt, struct v4l2_subdev_format)
IMX335_RK_STATE_WRAP(imx335_set_fmt_rk, imx335_set_pad_format, struct v4l2_subdev_format)
IMX335_RK_STATE_WRAP(imx335_get_selection_rk, imx335_get_selection, struct v4l2_subdev_selection)
IMX335_RK_STATE_WRAP(imx335_enum_frame_size_rk, imx335_enum_frame_size, struct v4l2_subdev_frame_size_enum)
IMX335_RK_STATE_WRAP(imx335_enum_mbus_code_rk, imx335_enum_mbus_code, struct v4l2_subdev_mbus_code_enum)

static int imx335_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				  struct v4l2_mbus_config *config)
{
	struct imx335 *imx335 = to_imx335(sd);

	config->type = V4L2_MBUS_CSI2_DPHY;
	config->bus.mipi_csi2.num_data_lanes = imx335->num_lanes;

	return 0;
}

static int imx335_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx335 *imx335 = to_imx335(sd);
	struct v4l2_subdev_state *state;
	const struct v4l2_mbus_framefmt *fmt;
	u64 pixels;
	s64 pixel_rate;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	fmt = v4l2_subdev_get_pad_format(sd, state, 0);
	pixel_rate = *imx335->pclk_ctrl->p_cur.p_s64;
	pixels = (u64)(fmt->width + imx335->hblank_ctrl->cur.val) *
		 (fmt->height + imx335->vblank_ctrl->cur.val);
	v4l2_subdev_unlock_state(state);

	/* Interval in microseconds, enough for the frame rate the ISP shows. */
	fi->interval.numerator = pixel_rate ? div64_u64(pixels * 1000000, pixel_rate) : 1;
	fi->interval.denominator = 1000000;

	return 0;
}

static long imx335_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct imx335 *imx335 = to_imx335(sd);
	struct rkmodule_inf *inf;
	struct rkmodule_hdr_cfg *hdr;

	switch (cmd) {
	case RKMODULE_GET_HDR_CFG:
		hdr = arg;
		memset(hdr, 0, sizeof(*hdr));
		hdr->hdr_mode = NO_HDR;
		return 0;
	case RKMODULE_SET_HDR_CFG:
		hdr = arg;
		return hdr->hdr_mode == NO_HDR ? 0 : -EINVAL;
	case RKMODULE_GET_MODULE_INFO:
		inf = arg;
		memset(inf, 0, sizeof(*inf));
		strscpy(inf->base.sensor, "imx335", sizeof(inf->base.sensor));
		strscpy(inf->base.module, imx335->module_name, sizeof(inf->base.module));
		strscpy(inf->base.lens, imx335->len_name, sizeof(inf->base.lens));
		return 0;
	default:
		return -ENOIOCTLCMD;
	}
}

/* V4l2 subdevice ops */
static const struct v4l2_subdev_core_ops imx335_core_ops = {
	.ioctl = imx335_ioctl,
};

static const struct v4l2_subdev_video_ops imx335_video_ops = {
	.s_stream = imx335_s_stream,
	.g_frame_interval = imx335_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx335_pad_ops = {
	.init_cfg = imx335_init_state,
	.enum_mbus_code = imx335_enum_mbus_code_rk,
	.enum_frame_size = imx335_enum_frame_size_rk,
	.get_selection = imx335_get_selection_rk,
	.set_selection = imx335_get_selection_rk,
	.get_fmt = imx335_get_fmt_rk,
	.set_fmt = imx335_set_fmt_rk,
	.get_mbus_config = imx335_get_mbus_config,
};

static const struct v4l2_subdev_ops imx335_subdev_ops = {
	.core = &imx335_core_ops,
	.video = &imx335_video_ops,
	.pad = &imx335_pad_ops,
};

/**
 * imx335_power_on() - Sensor power on sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx335 *imx335 = to_imx335(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx335_supply_name),
				    imx335->supplies);
	if (ret) {
		dev_err(dev, "%s: failed to enable regulators\n",
			__func__);
		return ret;
	}

	usleep_range(500, 550); /* Tlow */

	gpiod_set_value_cansleep(imx335->reset_gpio, 0);

	ret = clk_prepare_enable(imx335->inclk);
	if (ret) {
		dev_err(imx335->dev, "fail to enable inclk\n");
		goto error_reset;
	}

	usleep_range(20, 22); /* T4 */

	return 0;

error_reset:
	gpiod_set_value_cansleep(imx335->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(imx335_supply_name), imx335->supplies);

	return ret;
}

/**
 * imx335_power_off() - Sensor power off sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx335 *imx335 = to_imx335(sd);

	gpiod_set_value_cansleep(imx335->reset_gpio, 1);
	clk_disable_unprepare(imx335->inclk);
	regulator_bulk_disable(ARRAY_SIZE(imx335_supply_name), imx335->supplies);

	return 0;
}

/**
 * imx335_init_controls() - Initialize sensor subdevice controls
 * @imx335: pointer to imx335 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_init_controls(struct imx335 *imx335)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx335->ctrl_handler;
	const struct imx335_mode *mode = imx335->cur_mode;
	struct v4l2_fwnode_device_properties props;
	u32 lpfr, shutter_min;
	int ret;

	ret = v4l2_fwnode_device_parse(imx335->dev, &props);
	if (ret)
		return ret;

	/* v4l2_fwnode_device_properties can add two more controls */
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 10);
	if (ret)
		return ret;

	/* Initialize exposure and gain */
	lpfr = mode->vblank + mode->height;
	shutter_min = IMX335_SHUTTER_MIN;
	if (mode->scan_mode == IMX335_2_2_BINNING)
		shutter_min = IMX335_SHUTTER_MIN_BINNED;
	imx335->exp_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					     &imx335_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX335_EXPOSURE_MIN,
					     lpfr - shutter_min,
					     IMX335_EXPOSURE_STEP,
					     IMX335_EXPOSURE_DEFAULT);

	/*
	 * The sensor has an analog gain and a digital gain, both controlled
	 * through a single gain value, expressed in 0.3dB increments. Values
	 * from 0.0dB (0) to 30.0dB (100) apply analog gain only, higher values
	 * up to 72.0dB (240) add further digital gain. Limit the range to
	 * analog gain only, support for digital gain can be added separately
	 * if needed.
	 */
	imx335->again_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					       &imx335_ctrl_ops,
					       V4L2_CID_ANALOGUE_GAIN,
					       IMX335_AGAIN_MIN,
					       IMX335_AGAIN_MAX,
					       IMX335_AGAIN_STEP,
					       IMX335_AGAIN_DEFAULT);

	v4l2_ctrl_cluster(2, &imx335->exp_ctrl);

	imx335->vflip = v4l2_ctrl_new_std(ctrl_hdlr,
					  &imx335_ctrl_ops,
					  V4L2_CID_VFLIP,
					  0, 1, 1, 0);
	if (imx335->vflip)
		imx335->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx335->vblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx335_ctrl_ops,
						V4L2_CID_VBLANK,
						mode->vblank_min,
						mode->vblank_max,
						1, mode->vblank);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr,
				     &imx335_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx335_tpg_menu) - 1,
				     0, 0, imx335_tpg_menu);

	/* Read only controls */
	imx335->pclk_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					      &imx335_ctrl_ops,
					      V4L2_CID_PIXEL_RATE,
					      mode->pclk, mode->pclk,
					      1, mode->pclk);

	imx335->link_freq_ctrl = v4l2_ctrl_new_int_menu(ctrl_hdlr,
							&imx335_ctrl_ops,
							V4L2_CID_LINK_FREQ,
							__fls(imx335->link_freq_bitmap),
							__ffs(imx335->link_freq_bitmap),
							link_freq);
	if (imx335->link_freq_ctrl)
		imx335->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx335->hblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx335_ctrl_ops,
						V4L2_CID_HBLANK,
						mode->hblank,
						mode->hblank,
						1, mode->hblank);
	if (imx335->hblank_ctrl)
		imx335->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx335_ctrl_ops, &props);

	if (ctrl_hdlr->error) {
		dev_err(imx335->dev, "control init failed: %d\n",
			ctrl_hdlr->error);
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ctrl_hdlr->error;
	}

	imx335->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

/**
 * imx335_probe() - I2C client device binding
 * @client: pointer to i2c client device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx335_probe(struct i2c_client *client)
{
	struct imx335 *imx335;
	int ret;

	imx335 = devm_kzalloc(&client->dev, sizeof(*imx335), GFP_KERNEL);
	if (!imx335)
		return -ENOMEM;

	imx335->dev = &client->dev;
	imx335->cci = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx335->cci)) {
		dev_err(imx335->dev, "Unable to initialize I2C\n");
		return -ENODEV;
	}

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&imx335->sd, client, &imx335_subdev_ops);

	ret = imx335_parse_hw_config(imx335);
	if (ret) {
		dev_err(imx335->dev, "HW configuration is not supported\n");
		return ret;
	}

	ret = imx335_power_on(imx335->dev);
	if (ret) {
		dev_err(imx335->dev, "failed to power-on the sensor\n");
		return ret;
	}

	/* Check module identity */
	ret = imx335_detect(imx335);
	if (ret) {
		dev_err(imx335->dev, "failed to find sensor: %d\n", ret);
		goto error_power_off;
	}

	/* Set default mode to max resolution */
	imx335->cur_mode = &supported_modes[0];
	imx335->cur_mbus_code = imx335_mbus_codes[0];
	imx335->vblank = imx335->cur_mode->vblank;

	ret = imx335_init_controls(imx335);
	if (ret) {
		dev_err(imx335->dev, "failed to init controls: %d\n", ret);
		goto error_power_off;
	}

	/* Initialize subdev */
	imx335->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx335->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx335->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx335->sd.entity, 1, &imx335->pad);
	if (ret) {
		dev_err(imx335->dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	/* Optional Rockchip module description and naming */
	if (device_property_read_u32(imx335->dev, RKMODULE_CAMERA_MODULE_INDEX,
				     &imx335->module_index))
		imx335->module_index = 0;
	if (device_property_read_string(imx335->dev, RKMODULE_CAMERA_MODULE_FACING,
					&imx335->module_facing))
		imx335->module_facing = "back";
	if (device_property_read_string(imx335->dev, RKMODULE_CAMERA_MODULE_NAME,
					&imx335->module_name))
		imx335->module_name = "default";
	if (device_property_read_string(imx335->dev, RKMODULE_CAMERA_LENS_NAME,
					&imx335->len_name))
		imx335->len_name = "default";
	snprintf(imx335->sd.name, sizeof(imx335->sd.name), "m%02u_%s_imx335 %s",
		 imx335->module_index,
		 strcmp(imx335->module_facing, "back") ? "f" : "b",
		 dev_name(imx335->dev));

	imx335->sd.state_lock = imx335->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&imx335->sd);
	if (ret < 0) {
		dev_err(imx335->dev, "subdev init error\n");
		goto error_media_entity;
	}

	ret = v4l2_async_register_subdev_sensor(&imx335->sd);
	if (ret < 0) {
		dev_err(imx335->dev,
			"failed to register async subdev: %d\n", ret);
		goto error_subdev_cleanup;
	}

	pm_runtime_set_active(imx335->dev);
	pm_runtime_enable(imx335->dev);
	pm_runtime_idle(imx335->dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&imx335->sd);
error_media_entity:
	media_entity_cleanup(&imx335->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(imx335->sd.ctrl_handler);
error_power_off:
	imx335_power_off(imx335->dev);

	return ret;
}

/**
 * imx335_remove() - I2C client device unbinding
 * @client: pointer to I2C client device
 *
 * Return: 0 if successful, error code otherwise.
 */
static void imx335_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx335_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct dev_pm_ops imx335_pm_ops = {
	SET_RUNTIME_PM_OPS(imx335_power_off, imx335_power_on, NULL)
};

static const struct of_device_id imx335_of_match[] = {
	{ .compatible = "sony,imx335" },
	{ }
};

MODULE_DEVICE_TABLE(of, imx335_of_match);

static struct i2c_driver imx335_driver = {
	.probe_new = imx335_probe,
	.remove = imx335_remove,
	.driver = {
		.name = "rpi_imx335",
		.pm = &imx335_pm_ops,
		.of_match_table = imx335_of_match,
	},
};

module_i2c_driver(imx335_driver);

MODULE_DESCRIPTION("Sony imx335 sensor driver");
MODULE_LICENSE("GPL");
