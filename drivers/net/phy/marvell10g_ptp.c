// SPDX-License-Identifier: GPL-2.0+
/*
 * Marvell 10G 88x3310 PHY driver PTP support
 *
 * There are four 32-bit TOD registers (fractional nanoseconds, nanoseconds,
 * seconds low and seconds high). Each 32-bit register write requires two MDIO
 * operations and each read requires four MDIO operations. MDIO access is slow,
 * therefore this implementation protects against concurrent access to the TOD
 * registers by using mutex instead of spinlock to avoid potential RCU stalls
 * when the spinlock would not be available for a long time.
 */
#include <linux/phy.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/mutex.h>

#define MV_EXTTS_PERIOD_MS 95

enum {
	/* PMA/PMD MMD Registers */
	MV_PMA_XG_EXT_STATUS		= 0xc001,
	MV_PMA_XG_EXT_STATUS_PTP_UNSUPP = BIT(12),

	/* Vendor2 MMD registers */
	MV_V2_SLC_CFG_GEN		= 0x8000,
	MV_V2_SLC_CFG_GEN_DEF_VAL	= 0x7e50000f,
	MV_V2_SLC_CFG_GEN_WMC_ANEG_EN	= BIT(23),
	MV_V2_SLC_CFG_GEN_SMC_ANEG_EN	= BIT(24),
	MV_V2_MODE_CFG 			= 0xf000,
	MV_V2_MODE_CFG_M_UNIT_PWRUP 	= BIT(12),

	/* Vendor2 MMD PTP registers */
	MV_V2_INDIRECT_READ_ADDR 	= 0x97fd,
	MV_V2_INDIRECT_READ_DATA_LOW 	= 0x97fe,
	MV_V2_INDIRECT_READ_DATA_HIGH 	= 0x97ff,

	MV_V2_PTP_PARSER_EG_UDATA	= 0xa200,
	MV_V2_PTP_UPDATER_EG_UDATA	= 0xa400,
	MV_V2_PTP_PARSER_IG_UDATA	= 0xaa00,
	MV_V2_PTP_UPDATER_IG_UDATA	= 0xac00,

	MV_V2_PTP_TOD_LOAD_NSEC_FRAC 	= 0xbc2a,
	MV_V2_PTP_TOD_LOAD_NSEC 	= 0xbc2c,
	MV_V2_PTP_TOD_LOAD_SEC_LOW 	= 0xbc2e,
	MV_V2_PTP_TOD_LOAD_SEC_HIGH 	= 0xbc30,
	MV_V2_PTP_TOD_CAP0_NSEC_FRAC 	= 0xbc32,
	MV_V2_PTP_TOD_CAP0_NSEC 	= 0xbc34,
	MV_V2_PTP_TOD_CAP0_SEC_LOW 	= 0xbc36,
	MV_V2_PTP_TOD_CAP0_SEC_HIGH 	= 0xbc38,

	MV_V2_PTP_TOD_CAP_CFG 		= 0xbc42,
	MV_V2_PTP_TOD_CAP_CFG_VAL0 	= BIT(0),
	MV_V2_PTP_TOD_CAP_CFG_VAL1 	= BIT(1),
	MV_V2_PTP_TOD_FUNC_CFG 		= 0xbc46,
	MV_V2_PTP_TOD_FUNC_CFG_TRIG 	= BIT(28),
	MV_V2_PTP_TOD_FUNC_CFG_UPDATE 	= 0,
	MV_V2_PTP_TOD_FUNC_CFG_INCR 	= BIT(30),
	MV_V2_PTP_TOD_FUNC_CFG_DECR 	= BIT(31),
	MV_V2_PTP_TOD_FUNC_CFG_CAPTURE 	= BIT(31) | BIT(30),
};

static const u32 mv3310_parser_ucode[] = {
	0x200ea,   0x0d008, 0x0c140, 0x28427, 0x2cc27, 0x29c4a, 0x2a44a,
	0x2c417,   0x2e4aa, 0x2ecb7, 0x2ac25, 0x28c37, 0x2b469, 0x2bc83,
	0x294c6,   0x2f414, 0x0d840, 0x35452, 0x23836, 0x30000, 0x0e050,
	0x07008,   0x30000, 0x0d0c0, 0x7cb23, 0x7dddb, 0x7bddb, 0x33167,
	0x0d8e0,   0x1d802, 0x06040, 0x76b03, 0x1d804, 0x06040, 0x20003,
	0x3700e,   0x30000, 0x1d808, 0x20028, 0x1d801, 0x28427, 0x2cc27,
	0x29c4a,   0x2a44a, 0x2c417, 0x2b469, 0x2bc83, 0x294c6, 0x2ac25,
	0x28c37,   0x2f414, 0x0d840, 0x35452, 0x30400, 0x1d801, 0x0d8a0,
	0x35496,   0x30900, 0x0d8a0, 0x1e098, 0x35497, 0x30900, 0x3702c,
	0x0d8a0,   0x28427, 0x2cc27, 0x2ac25, 0x29c4a, 0x2a44a, 0x2c417,
	0x2b469,   0x2bc83, 0x294c6, 0x30000, 0x0d860, 0x0d8a0, 0x7c259,
	0x0d8e0,   0x0d8e0, 0x7c359, 0x0d8e0, 0x0d8e0, 0x7c359, 0x0d8e0,
	0x0d8e0,   0x7c359, 0x0d8e0, 0x0d8e0, 0x743db, 0x1e453, 0x2185e,
	0x1e095,   0x1e494, 0x21067, 0x0d840, 0x1de4c, 0x35844, 0x24069,
	0x35846,   0x24083, 0x35840, 0x30900, 0x1d802, 0x1d806, 0x20028,
	0x37022,   0x0d840, 0x0c081, 0x1d804, 0x1de8c, 0x35884, 0x248ea,
	0x1e050,   0x1de47, 0x3584a, 0x230ea, 0x1da44, 0x05900, 0x1c801,
	0x35911,   0x240bb, 0x3592f, 0x2408d, 0x35904, 0x24069, 0x35929,
	0x24083,   0x35932, 0x2409f, 0x0d900, 0x30000, 0x0d880, 0x1de8c,
	0x35886,   0x248ea, 0x1d803, 0x37023, 0x0d900, 0x1d811, 0x1df08,
	0x20077,   0x37025, 0x0d860, 0x1de4a, 0x30300, 0x2d467, 0x28427,
	0x2cc27,   0x2ac25, 0x29c4a, 0x2a44a, 0x2b469, 0x2bc83, 0x294c6,
	0x28c37,   0x0d840, 0x35452, 0x23836, 0x30000, 0x74ddb, 0x1d802,
	0x0f00c,   0x0f024, 0x0d860, 0x35841, 0x240a8, 0x37020, 0x30000,
	0x37026,   0x30000, 0x7d5ea, 0x0d860, 0x1e05b, 0x0e011, 0x0d9e0,
	0x0d9a0,   0x3316e, 0x77928, 0x3722f, 0x1d802, 0x0f02c, 0x1d801,
	0x20028,   0x7d5ea, 0x1d802, 0x0f02c, 0x20028, 0x37021, 0x0d8e0,
	0x2dcc1,   0x2fcc1, 0x2f4ed, 0x30000, 0x1d801, 0x0d8e0, 0x358c0,
	0x240c6,   0x3722d, 0x06050, 0x0d840, 0x709dc, 0x37020, 0x0f00c,
	0x1d801,   0x0f024, 0x0f01c, 0x0f07c, 0x0f074, 0x0f06c, 0x0f064,
	0x0f034,   0x0f03c, 0x1d805, 0x0f04c, 0x1d805, 0x775da, 0x0e037,
	0x0e03e,   0x0d900, 0x30000, 0x1d801, 0x37000, 0x37028, 0x0f07c,
	0x1d808,   0x0f00c, 0x0f074, 0x0f06c, 0x0f064, 0x0f024, 0x0f04c,
	0x1d801,   0x0f018, 0x30000, 0x0c800, 0x3702f, 0x30000, 0x1d801,
	0x0d8e0,   0x358c0, 0x240f2, 0x3722d, 0x06050, 0x0f04c, 0x1d805,
	0x0f00c,   0x0f024, 0x1d80f, 0x37006, 0x37027, 0x0f060, 0x30000,
	0xffffffff
};

static const u32 mv3310_updater_ucode[] = {

	0x2008d, 0x0d055, 0x46801, 0x0d1c0, 0x0d088,   0x7d1bf, 0x7f10e,
	0x76913, 0x42000, 0x8c450, 0x8c513, 0x8c489,   0x8c044, 0x20013,
	0x42000, 0x8b450, 0x8b513, 0x8b489, 0x8b044,   0x75234, 0x7ca1b,
	0x0d110, 0x0d0d1, 0x0d192, 0x7061b, 0xd4230,   0xdc611, 0x7493e,
	0x07e08, 0x0d148, 0x1dd41, 0x71924, 0x40546,   0x8d754, 0x8d4d2,
	0x2003e, 0x77926, 0x1d988, 0x40546, 0x0c141,   0x1dd4d, 0x1df4d,
	0x7f92e, 0x474e5, 0x8d450, 0x2003e, 0x3316f,   0x474e5, 0x434c4,
	0x8d513, 0x8d496, 0x2003e, 0x7493e, 0x7273e,   0x0d108, 0x7993b,
	0x77c3b, 0x0fb10, 0x2003c, 0x0fb08, 0x1dd01,   0x40d03, 0x0514a,
	0x1db41, 0x1dd41, 0x76a44, 0x48942, 0x49082,   0x7079c, 0x1d94a,
	0x0d18f, 0x7524e, 0x72961, 0x535ba, 0x5b9db,   0xdbc0f, 0x330a8,
	0x20061, 0x75961, 0x7078d, 0x0d105, 0x70454,   0xd1e27, 0xd9a06,
	0x0d112, 0x70458, 0xd4230, 0xdc611, 0x0d111,   0x35506, 0x22061,
	0x2305f, 0x0d110, 0x35507, 0x22061, 0xd4270,   0xdc651, 0x7316c,
	0x330a8, 0x0d118, 0x7fc69, 0x535b7, 0x5b9d8,   0xdbc0f, 0x2006c,
	0x535b7, 0x5b9d8, 0xdbfef, 0x7d27d, 0x7597d,   0x330a8, 0x640f0,
	0x6c4d1, 0xec812, 0x5360d, 0x5ba2e, 0xdbc0f,   0x2587d, 0x7fe7d,
	0x0d119, 0x0e064, 0x0e06c, 0x0e074, 0x3310f,   0x0e07c, 0x74281,
	0x48948, 0x8d3ce, 0x8d34c, 0x1d948, 0x7d28f,   0x76185, 0x49944,
	0x7418d, 0x7bf98, 0x7b7a2, 0x1d952, 0x4894a,   0x8d513, 0x8d491,
	0x45082, 0x40000, 0x30000, 0x76196, 0x48944,   0x71995, 0x434c4,
	0x40000, 0x30000, 0x8d450, 0x40000, 0x30000,   0x1d96a, 0x200b5,
	0x1d962, 0x200b5, 0x75285, 0x7618d, 0x7bf9a,   0x7378d, 0x0fe08,
	0x200a4, 0x7418d, 0x0fe00, 0x0d14a, 0x1dd41,   0x1c96e, 0x0d1c1,
	0x0fe2e, 0x351c6, 0x240b2, 0x0fe2d, 0x351c6,   0x240b4, 0x0fe2f,
	0x351c6, 0x240b4, 0x2008d, 0x1d954, 0x200b5,   0x1d944, 0x48948,
	0x8d4d2, 0x752bc, 0x7cabc, 0x434c4, 0x40000,   0x30000, 0x8d450,
	0x40000, 0x30000, 0x41400, 0x30000, 0xffffffff
};

struct mv3310_ptp_priv {
	struct phy_device *phydev;
	struct ptp_clock_info caps;
	struct ptp_clock *clock;
	struct mutex lock; /* Protects against concurrent MDIO register access */
	bool extts_enabled;
};

struct mv3310_ptp_priv *mv3310_ptp_probe(struct phy_device *phydev);
int mv3310_ptp_power_up(struct phy_device *phydev);
int mv3310_ptp_power_down(struct phy_device *phydev);
int mv3310_ptp_check_ucode(struct phy_device *phydev);

static int mv3310_read_ptp_reg(struct phy_device *phydev, u32 regnum,
			       u32 *regval);
static int mv3310_write_ptp_reg(struct phy_device *phydev, u32 regnum,
				u32 regval);
static int mv3310_ptp_set_udata(struct phy_device *phydev, const u32 *filedata,
				u32 baseaddr);

static int mv3310_adjfine(struct ptp_clock_info *ptp, long scaled_ppm);
static int mv3310_adjphase(struct ptp_clock_info *ptp, s32 phase);
static int mv3310_adjtime(struct ptp_clock_info *ptp, s64 delta);
static int mv3310_gettimex64(struct ptp_clock_info *ptp, struct timespec64 *ts,
			     struct ptp_system_timestamp *sts);
static int mv3310_settime64(struct ptp_clock_info *ptp,
			    const struct timespec64 *ts);
static int mv3310_enable(struct ptp_clock_info *ptp,
			 struct ptp_clock_request *request, int on);
static int mv3310_verify(struct ptp_clock_info *ptp, unsigned int pin,
			 enum ptp_pin_function func, unsigned int chan);
static long mv3310_do_aux_work(struct ptp_clock_info *ptp);

static bool mv3310_is_ptp_supported(struct phy_device *phydev)
{
	int ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, MV_PMA_XG_EXT_STATUS);
	if (ret < 0)
		return false;

	return !(ret & MV_PMA_XG_EXT_STATUS_PTP_UNSUPP);
}

struct mv3310_ptp_priv *mv3310_ptp_probe(struct phy_device *phydev)
{
	struct mv3310_ptp_priv *priv;

	if (!mv3310_is_ptp_supported(phydev)) {
		dev_info(&phydev->mdio.dev, "PTP is not present in this device\n");
		return NULL;
	}

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return NULL;

	priv->phydev = phydev;
	mutex_init(&priv->lock);
	priv->extts_enabled = false;

	priv->caps.owner = THIS_MODULE;
	strscpy(priv->caps.name, "mv10g-phy-phc", sizeof(priv->caps.name));
	priv->caps.max_adj = 0;
	priv->caps.n_alarm = 0;
	priv->caps.n_ext_ts = 1;
	priv->caps.n_per_out = 0;
	priv->caps.n_pins = 0;
	priv->caps.pps = 0;
	priv->caps.pin_config = NULL;
	priv->caps.adjfine = mv3310_adjfine;
	priv->caps.adjphase = mv3310_adjphase;
	priv->caps.adjtime = mv3310_adjtime;
	priv->caps.gettimex64 = mv3310_gettimex64;
	priv->caps.settime64 = mv3310_settime64;
	priv->caps.enable = mv3310_enable;
	priv->caps.verify = mv3310_verify;
	priv->caps.do_aux_work = mv3310_do_aux_work;
	/* This is set to NULL instead of EOPNOTSUPP, simply defining it will
	   present "has cross timestamping support" in capabilities. */
	priv->caps.getcrosststamp = NULL;

	priv->clock = ptp_clock_register(&priv->caps, &phydev->mdio.dev);
	if (IS_ERR(priv->clock)) {
		dev_err(&phydev->mdio.dev, "failed to register PTP clock\n");
		devm_kfree(&phydev->mdio.dev, priv);
		return NULL;
	}

	return priv;
}

int mv3310_ptp_power_up(struct phy_device *phydev)
{
	int ret;

	if (!mv3310_is_ptp_supported(phydev))
		return 0;

	/* Enable M unit used for PTP */
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, MV_V2_MODE_CFG,
			       MV_V2_MODE_CFG_M_UNIT_PWRUP);
	if (ret < 0)
		return ret;

	/* PHY Errata section 4.4: after the M unit is powered up
	   auto-negotiation is disabled by default. Enable:
	   * WMC - auto negotiation for wire mac
	   * SMC - auto negotiation for system mac */
	ret = mv3310_write_ptp_reg(phydev, MV_V2_SLC_CFG_GEN,
				   MV_V2_SLC_CFG_GEN_DEF_VAL |
					   MV_V2_SLC_CFG_GEN_WMC_ANEG_EN |
					   MV_V2_SLC_CFG_GEN_SMC_ANEG_EN);
	if (ret < 0)
		return ret;

	return 0;
}

int mv3310_ptp_power_down(struct phy_device *phydev)
{
	if (!mv3310_is_ptp_supported(phydev))
		return 0;

	return phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2, MV_V2_MODE_CFG,
				  MV_V2_MODE_CFG_M_UNIT_PWRUP);
}

int mv3310_ptp_check_ucode(struct phy_device *phydev)
{
	int ret = 0;
	u32 regval = 0;

	if (!mv3310_is_ptp_supported(phydev))
		return 0;

	/* Check if the microcode is already loaded */
	mv3310_read_ptp_reg(phydev, MV_V2_PTP_PARSER_EG_UDATA, &regval);
	if (regval == mv3310_updater_ucode[0])
		return 0;

	dev_info(&phydev->mdio.dev, "loading PTP parser & updater microcode\n");
	ret |= mv3310_ptp_set_udata(phydev, mv3310_parser_ucode,
				    MV_V2_PTP_PARSER_EG_UDATA);
	ret |= mv3310_ptp_set_udata(phydev, mv3310_updater_ucode,
				    MV_V2_PTP_UPDATER_EG_UDATA);
	ret |= mv3310_ptp_set_udata(phydev, mv3310_parser_ucode,
				    MV_V2_PTP_PARSER_IG_UDATA);
	ret |= mv3310_ptp_set_udata(phydev, mv3310_updater_ucode,
				    MV_V2_PTP_UPDATER_IG_UDATA);

	return ret;
}

static int mv3310_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	return -EOPNOTSUPP;
}

static int mv3310_adjphase(struct ptp_clock_info *ptp, s32 phase)
{
	return -EOPNOTSUPP;
}

static int mv3310_verify(struct ptp_clock_info *ptp, unsigned int pin,
			 enum ptp_pin_function func, unsigned int chan)
{
	return -EOPNOTSUPP;
}

static int mv3310_read_ptp_reg(struct phy_device *phydev, u32 regnum,
			       u32 *regval)
{
	int ret;

	/* Read register address */
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2, regnum);
	if (ret < 0)
		return ret;

	/* Read that Indirect_read_address gives requested address */
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2, MV_V2_INDIRECT_READ_ADDR);
	if (ret < 0)
		return ret;
	if (ret != regnum) {
		pr_err("Indirect read address mismatch: %04x != %04x\n", ret,
		       regnum);
		return -EINVAL;
	}

	/* Read Indirect_read_data_low provides lower 16-bits (15:0) of data */
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2,
			   MV_V2_INDIRECT_READ_DATA_LOW);
	if (ret < 0)
		return ret;
	*regval = ret & 0xffff;

	/* Read Indirect_read_data_high provides upper 16-bits (31:16) of data */
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2,
			   MV_V2_INDIRECT_READ_DATA_HIGH);
	if (ret < 0)
		return ret;
	*regval += ((ret & 0xffff) << 16);

	return 0;
}

static int mv3310_write_ptp_reg(struct phy_device *phydev, u32 regnum,
				u32 regval)
{
	int ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, regnum, regval);
	if (ret < 0)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, regnum + 1, regval >> 16U);
	if (ret < 0)
		return ret;

	return 0;
}

static int mv3310_ptp_set_udata(struct phy_device *phydev, const u32 *filedata,
				u32 baseaddr)
{
	int ret = 0;
	int word_index = 0;
	u32 word;

	while ((word = *filedata) != -1) {
		filedata++;
		ret = mv3310_write_ptp_reg(phydev, baseaddr + word_index * 2,
					   word);
		if (ret < 0) {
			dev_err(&phydev->mdio.dev,
				"Failed to write PTP microcode address: %x\n",
				baseaddr + word_index * 2);
			break;
		}

		word_index++;
	}
	return ret;
}

static int mv3310_trigger_ptp_op(struct phy_device *phydev, int op)
{
	int ret;

	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_FUNC_CFG,
				   MV_V2_PTP_TOD_FUNC_CFG_TRIG | op);
	if (ret < 0)
		return ret;

	if (op != MV_V2_PTP_TOD_FUNC_CFG_CAPTURE) {
		/* Restore capture mode */
		return mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_FUNC_CFG,
					    MV_V2_PTP_TOD_FUNC_CFG_CAPTURE);
	}

	return 0;
}

static int mv3310_read_tod(struct phy_device *phydev, struct timespec64 *ts,
			   struct ptp_system_timestamp *sts)
{
	int ret = 0;
	u32 nsec_frac = 0, nsec = 0, sec_low = 0, sec_high = 0;

	ptp_read_system_prets(sts);
	ret |= mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP0_NSEC_FRAC,
				   &nsec_frac);
	ptp_read_system_postts(sts);
	ret |= mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP0_NSEC, &nsec);
	ret |= mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP0_SEC_LOW,
				   &sec_low);
	ret |= mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP0_SEC_HIGH,
				   &sec_high);

	if (ret < 0)
		return -EIO;

	/* check if nsec should be rounded up */
	if (nsec_frac > (U32_MAX / 2))
		nsec++;
	ts->tv_sec = ((u64)sec_high << 32U) | sec_low;
	ts->tv_nsec = nsec;

	return 0;
}

static int mv3310_write_tod(struct phy_device *phydev,
			    const struct timespec64 *ts)
{
	int ret = 0;
	u32 nsec = lower_32_bits(ts->tv_nsec);
	u32 sec_low = lower_32_bits(ts->tv_sec);
	u32 sec_high = upper_32_bits(ts->tv_sec) & 0xffff;

	ret |= mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_LOAD_NSEC_FRAC, 0);
	ret |= mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_LOAD_NSEC, nsec);
	ret |= mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_LOAD_SEC_LOW,
				    sec_low);
	ret |= mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_LOAD_SEC_HIGH,
				    sec_high);

	if (ret < 0)
		return -EIO;

	return 0;
}

static int mv3310_getppstime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	int ret;
	u32 cap_cfg = 0;

	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);
	struct phy_device *phydev = priv->phydev;

	mutex_lock(&priv->lock);
	/* Check if TOD@pps is available */
	ret = mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP_CFG, &cap_cfg);
	if (ret < 0)
		goto unlock_out;
	if (!(cap_cfg & MV_V2_PTP_TOD_CAP_CFG_VAL0)) {
		ret = -EAGAIN;
		goto unlock_out;
	}

	ret = mv3310_read_tod(phydev, ts, NULL);
	if (ret < 0)
		goto unlock_out;

	/* Finished reading capture, reset */
	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_CAP_CFG, 0);

unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_gettimex64(struct ptp_clock_info *ptp, struct timespec64 *ts,
			     struct ptp_system_timestamp *sts)
{
	int ret;
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);
	struct phy_device *phydev = priv->phydev;

	mutex_lock(&priv->lock);
	/* Clear existing TOD Capture Values and trigger new capture.
	   In the unlikely event that a pulse-in trigger will capture the TOD
	   to TOD_CAP0 and this CPU trigger will capture it to TOD_CAP1, we are
	   still reading from TOD_CAP0 as they will be almost equal. */
	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_CAP_CFG, 0);
	if (ret < 0)
		goto unlock_out;

	ret = mv3310_trigger_ptp_op(phydev, MV_V2_PTP_TOD_FUNC_CFG_CAPTURE);
	if (ret < 0)
		goto unlock_out;

	/* Read capture */
	ret = mv3310_read_tod(phydev, ts, sts);
	if (ret < 0)
		goto unlock_out;

	/* Finished reading capture, reset */
	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_CAP_CFG, 0);

unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_settime64(struct ptp_clock_info *ptp,
			    const struct timespec64 *ts)
{
	int ret;
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);
	struct phy_device *phydev = priv->phydev;

	mutex_lock(&priv->lock);
	/* Load the new timestamp */
	ret = mv3310_write_tod(phydev, ts);
	if (ret < 0)
		goto unlock_out;

	/* Trigger update */
	ret = mv3310_trigger_ptp_op(phydev, MV_V2_PTP_TOD_FUNC_CFG_UPDATE);

unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	int ret;
	const struct timespec64 ts = ns_to_timespec64(abs(delta));
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);
	struct phy_device *phydev = priv->phydev;

	if (delta == 0)
		return 0;

	mutex_lock(&priv->lock);
	/* Load the new timestamp */
	ret = mv3310_write_tod(phydev, &ts);
	if (ret < 0)
		goto unlock_out;

	/* Trigger update */
	ret = mv3310_trigger_ptp_op(phydev,
				    delta < 0 ? MV_V2_PTP_TOD_FUNC_CFG_DECR :
						MV_V2_PTP_TOD_FUNC_CFG_INCR);
unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_enable(struct ptp_clock_info *ptp,
			 struct ptp_clock_request *request, int on)
{
	int ret = 0;
	bool enable = on != 0;
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);

	switch (request->type) {
	case PTP_CLK_REQ_EXTTS:
		if (enable)
			if (!priv->extts_enabled)
				ptp_schedule_worker(priv->clock, 0);
			else
				ret = -EBUSY;
		else
			if (priv->extts_enabled)
				ptp_cancel_worker_sync(priv->clock);

		priv->extts_enabled = enable;
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static long mv3310_do_aux_work(struct ptp_clock_info *ptp)
{
	struct ptp_clock_event event;
	struct timespec64 ts;
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);

	if (mv3310_getppstime(ptp, &ts) == 0) {
		event.type = PTP_CLOCK_EXTTS;
		event.index = 0; /* We only have one channel */
		event.timestamp = timespec64_to_ns(&ts);
		ptp_clock_event(priv->clock, &event);
	}

	return msecs_to_jiffies(MV_EXTTS_PERIOD_MS);
}
