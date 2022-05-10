// SPDX-License-Identifier: GPL-2.0+

#include <linux/module.h>
#include <linux/phy.h>

#define TI_PHY_ID_TLK10031	0x40005100

#define PMA_TI_TRAIN_CONTROL		0x96
#define LT_TRAINING_ENABLE		BIT(1)

#define TI_V1_HS_SERDES_CONTROL_3	0x4

#define TI_V1_RESET_CONTROL		0xe
#define RESET_CONTROL_DATAPATH_RESET	BIT(3)

static int tlk10031_get_features(struct phy_device *phydev)
{
	linkmode_set_bit(ETHTOOL_LINK_MODE_10000baseKR_Full_BIT,
			phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseKX_Full_BIT,
			phydev->supported);

	return 0;
}

static int tlk10031_config_init(struct phy_device *phydev)
{
	int ret;

	ret = genphy_c45_an_disable_aneg(phydev);
	if (ret >= 0)
		ret = phy_clear_bits_mmd(phydev, MDIO_MMD_PMAPMD,
				PMA_TI_TRAIN_CONTROL, LT_TRAINING_ENABLE);
	if (ret >= 0)
		ret = phy_write_mmd(phydev, MDIO_MMD_PMAPMD, 0x9002, 0);
	if (ret >= 0)
		ret = phy_write_mmd(phydev, MDIO_MMD_PMAPMD, 0x9003, 0);
	if (ret >= 0)
		ret = phy_write_mmd(phydev, MDIO_MMD_VEND1,
				TI_V1_HS_SERDES_CONTROL_3, 0xe640);
	if (ret >= 0)
		ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND1,
				TI_V1_RESET_CONTROL,
				RESET_CONTROL_DATAPATH_RESET);

	return ret;
}

static int tlk10031_read_status(struct phy_device *phydev)
{
	int val;

	phydev->speed = SPEED_10000;
	phydev->duplex = DUPLEX_FULL;
	phydev->port = PORT_MII;

	val = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_STAT1);
	if (val < 0)
		return val;

	if ((val & MDIO_STAT1_LSTATUS) == 0 && (val & MDIO_STAT1_FAULT)) {
		/* Try reading STAT2 to clear previous fault */
		phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_STAT2);
		val = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_STAT1);
		if (val < 0)
			return val;
	}

	phydev->link = !!(val & MDIO_STAT1_LSTATUS);

	return 0;
}

static struct phy_driver tlk10031_driver[] = {
	{
		PHY_ID_MATCH_MODEL(TI_PHY_ID_TLK10031),
		.name		= "tlk10031",
		.get_features	= tlk10031_get_features,
		.config_init	= tlk10031_config_init,
		.read_status	= tlk10031_read_status,
	},
};

module_phy_driver(tlk10031_driver);

static struct mdio_device_id __maybe_unused tlk10031_tbl[] = {
	{ PHY_ID_MATCH_MODEL(TI_PHY_ID_TLK10031) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, tlk10031_tbl);
MODULE_DESCRIPTION("TI TLK10031 10G Transceiver driver");
MODULE_LICENSE("GPL");
