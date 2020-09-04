// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2020 Caleb Connolly <caleb@connolly.tech>
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
 *   Copyright (c) 2020, The Linux Foundation. All rights reserved.
 *
 * Caleb Connolly <caleb@connolly.tech>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>

struct oneplus6_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct backlight_device *backlight;
	struct regulator *supply;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
	const struct drm_display_mode *mode;
	bool prepared;
	bool enabled;
};

static inline
struct oneplus6_panel *to_oneplus6_panel(struct drm_panel *panel)
{
	return container_of(panel, struct oneplus6_panel, panel);
}

#define dsi_dcs_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static void oneplus6_panel_reset(struct oneplus6_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
}

static int oneplus6_panel_on(struct oneplus6_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	usleep_range(10000, 11000);

	dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear on: %d\n", ret);
		return ret;
	}

	dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);
	dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);
	dsi_dcs_write_seq(dsi, 0xb0, 0x07);
	dsi_dcs_write_seq(dsi, 0xb6, 0x12);
	dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);
	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_POWER_SAVE, 0x00);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}

	return 0;
}

static int oneplus6_panel_off(struct oneplus6_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}
	msleep(40);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(160);

	return 0;
}

static int oneplus6_panel_prepare(struct drm_panel *panel)
{
	struct oneplus6_panel *ctx = to_oneplus6_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	oneplus6_panel_reset(ctx);

	ret = oneplus6_panel_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int oneplus6_panel_unprepare(struct drm_panel *panel)
{
	struct oneplus6_panel *ctx = to_oneplus6_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulator: %d\n", ret);
		return ret;
	}

	ret = oneplus6_panel_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	regulator_disable(ctx->supply);

	ctx->prepared = false;
	return 0;
}


static int oneplus6_panel_enable(struct drm_panel *panel)
{
	struct oneplus6_panel *ctx = to_oneplus6_panel(panel);
	int ret;

	if (ctx->enabled)
		return 0;

	ret = backlight_enable(ctx->backlight);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to enable backlight: %d\n", ret);
		return ret;
	}

	ctx->enabled = true;
	return 0;
}

static int oneplus6_panel_disable(struct drm_panel *panel)
{
	struct oneplus6_panel *ctx = to_oneplus6_panel(panel);
	int ret;

	if (!ctx->enabled)
		return 0;

	ret = backlight_disable(ctx->backlight);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to disable backlight: %d\n", ret);
		return ret;
	}

	ctx->enabled = false;
	return 0;
}


static const struct drm_display_mode enchilada_panel_mode = {
	.clock = (1080 + 112 + 16 + 36) * (2280 + 36 + 8 + 12) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 112,
	.hsync_end = 1080 + 112 + 16,
	.htotal = 1080 + 112 + 16 + 36,
	.vdisplay = 2280,
	.vsync_start = 2280 + 36,
	.vsync_end = 2280 + 36 + 8,
	.vtotal = 2280 + 36 + 8 + 12,
	.width_mm = 68,
	.height_mm = 145,
};

static const struct drm_display_mode fajita_panel_mode = {
	.clock = (1080 + 72 + 16 + 36) * (2340 + 32 + 4 + 18) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 72,
	.hsync_end = 1080 + 72 + 16,
	.htotal = 1080 + 72 + 16 + 36,
	.vdisplay = 2340,
	.vsync_start = 2340 + 32,
	.vsync_end = 2340 + 32 + 4,
	.vtotal = 2340 + 32 + 4 + 18,
	.width_mm = 68,
	.height_mm = 145,
};

static int oneplus6_panel_get_modes(struct drm_panel *panel,
						struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct oneplus6_panel *ctx = to_oneplus6_panel(panel);

	mode = drm_mode_duplicate(connector->dev, ctx->mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs oneplus6_panel_panel_funcs = {
	.disable = oneplus6_panel_disable,
	.enable = oneplus6_panel_enable,
	.prepare = oneplus6_panel_prepare,
	.unprepare = oneplus6_panel_unprepare,
	.get_modes = oneplus6_panel_get_modes,
};

static int oneplus6_panel_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	int err;
	u16 brightness = bl->props.brightness;

	err = mipi_dsi_dcs_get_display_brightness(dsi, &brightness);
	if (err < 0) {
		return err;
	}

	return brightness & 0xff;
}

static int oneplus6_panel_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	int err;
	unsigned short brightness;

	// This panel needs the high and low bytes swapped for the brightness value
	brightness = ((bl->props.brightness<<8)&0xff00)|((bl->props.brightness>>8)&0x00ff);

	err = mipi_dsi_dcs_set_display_brightness(dsi, brightness);
	if (err < 0) {
		return err;
	}

	return 0;
}

static const struct backlight_ops oneplus6_panel_bl_ops = {
	.update_status = oneplus6_panel_bl_update_status,
	.get_brightness = oneplus6_panel_bl_get_brightness,
};

static struct backlight_device *
oneplus6_panel_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.scale = BACKLIGHT_SCALE_LINEAR,
		.brightness = 255,
		.max_brightness = 512,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
						  &oneplus6_panel_bl_ops, &props);
}


static int oneplus6_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct oneplus6_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->mode = of_device_get_match_data(dev);

	if (!ctx->mode) {
		dev_err(dev, "Missing device mode\n");
		return -ENODEV;
	}

	ctx->supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->supply)) {
		ret = PTR_ERR(ctx->supply);
		dev_err(dev, "Failed to get vddio regulator: %d\n", ret);
		return ret;
	}

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		dev_warn(dev, "Failed to get reset-gpios: %d\n", ret);
		return ret;
	}

	ctx->backlight = oneplus6_panel_create_backlight(dsi);
	if (IS_ERR(ctx->backlight)) {
		ret = PTR_ERR(ctx->backlight);
		dev_err(dev, "Failed to create backlight: %d\n", ret);
		return ret;
	}

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;

	drm_panel_init(&ctx->panel, dev, &oneplus6_panel_panel_funcs,
			   DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0) {
		dev_err(dev, "Failed to add panel: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		return ret;
	}

	dev_info(dev, "Successfully added oneplus6 panel");

	return 0;
}

static int oneplus6_panel_remove(struct mipi_dsi_device *dsi)
{
	struct oneplus6_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id oneplus6_panel_of_match[] = {
	{
		.compatible = "samsung,sofef00",
		.data = &enchilada_panel_mode,
	},
	{
		.compatible = "samsung,s6e3fc2x01",
		.data = &fajita_panel_mode,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, oneplus6_panel_of_match);

static struct mipi_dsi_driver oneplus6_panel_driver = {
	.probe = oneplus6_panel_probe,
	.remove = oneplus6_panel_remove,
	.driver = {
		.name = "panel-oneplus6",
		.of_match_table = oneplus6_panel_of_match,
	},
};

module_mipi_dsi_driver(oneplus6_panel_driver);

MODULE_AUTHOR("Caleb Connolly <caleb@connolly.tech>");
MODULE_DESCRIPTION("DRM driver for Samsung AMOLED DSI panels found in OnePlus 6/6T phones");
MODULE_LICENSE("GPL v2");
