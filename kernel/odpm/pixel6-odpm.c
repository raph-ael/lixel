// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal reader for the S2MPG10/S2MPG11 rail power meters ("ODPM") of the
 * Pixel 6 on a mainline kernel. Binds to the "s2mpg1x-meter" MFD cells and
 * uses the parent's "meter" regmap (ACPM access type 0x0a).
 *
 * Register usage and scaling follow Google's vendor kernel
 * (drivers/iio/power/odpm.c, s2mpg1{0,1}-powermeter.c, s2mpg1x-meter.h).
 *
 *   cat /sys/kernel/debug/odpm/power           average mW since previous read
 *   cat /sys/kernel/debug/odpm/assign          list of selectable rails
 *   echo "3 LDO7M" > /sys/kernel/debug/odpm/assign   (channel 0-7 = S2MPG10,
 *                                                     8-15 = S2MPG11)
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#define NCH			8

/* Meter block registers common to both PMICs */
#define REG_CTRL1		0x00
#define  CTRL1_METER_EN		BIT(0)
#define  CTRL1_EXT_METER_EN	BIT(1)
#define  CTRL1_INT_RATE		GENMASK(4, 2)
#define  CTRL1_INT_RATE_125HZ	(4 << 2)
#define REG_CTRL2		0x01
#define  CTRL2_ASYNC_RD		BIT(7)
#define  CTRL2_EXT_CH_EN	GENMASK(5, 3)
#define  CTRL2_EXT_RATE		GENMASK(2, 0)
#define  CTRL2_EXT_RATE_30HZ	2
#define REG_ACC_DATA		0x40	/* 8 x 6 bytes LE, 41 bit */
#define REG_ACC_COUNT		0x70	/* 3 bytes LE, 20 bit */
#define REG_LPF_DATA		0x73	/* 8 x 3 bytes LE, 21 bit */
#define MUXSEL_MASK		0x7f
#define INT_RATE_HZ		125

/* S2MPG10 only: vendor "powermeter offset rising" workaround */
#define G10_DSM_TRIM_OFFSET	0xee	/* bits 5:0 */
#define G10_BUCK_METER_TRIM3	0xf1	/* bits 7:6 */

/* Power per LSB in pW (vendor s2mpg1x-register.h, mW IQ30 constants) */
#define CMS_BUCK	6105006
#define CMD_BUCK	12210012
#define CMT_BUCK	18315018
#define VM_BUCK		12210012
#define BB		10989011
#define DVS_N150	228938
#define N150		457875
#define P150		915751
#define N300		915751
#define P300		1831502
#define DVS_N450	686813
#define N450		1373626
#define DVS_N800	1221001
#define N800		2442002
#define P800		4884005
#define N1000		1418251
/* 1.3186813 mV * 0.7935698 * 8 * 10 / shunt_uohms [mW], as odpm.c does */
#define SHUNT(uohm)	((u32)(83717252400ULL / (uohm)))

struct odpm_rail {
	u8 mux;
	u32 res_pw;
	const char *name;
	const char *sch;
};

#define BUCK(n, sfx, res, sch)	{ n, res, "BUCK" #n sfx, sch }
#define LDO(n, sfx, res, sch)	{ 0x20 + n, res, "LDO" #n sfx, sch }

static const struct odpm_rail s2mpg10_rails[] = {
	BUCK(1, "M", CMS_BUCK, "S1M_VDD_MIF"),
	BUCK(2, "M", CMT_BUCK, "S2M_VDD_CPUCL2"),
	BUCK(3, "M", CMD_BUCK, "S3M_VDD_CPUCL1"),
	BUCK(4, "M", CMS_BUCK, "S4M_VDD_CPUCL0"),
	BUCK(5, "M", CMT_BUCK, "S5M_VDD_INT"),
	BUCK(6, "M", CMS_BUCK, "S6M_VDD_CPUCL2_M"),
	BUCK(7, "M", CMS_BUCK, "S7M_VDD_INT_M"),
	BUCK(8, "M", CMS_BUCK, "S8M_LLDO2"),
	BUCK(9, "M", CMS_BUCK, "S9M_LLDO3"),
	BUCK(10, "M", CMT_BUCK, "S10M_VDD_TPU"),
	LDO(1, "M", DVS_N150, "L1M_ALIVE"),
	LDO(2, "M", P800, "L2M_ALIVE(DDR)"),
	LDO(3, "M", N800, "L3M(IO)"),
	LDO(4, "M", P150, "L4M(IO)"),
	LDO(5, "M", N150, "L5M_TCXO"),
	LDO(6, "M", N150, "L6M_PLL"),
	LDO(7, "M", DVS_N450, "L7M_VDD_HSI"),
	LDO(8, "M", N300, "L8M_USB"),
	LDO(9, "M", P150, "L9M_USB"),
	LDO(10, "M", P150, "L10M_USB"),
	LDO(11, "M", DVS_N150, "L11M_VDD_CPUCL1_M"),
	LDO(12, "M", DVS_N800, "L12M_VDD_CPUCL0_M"),
	LDO(13, "M", DVS_N450, "L13M_VDD_TPU_M"),
	LDO(14, "M", P150, "L14M_AVDD_TCXO"),
	LDO(15, "M", DVS_N800, "L15M_VDD_SLC_M"),
	LDO(16, "M", N800, "L16M_PCIE0"),
	LDO(17, "M", N800, "L17M_PCIE1"),
	LDO(18, "M", P150, "L18M_PCIE0"),
	LDO(19, "M", P150, "L19M_PCIE1"),
	LDO(20, "M", P150, "L20M_DMICS"),
	LDO(21, "M", P300, "L21M_DTLS"),
	LDO(22, "M", P150, "L22M_DISP_VCI"),
	LDO(23, "M", P300, "L23M_DISP_VDDI"),
	LDO(24, "M", N150, "L24M_UNUSED"),
	LDO(25, "M", P150, "L25M_TSP_DVDD"),
	LDO(26, "M", P150, "L26M_TS_AVDD"),
	LDO(27, "M", P150, "L27M_UDFPS_AVDD"),
	LDO(28, "M", N300, "L28M_DISP_VDDD"),
	LDO(29, "M", P150, "L29M_UNUSED"),
	LDO(30, "M", P150, "L30M_UNUSED"),
	LDO(31, "M", P300, "L31M_NFC"),
	{ 0x5c, SHUNT(10000), "VSEN_C1", "PPVAR_VSYS_PWR_DISP" },
	{ 0x5d, SHUNT(5000), "VSEN_C2", "VSYS_PWR_MODEM" },
	{ 0x5e, SHUNT(5000), "VSEN_C3", "VSYS_PWR_RFFE" },
};

static const struct odpm_rail s2mpg11_rails[] = {
	BUCK(1, "S", CMD_BUCK, "S1S_VDD_CAM"),
	BUCK(2, "S", CMT_BUCK, "S2S_VDD_G3D"),
	BUCK(3, "S", CMS_BUCK, "S3S_LLDO1"),
	BUCK(4, "S", CMS_BUCK, "S4S_VDD2H_MEM"),
	BUCK(5, "S", CMS_BUCK, "S5S_VDDQ_MEM"),
	BUCK(6, "S", CMS_BUCK, "S6S_LLDO4"),
	BUCK(7, "S", VM_BUCK, "S7S_MLDO"),
	BUCK(8, "S", CMS_BUCK, "S8S_VDD_G3D_L2"),
	BUCK(9, "S", CMS_BUCK, "S9S_VDD_AOC"),
	BUCK(10, "S", CMS_BUCK, "S10S_VDD2L"),
	{ 0x0b, VM_BUCK, "BUCKD", "SD(UFS)" },
	{ 0x0c, VM_BUCK, "BUCKA", "SA(IO)" },
	{ 0x0d, BB, "BUCKBOOST", "BB_HLDO" },
	LDO(1, "S", DVS_N800, "L1S_VDD_G3D_M"),
	LDO(2, "S", DVS_N150, "L2S_VDD_AOC_RET"),
	LDO(3, "S", P150, "L3S_UNUSED"),
	LDO(4, "S", P150, "L4S_UWB"),
	LDO(5, "S", N450, "L5S_UNUSED"),
	LDO(6, "S", P300, "L6S_PROX"),
	LDO(7, "S", P150, "L7S_SENSORS"),
	LDO(8, "S", N1000, "L8S_UFS_VCCQ"),
	LDO(9, "S", N150, "L9S_GNSS_CORE"),
	LDO(10, "S", P300, "L10S_GNSS_RF"),
	LDO(11, "S", P150, "L11S_GNSS_AUX"),
	LDO(12, "S", P300, "L12S_UNUSED"),
	LDO(13, "S", P150, "L13S_DPAUX"),
	LDO(14, "S", P300, "L14S_UWB"),
	LDO(15, "S", P150, "L15S_UNUSED"),
	{ 0x5c, SHUNT(10000), "VSEN_C4", "VSYS_PWR_DISPLAY" },
	{ 0x5d, SHUNT(10000), "VSEN_C5", "VSEN_C5_NC" },
	{ 0x5e, SHUNT(10000), "VSEN_C6", "VSYS_PWR_MMWAVE" },
};

struct odpm_desc {
	const char *name;
	const struct odpm_rail *rails;
	unsigned int nrails;
	u8 reg_acc_mode, reg_lpf_mode, reg_bucken, reg_muxsel;
	const char *def[NCH];	/* vendor gs101 oriole (EVT1+) assignment */
};

static const struct odpm_desc odpm_descs[] = {
	{
		.name = "S2MPG10",
		.rails = s2mpg10_rails, .nrails = ARRAY_SIZE(s2mpg10_rails),
		.reg_acc_mode = 0x02, .reg_lpf_mode = 0x03,
		.reg_bucken = 0x04, .reg_muxsel = 0x06,
		.def = { "VSEN_C1", "VSEN_C2", "VSEN_C3", "BUCK2M",
			 "BUCK3M", "BUCK4M", "BUCK5M", "BUCK1M" },
	}, {
		.name = "S2MPG11",
		.rails = s2mpg11_rails, .nrails = ARRAY_SIZE(s2mpg11_rails),
		.reg_acc_mode = 0x03, .reg_lpf_mode = 0x04,
		.reg_bucken = 0x05, .reg_muxsel = 0x07,
		.def = { "VSEN_C6", "LDO2S", "BUCK9S", "BUCK5S",
			 "BUCK10S", "BUCK4S", "BUCK2S", "LDO9S" },
	},
};

struct odpm_chip {
	struct device *dev;
	struct regmap *map;
	const struct odpm_desc *desc;
	const struct odpm_rail *ch[NCH];
	ktime_t last;
};

static struct odpm_chip *odpm_chips[ARRAY_SIZE(odpm_descs)];
static DEFINE_MUTEX(odpm_lock);
static struct dentry *odpm_dir;

static bool dsm_trim;
module_param(dsm_trim, bool, 0444);
MODULE_PARM_DESC(dsm_trim, "Apply the vendor S2MPG10 DSM trim offset workaround (NOT idempotent: use at most once per boot)");

static const struct odpm_rail *odpm_find_rail(const struct odpm_desc *d,
					      const char *name)
{
	unsigned int i;

	for (i = 0; i < d->nrails; i++)
		if (!strcasecmp(d->rails[i].name, name))
			return &d->rails[i];
	return NULL;
}

/*
 * Latch the accumulators into the readable registers (and restart them).
 * ASYNC_RD is self-clearing, so it must not go through the regmap cache.
 */
static int odpm_async(struct odpm_chip *c)
{
	unsigned int v = 0;
	int ret, i;

	regcache_cache_bypass(c->map, true);
	ret = regmap_read(c->map, REG_CTRL2, &v);
	if (!ret)
		ret = regmap_write(c->map, REG_CTRL2, v | CTRL2_ASYNC_RD);
	c->last = ktime_get_boottime();
	for (i = 0; !ret && i < 20; i++) {
		ret = regmap_read(c->map, REG_CTRL2, &v);
		if (ret || !(v & CTRL2_ASYNC_RD))
			break;
		usleep_range(320, 420);	/* 40 us * 8 channels */
	}
	regcache_cache_bypass(c->map, false);
	if (!ret && (v & CTRL2_ASYNC_RD))
		ret = -ETIMEDOUT;
	return ret;
}

/* Program MUXSELn for all channels and the BUCKEN bits of metered bucks */
static int odpm_set_channels(struct odpm_chip *c)
{
	u8 bucken[2] = { 0, 0 };
	int ret, i;

	for (i = 0; i < NCH; i++) {
		u8 mux = c->ch[i]->mux;

		ret = regmap_update_bits(c->map, c->desc->reg_muxsel + i,
					 MUXSEL_MASK, mux);
		if (ret)
			return ret;
		if (mux >= 1 && mux <= 16)
			bucken[(mux - 1) / 8] |= BIT((mux - 1) % 8);
	}
	return regmap_bulk_write(c->map, c->desc->reg_bucken, bucken, 2);
}

static void odpm_dsm_trim(struct odpm_chip *c)
{
	unsigned int lo = 0, hi = 0;
	u8 trim, res;

	if (regmap_read(c->map, G10_DSM_TRIM_OFFSET, &lo) ||
	    regmap_read(c->map, G10_BUCK_METER_TRIM3, &hi))
		return;
	trim = (hi & 0xc0) | (lo & 0x3f);
	if (!(trim & BIT(7)) && trim >= 99)
		res = 127;
	else
		res = trim + 28;
	dev_info(c->dev, "DSM trim 0x%02x -> 0x%02x%s\n", trim, res,
		 dsm_trim ? "" : " (not applied, dsm_trim=0)");
	if (!dsm_trim)
		return;
	regmap_update_bits(c->map, G10_DSM_TRIM_OFFSET, 0x3f, res);
	regmap_update_bits(c->map, G10_BUCK_METER_TRIM3, 0xc0, res);
}

static int odpm_configure(struct odpm_chip *c)
{
	struct regmap *m = c->map;
	int ret;

	ret = regmap_update_bits(m, REG_CTRL1, CTRL1_INT_RATE,
				 CTRL1_INT_RATE_125HZ);
	if (!ret)
		ret = regmap_update_bits(m, REG_CTRL2, CTRL2_EXT_RATE,
					 CTRL2_EXT_RATE_30HZ);
	/* all channels: accumulate / low-pass filter power, not current */
	if (!ret)
		ret = regmap_write(m, c->desc->reg_acc_mode, 0x00);
	if (!ret)
		ret = regmap_write(m, c->desc->reg_lpf_mode, 0x00);
	if (!ret)
		ret = odpm_set_channels(c);
	/* external channel enables may only change with the ext meter off */
	if (!ret)
		ret = regmap_update_bits(m, REG_CTRL1, CTRL1_EXT_METER_EN, 0);
	if (!ret)
		ret = regmap_update_bits(m, REG_CTRL2, CTRL2_EXT_CH_EN,
					 CTRL2_EXT_CH_EN);
	if (!ret)
		ret = regmap_update_bits(m, REG_CTRL1, CTRL1_METER_EN,
					 CTRL1_METER_EN);
	if (!ret)
		ret = regmap_update_bits(m, REG_CTRL1, CTRL1_EXT_METER_EN,
					 CTRL1_EXT_METER_EN);
	if (!ret)
		ret = odpm_async(c);	/* discard whatever was accumulated */
	return ret;
}

static void odpm_show_chip(struct seq_file *s, struct odpm_chip *c, int base)
{
	u8 acc[NCH][6], lpf[NCH][3], cnt[3];
	ktime_t prev = c->last;
	unsigned int count;
	s64 ms;
	int ret, i;

	ret = odpm_async(c);
	for (i = 0; !ret && i < NCH; i++) {
		ret = regmap_bulk_read(c->map, REG_ACC_DATA + 6 * i, acc[i], 6);
		if (!ret)
			ret = regmap_bulk_read(c->map, REG_LPF_DATA + 3 * i,
					       lpf[i], 3);
	}
	if (!ret)
		ret = regmap_bulk_read(c->map, REG_ACC_COUNT, cnt, 3);
	if (ret) {
		seq_printf(s, "%s: read error %d\n", c->desc->name, ret);
		return;
	}

	count = cnt[0] | cnt[1] << 8 | (cnt[2] & 0x0f) << 16;
	ms = ktime_ms_delta(c->last, prev);
	seq_printf(s, "%s: %u samples in %lld ms (%lld mHz, nominal %d Hz)\n",
		   c->desc->name, count, ms,
		   ms > 0 ? div64_s64(1000000LL * count, ms) : 0, INT_RATE_HZ);

	for (i = 0; i < NCH; i++) {
		const struct odpm_rail *r = c->ch[i];
		u64 a = (get_unaligned_le32(acc[i]) | (u64)(acc[i][4] | (acc[i][5] & 1) << 8) << 32);
		u32 l = get_unaligned_le24(lpf[i]) & GENMASK(20, 0);
		u64 avg_uw = 0, lpf_uw;

		/* mean raw sample * pW/LSB */
		if (count)
			avg_uw = div_u64(mul_u64_u32_div(a, r->res_pw, count),
					 1000000);
		lpf_uw = div_u64((u64)l * r->res_pw, 1000000);
		seq_printf(s, "%2d %-9s %-20s avg %6llu.%03llu mW  lpf %6llu.%03llu mW  (acc 0x%011llx lpf 0x%06x)\n",
			   base + i, r->name, r->sch,
			   avg_uw / 1000, avg_uw % 1000,
			   lpf_uw / 1000, lpf_uw % 1000, a, l);
	}
}

static int odpm_power_show(struct seq_file *s, void *unused)
{
	int i;

	mutex_lock(&odpm_lock);
	for (i = 0; i < ARRAY_SIZE(odpm_chips); i++)
		if (odpm_chips[i])
			odpm_show_chip(s, odpm_chips[i], i * NCH);
	mutex_unlock(&odpm_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(odpm_power);

static int odpm_assign_show(struct seq_file *s, void *unused)
{
	unsigned int i, j;

	seq_puts(s, "usage: echo \"<channel 0-15> <rail>\" > assign\n");
	mutex_lock(&odpm_lock);
	for (i = 0; i < ARRAY_SIZE(odpm_chips); i++) {
		const struct odpm_desc *d = &odpm_descs[i];
		struct odpm_chip *c = odpm_chips[i];

		seq_printf(s, "%s (channels %u-%u)%s:\n", d->name, i * NCH,
			   i * NCH + NCH - 1, c ? "" : " [not probed]");
		for (j = 0; c && j < NCH; j++)
			seq_printf(s, "  ch%-2u = %s\n", i * NCH + j,
				   c->ch[j]->name);
		for (j = 0; j < d->nrails; j++)
			seq_printf(s, "  %-9s %s\n", d->rails[j].name,
				   d->rails[j].sch);
	}
	mutex_unlock(&odpm_lock);
	return 0;
}

static int odpm_assign_open(struct inode *inode, struct file *file)
{
	return single_open(file, odpm_assign_show, NULL);
}

static ssize_t odpm_assign_write(struct file *file, const char __user *ubuf,
				 size_t len, loff_t *ppos)
{
	const struct odpm_rail *r;
	struct odpm_chip *c;
	char buf[32], name[16];
	unsigned int ch;
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	if (sscanf(buf, "%u %15s", &ch, name) != 2 ||
	    ch >= NCH * ARRAY_SIZE(odpm_chips))
		return -EINVAL;

	mutex_lock(&odpm_lock);
	c = odpm_chips[ch / NCH];
	r = odpm_find_rail(&odpm_descs[ch / NCH], name);
	if (!c) {
		ret = -ENODEV;
	} else if (!r) {
		ret = -ENOENT;
	} else {
		c->ch[ch % NCH] = r;
		ret = odpm_set_channels(c);
		/* restart the averaging window of this PMIC */
		if (!ret)
			ret = odpm_async(c);
	}
	mutex_unlock(&odpm_lock);
	return ret ? ret : len;
}

static const struct file_operations odpm_assign_fops = {
	.owner = THIS_MODULE,
	.open = odpm_assign_open,
	.read = seq_read,
	.write = odpm_assign_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int odpm_probe(struct platform_device *pdev)
{
	unsigned int id = platform_get_device_id(pdev)->driver_data;
	struct odpm_chip *c;
	int ret, i;

	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = &pdev->dev;
	c->desc = &odpm_descs[id];
	c->map = dev_get_regmap(pdev->dev.parent, "meter");
	if (!c->map)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "parent has no \"meter\" regmap\n");
	for (i = 0; i < NCH; i++) {
		c->ch[i] = odpm_find_rail(c->desc, c->desc->def[i]);
		if (!c->ch[i])
			return -EINVAL;
	}

	if (id == 0)
		odpm_dsm_trim(c);
	ret = odpm_configure(c);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "meter setup failed\n");

	platform_set_drvdata(pdev, c);
	mutex_lock(&odpm_lock);
	odpm_chips[id] = c;
	mutex_unlock(&odpm_lock);
	dev_info(&pdev->dev, "%s power meter enabled\n", c->desc->name);
	return 0;
}

static void odpm_remove(struct platform_device *pdev)
{
	struct odpm_chip *c = platform_get_drvdata(pdev);

	mutex_lock(&odpm_lock);
	odpm_chips[c->desc - odpm_descs] = NULL;
	mutex_unlock(&odpm_lock);
	regmap_update_bits(c->map, REG_CTRL1,
			   CTRL1_METER_EN | CTRL1_EXT_METER_EN, 0);
}

static const struct platform_device_id odpm_ids[] = {
	{ "s2mpg10-meter", 0 },
	{ "s2mpg11-meter", 1 },
	{ }
};
MODULE_DEVICE_TABLE(platform, odpm_ids);

static struct platform_driver odpm_driver = {
	.driver = { .name = "pixel6-odpm" },
	.probe = odpm_probe,
	.remove = odpm_remove,
	.id_table = odpm_ids,
};

static int __init odpm_init(void)
{
	int ret;

	odpm_dir = debugfs_create_dir("odpm", NULL);
	debugfs_create_file("power", 0400, odpm_dir, NULL, &odpm_power_fops);
	debugfs_create_file("assign", 0600, odpm_dir, NULL, &odpm_assign_fops);
	ret = platform_driver_register(&odpm_driver);
	if (ret)
		debugfs_remove_recursive(odpm_dir);
	return ret;
}
module_init(odpm_init);

static void __exit odpm_exit(void)
{
	debugfs_remove_recursive(odpm_dir);
	platform_driver_unregister(&odpm_driver);
}
module_exit(odpm_exit);

MODULE_DESCRIPTION("Pixel 6 S2MPG10/S2MPG11 rail power meter (ODPM) debugfs reader");
MODULE_LICENSE("GPL");
