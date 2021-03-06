/*
 * Rockchip OTP Driver
 *
 * Copyright (c) 2018 Rockchip Electronics Co. Ltd.
 * Author: Finley Xiao <finley.xiao@rock-chips.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/reset.h>
#include <linux/rockchip/cpu.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

/* OTP Register Offsets */
#define OTPC_SBPI_CTRL			0x0020
#define OTPC_SBPI_CMD_VALID_PRE		0x0024
#define OTPC_SBPI_CS_VALID_PRE		0x0028
#define OTPC_SBPI_STATUS		0x002C
#define OTPC_USER_CTRL			0x0100
#define OTPC_USER_ADDR			0x0104
#define OTPC_USER_ENABLE		0x0108
#define OTPC_USER_Q			0x0124
#define OTPC_INT_STATUS			0x0304
#define OTPC_SBPI_CMD0_OFFSET		0x1000
#define OTPC_SBPI_CMD1_OFFSET		0x1004

#define OTPC_MODE_CTRL			0x2000
#define OTPC_IRQ_ST			0x2008
#define OTPC_ACCESS_ADDR		0x200c
#define OTPC_RD_DATA			0x2010
#define OTPC_REPR_RD_TRANS_NUM		0x2020

#define OTPC_DEEP_STANDBY		0x0
#define OTPC_STANDBY			0x1
#define OTPC_ACTIVE			0x2
#define OTPC_READ_ACCESS		0x3
#define OTPC_TRANS_NUM			0x1
#define OTPC_RDM_IRQ_ST			BIT(0)
#define OTPC_STB2ACT_IRQ_ST		BIT(7)
#define OTPC_DP2STB_IRQ_ST		BIT(8)
#define OTPC_ACT2STB_IRQ_ST		BIT(9)
#define OTPC_STB2DP_IRQ_ST		BIT(10)
#define RK3308BS_NBYTES			4
#define RK3308BS_NO_SECURE_OFFSET	224

/* OTP Register bits and masks */
#define OTPC_USER_ADDR_MASK		GENMASK(31, 16)
#define OTPC_USE_USER			BIT(0)
#define OTPC_USE_USER_MASK		GENMASK(16, 16)
#define OTPC_USER_FSM_ENABLE		BIT(0)
#define OTPC_USER_FSM_ENABLE_MASK	GENMASK(16, 16)
#define OTPC_SBPI_DONE			BIT(1)
#define OTPC_USER_DONE			BIT(2)

#define SBPI_DAP_ADDR			0x02
#define SBPI_DAP_ADDR_SHIFT		8
#define SBPI_DAP_ADDR_MASK		GENMASK(31, 24)
#define SBPI_CMD_VALID_MASK		GENMASK(31, 16)
#define SBPI_DAP_CMD_WRF		0xC0
#define SBPI_DAP_REG_ECC		0x3A
#define SBPI_ECC_ENABLE			0x00
#define SBPI_ECC_DISABLE		0x09
#define SBPI_ENABLE			BIT(0)
#define SBPI_ENABLE_MASK		GENMASK(16, 16)

#define OTPC_TIMEOUT			10000

struct rockchip_data;

struct rockchip_otp {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct clk *pclk;
	struct clk *pclk_phy;
	struct reset_control *rst;
	const struct rockchip_data *data;
};

struct rockchip_data {
	unsigned int size;
	nvmem_reg_read_t reg_read;
};

static int rockchip_otp_reset(struct rockchip_otp *otp)
{
	int ret;

	ret = reset_control_assert(otp->rst);
	if (ret) {
		dev_err(otp->dev, "failed to assert otp phy %d\n", ret);
		return ret;
	}

	udelay(2);

	ret = reset_control_deassert(otp->rst);
	if (ret) {
		dev_err(otp->dev, "failed to deassert otp phy %d\n", ret);
		return ret;
	}

	return 0;
}

static int px30_otp_wait_status(struct rockchip_otp *otp, u32 flag)
{
	u32 status = 0;
	int ret;

	ret = readl_poll_timeout_atomic(otp->base + OTPC_INT_STATUS, status,
					(status & flag), 1, OTPC_TIMEOUT);
	if (ret)
		return ret;

	/* clean int status */
	writel(flag, otp->base + OTPC_INT_STATUS);

	return 0;
}

static int px30_otp_ecc_enable(struct rockchip_otp *otp, bool enable)
{
	int ret = 0;

	writel(SBPI_DAP_ADDR_MASK | (SBPI_DAP_ADDR << SBPI_DAP_ADDR_SHIFT),
	       otp->base + OTPC_SBPI_CTRL);

	writel(SBPI_CMD_VALID_MASK | 0x1, otp->base + OTPC_SBPI_CMD_VALID_PRE);
	writel(SBPI_DAP_CMD_WRF | SBPI_DAP_REG_ECC,
	       otp->base + OTPC_SBPI_CMD0_OFFSET);
	if (enable)
		writel(SBPI_ECC_ENABLE, otp->base + OTPC_SBPI_CMD1_OFFSET);
	else
		writel(SBPI_ECC_DISABLE, otp->base + OTPC_SBPI_CMD1_OFFSET);

	writel(SBPI_ENABLE_MASK | SBPI_ENABLE, otp->base + OTPC_SBPI_CTRL);

	ret = px30_otp_wait_status(otp, OTPC_SBPI_DONE);
	if (ret < 0)
		dev_err(otp->dev, "timeout during ecc_enable\n");

	return ret;
}

static int px30_otp_read(void *context, unsigned int offset, void *val,
			 size_t bytes)
{
	struct rockchip_otp *otp = context;
	u8 *buf = val;
	int ret = 0;

	ret = clk_prepare_enable(otp->clk);
	if (ret < 0) {
		dev_err(otp->dev, "failed to prepare/enable otp clk\n");
		return ret;
	}

	ret = clk_prepare_enable(otp->pclk);
	if (ret < 0) {
		dev_err(otp->dev, "failed to prepare/enable otp pclk\n");
		goto otp_clk;
	}

	ret = clk_prepare_enable(otp->pclk_phy);
	if (ret < 0) {
		dev_err(otp->dev, "failed to prepare/enable otp pclk phy\n");
		goto opt_pclk;
	}

	ret = rockchip_otp_reset(otp);
	if (ret) {
		dev_err(otp->dev, "failed to reset otp phy\n");
		goto opt_pclk_phy;
	}

	ret = px30_otp_ecc_enable(otp, false);
	if (ret < 0) {
		dev_err(otp->dev, "rockchip_otp_ecc_enable err\n");
		goto opt_pclk_phy;
	}

	writel(OTPC_USE_USER | OTPC_USE_USER_MASK, otp->base + OTPC_USER_CTRL);
	udelay(5);
	while (bytes--) {
		writel(offset++ | OTPC_USER_ADDR_MASK,
		       otp->base + OTPC_USER_ADDR);
		writel(OTPC_USER_FSM_ENABLE | OTPC_USER_FSM_ENABLE_MASK,
		       otp->base + OTPC_USER_ENABLE);
		ret = px30_otp_wait_status(otp, OTPC_USER_DONE);
		if (ret < 0) {
			dev_err(otp->dev, "timeout during read setup\n");
			goto read_end;
		}
		*buf++ = readb(otp->base + OTPC_USER_Q);
	}

read_end:
	writel(0x0 | OTPC_USE_USER_MASK, otp->base + OTPC_USER_CTRL);
opt_pclk_phy:
	clk_disable_unprepare(otp->pclk_phy);
opt_pclk:
	clk_disable_unprepare(otp->pclk);
otp_clk:
	clk_disable_unprepare(otp->clk);

	return ret;
}

static int rk3308bs_otp_wait_status(struct rockchip_otp *otp, u32 flag)
{
	u32 status = 0;
	int ret;

	ret = readl_poll_timeout_atomic(otp->base + OTPC_IRQ_ST, status,
					(status & flag), 1, OTPC_TIMEOUT);
	if (ret)
		return ret;

	/* clean int status */
	writel(flag, otp->base + OTPC_IRQ_ST);

	return 0;
}

static int rk3308bs_otp_active(struct rockchip_otp *otp)
{
	int ret = 0;
	u32 mode;

	mode = readl(otp->base + OTPC_MODE_CTRL);

	switch (mode) {
	case OTPC_DEEP_STANDBY:
		writel(OTPC_STANDBY, otp->base + OTPC_MODE_CTRL);
		ret = rk3308bs_otp_wait_status(otp, OTPC_DP2STB_IRQ_ST);
		if (ret < 0) {
			dev_err(otp->dev, "timeout during wait dp2stb\n");
			return ret;
		}
		/* fall through */
	case OTPC_STANDBY:
		writel(OTPC_ACTIVE, otp->base + OTPC_MODE_CTRL);
		ret = rk3308bs_otp_wait_status(otp, OTPC_STB2ACT_IRQ_ST);
		if (ret < 0) {
			dev_err(otp->dev, "timeout during wait stb2act\n");
			return ret;
		}
		break;
	default:
		break;
	}

	return ret;
}

static int rk3308bs_otp_standby(struct rockchip_otp *otp)
{
	int ret = 0;
	u32 mode;

	mode = readl(otp->base + OTPC_MODE_CTRL);

	switch (mode) {
	case OTPC_ACTIVE:
		writel(OTPC_STANDBY, otp->base + OTPC_MODE_CTRL);
		ret = rk3308bs_otp_wait_status(otp, OTPC_ACT2STB_IRQ_ST);
		if (ret < 0) {
			dev_err(otp->dev, "timeout during wait act2stb\n");
			return ret;
		}
		/* fall through */
	case OTPC_STANDBY:
		writel(OTPC_DEEP_STANDBY, otp->base + OTPC_MODE_CTRL);
		ret = rk3308bs_otp_wait_status(otp, OTPC_STB2DP_IRQ_ST);
		if (ret < 0) {
			dev_err(otp->dev, "timeout during wait stb2dp\n");
			return ret;
		}
		break;
	default:
		break;
	}

	return ret;
}

static int rk3308bs_otp_read(void *context, unsigned int offset, void *val,
			     size_t bytes)
{
	struct rockchip_otp *otp = context;
	unsigned int addr_start, addr_end, addr_offset, addr_len;
	u32 out_value;
	u8 *buf;
	int ret, i = 0;

	if (offset >= otp->data->size)
		return -ENOMEM;
	if (offset + bytes > otp->data->size)
		bytes = otp->data->size - offset;

	ret = clk_prepare_enable(otp->clk);
	if (ret < 0) {
		dev_err(otp->dev, "failed to prepare/enable otp clk\n");
		return ret;
	}

	ret = clk_prepare_enable(otp->pclk);
	if (ret < 0) {
		dev_err(otp->dev, "failed to prepare/enable otp pclk\n");
		goto otp_clk;
	}

	ret = clk_prepare_enable(otp->pclk_phy);
	if (ret < 0) {
		dev_err(otp->dev, "failed to prepare/enable otp pclk phy\n");
		goto opt_pclk;
	}

	ret = rockchip_otp_reset(otp);
	if (ret) {
		dev_err(otp->dev, "failed to reset otp phy\n");
		goto opt_pclk_phy;
	}

	ret = rk3308bs_otp_active(otp);
	if (ret)
		goto opt_pclk_phy;

	addr_start = rounddown(offset, RK3308BS_NBYTES) / RK3308BS_NBYTES;
	addr_end = roundup(offset + bytes, RK3308BS_NBYTES) / RK3308BS_NBYTES;
	addr_offset = offset % RK3308BS_NBYTES;
	addr_len = addr_end - addr_start;
	addr_start += RK3308BS_NO_SECURE_OFFSET;

	buf = kzalloc(sizeof(*buf) * addr_len * RK3308BS_NBYTES, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto read_end;
	}

	while (addr_len--) {
		writel(OTPC_TRANS_NUM, otp->base + OTPC_REPR_RD_TRANS_NUM);
		writel(addr_start++, otp->base + OTPC_ACCESS_ADDR);
		writel(OTPC_READ_ACCESS, otp->base + OTPC_MODE_CTRL);
		ret = rk3308bs_otp_wait_status(otp, OTPC_RDM_IRQ_ST);
		if (ret < 0) {
			dev_err(otp->dev, "timeout during wait rd\n");
			goto read_end;
		}
		out_value = readl(otp->base + OTPC_RD_DATA);

		memcpy(&buf[i], &out_value, RK3308BS_NBYTES);
		i += RK3308BS_NBYTES;
	}
	memcpy(val, buf + addr_offset, bytes);

read_end:
	kfree(buf);
	rk3308bs_otp_standby(otp);
opt_pclk_phy:
	clk_disable_unprepare(otp->pclk_phy);
opt_pclk:
	clk_disable_unprepare(otp->pclk);
otp_clk:
	clk_disable_unprepare(otp->clk);

	return ret;
}

static int rockchip_otp_read(void *context, unsigned int offset, void *val,
			     size_t bytes)
{
	struct rockchip_otp *otp = context;

	return otp->data->reg_read(context, offset, val, bytes);
}

static struct nvmem_config otp_config = {
	.name = "rockchip-otp",
	.owner = THIS_MODULE,
	.read_only = true,
	.stride = 1,
	.word_size = 1,
	.reg_read = rockchip_otp_read,
};

static const struct rockchip_data px30_data = {
	.size = 0x40,
	.reg_read = px30_otp_read,
};

static const struct rockchip_data rk3308bs_data = {
	.size = 0x80,
	.reg_read = rk3308bs_otp_read,
};

static const struct of_device_id rockchip_otp_match[] = {
	{
		.compatible = "rockchip,px30-otp",
		.data = (void *)&px30_data,
	},
	{
		.compatible = "rockchip,px30s-otp",
		.data = (void *)&rk3308bs_data,
	},
	{
		.compatible = "rockchip,rk3308-otp",
		.data = (void *)&px30_data,
	},
	{
		.compatible = "rockchip,rk3308bs-otp",
		.data = (void *)&rk3308bs_data,
	},
	{ /* sentinel */},
};
MODULE_DEVICE_TABLE(of, rockchip_otp_match);

static int __init rockchip_otp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_otp *otp;
	const struct rockchip_data *data;
	struct resource *res;
	struct nvmem_device *nvmem;
	const struct of_device_id *match;

	match = of_match_device(dev->driver->of_match_table, dev);
	if (!match || !match->data) {
		dev_err(dev, "failed to get match data\n");
		return -EINVAL;
	}
	data = match->data;
	if (soc_is_rk3308bs() || soc_is_px30s())
		data = &rk3308bs_data;

	otp = devm_kzalloc(&pdev->dev, sizeof(struct rockchip_otp),
			   GFP_KERNEL);
	if (!otp)
		return -ENOMEM;
	otp->data = data;
	otp->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	otp->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(otp->base))
		return PTR_ERR(otp->base);

	otp->clk = devm_clk_get(&pdev->dev, "clk_otp");
	if (IS_ERR(otp->clk))
		return PTR_ERR(otp->clk);

	otp->pclk = devm_clk_get(&pdev->dev, "pclk_otp");
	if (IS_ERR(otp->pclk))
		return PTR_ERR(otp->pclk);

	otp->pclk_phy = devm_clk_get(&pdev->dev, "pclk_otp_phy");
	if (IS_ERR(otp->pclk_phy))
		return PTR_ERR(otp->pclk_phy);

	otp->rst = devm_reset_control_get(dev, "otp_phy");
	if (IS_ERR(otp->rst))
		return PTR_ERR(otp->rst);

	otp_config.size = data->size;
	otp_config.priv = otp;
	otp_config.dev = dev;
	nvmem = nvmem_register(&otp_config);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	platform_set_drvdata(pdev, nvmem);

	return 0;
}

static int rockchip_otp_remove(struct platform_device *pdev)
{
	struct nvmem_device *nvmem = platform_get_drvdata(pdev);

	return nvmem_unregister(nvmem);
}

static struct platform_driver rockchip_otp_driver = {
	.remove = rockchip_otp_remove,
	.driver = {
		.name = "rockchip-otp",
		.of_match_table = rockchip_otp_match,
	},
};

static int __init rockchip_otp_module_init(void)
{
	return platform_driver_probe(&rockchip_otp_driver,
				     rockchip_otp_probe);
}

subsys_initcall(rockchip_otp_module_init);

MODULE_DESCRIPTION("Rockchip OTP driver");
MODULE_LICENSE("GPL v2");
