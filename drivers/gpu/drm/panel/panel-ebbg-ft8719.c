// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2021 venji10 <venji10@riseup.net>
 *
 * Based on panel-tianma-nt36672a.c:
 * Copyright (C) 2020 Linaro Ltd
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include <linux/gpio/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>

#include <linux/backlight.h>

#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#include <video/mipi_display.h>


struct panel_cmd {
	size_t len;
	const char *data;
};


#define dsi_generic_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_generic_write(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

#define dsi_dcs_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static const char * const regulator_names[] = {
	"vddio",
	"vddpos",
	"vddneg",
};

static unsigned long const regulator_enable_loads[] = {
	62000,
	100000,
	100000
};

static unsigned long const regulator_disable_loads[] = {
	80,
	100,
	100
};

struct panel_desc {
	const struct drm_display_mode *display_mode;
	const char *panel_name;

	unsigned int width_mm;
	unsigned int height_mm;

	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
};

struct panel_info {
	struct drm_panel base;
	struct mipi_dsi_device *link;
	const struct panel_desc *desc;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *reset_gpio;

	struct pinctrl *pinctrl;
	struct pinctrl_state *active;
	struct pinctrl_state *suspend;


	bool prepared;
	bool enabled;
};

static inline struct panel_info *to_panel_info(struct drm_panel *panel)
{
	return container_of(panel, struct panel_info, base);
}

static int panel_set_pinctrl_state(struct panel_info *panel, bool enable)
{
	int rc = 0;
	struct pinctrl_state *state;

	if (enable)
		state = panel->active;
	else
		state = panel->suspend;

	rc = pinctrl_select_state(panel->pinctrl, state);
	if (rc)
		pr_err("[%s] failed to set pin state, rc=%d\n", panel->desc->panel_name,
			rc);
	return rc;
}

static int ebbg_panel_disable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);

	pinfo->enabled = false;

	return 0;
}

static int ebbg_panel_power_off(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int i, ret = 0;

	gpiod_set_value(pinfo->reset_gpio, 0);

	ret = panel_set_pinctrl_state(pinfo, false);
	if (ret) {
		pr_err("[%s] failed to set pinctrl, rc=%d\n", pinfo->desc->panel_name, ret);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
				regulator_disable_loads[i]);
		if (ret) {
			DRM_DEV_ERROR(panel->dev,
				"regulator_set_load failed %d\n", ret);
			return ret;
		}
	}

	ret = regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret) {
		DRM_DEV_ERROR(panel->dev,
			"regulator_bulk_disable failed %d\n", ret);
	}
	return ret;
}

static int ebbg_panel_unprepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int ret;

	if (!pinfo->prepared)
		return 0;

    pinfo->link->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(pinfo->link);
	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
			"set_display_off cmd failed ret = %d\n",
			ret);
	}

	/* 120ms delay required here as per DCS spec */
	msleep(130);

	ret = mipi_dsi_dcs_enter_sleep_mode(pinfo->link);
	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
			"enter_sleep cmd failed ret = %d\n", ret);
	}
	/* 0x3C = 60ms delay */
	msleep(90);

	ret = ebbg_panel_power_off(panel);
	if (ret < 0)
		DRM_DEV_ERROR(panel->dev, "power_off failed ret = %d\n", ret);

	pinfo->prepared = false;

	return ret;

}

static int ebbg_panel_power_on(struct panel_info *pinfo)
{
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
					regulator_enable_loads[i]);
		if (ret)
			return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	ret = panel_set_pinctrl_state(pinfo, true);
	if (ret) {
		pr_err("[%s] failed to set pinctrl, rc=%d\n", pinfo->desc->panel_name, ret);
		return ret;
	}

	/* reset sequence */
	gpiod_set_value(pinfo->reset_gpio, 0);
	msleep(20);
	gpiod_set_value(pinfo->reset_gpio, 1);
	msleep(20);
	gpiod_set_value(pinfo->reset_gpio, 0);
	msleep(20);
	gpiod_set_value(pinfo->reset_gpio, 1);
	msleep(20);

	return 0;
}

static int ebbg_panel_prepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int err;

	if (pinfo->prepared)
		return 0;

	err = ebbg_panel_power_on(pinfo);
	if (err < 0)
		goto poweroff;

    pinfo->link->mode_flags |= MIPI_DSI_MODE_LPM;

	dsi_dcs_write_seq(pinfo->link, 0x00, 0x00);
	dsi_generic_write_seq(pinfo->link, 0xff, 0x87, 0x19, 0x01);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0x80);
	dsi_generic_write_seq(pinfo->link, 0xff, 0x87, 0x19);
    /* CABC dimming */
	dsi_dcs_write_seq(pinfo->link, 0x00, 0xa0);
	dsi_generic_write_seq(pinfo->link, 0x0f, 0x0f, 0x0f);
	/* CABC code */
	dsi_dcs_write_seq(pinfo->link, 0x00, 0x80);
	dsi_generic_write_seq(pinfo->link, 0xca, 0xbe, 0xb5, 0xad, 0xa6, 0xa0, 0x9b, 0x96, 0x91, 0x8d, 0x8a, 0x87, 0x83);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0x90);
	dsi_generic_write_seq(pinfo->link, 0xca, 0xfe, 0xff, 0x66, 0xf6, 0xff, 0x66, 0xfb, 0xff, 0x32);
	/* CE parameter */
	dsi_dcs_write_seq(pinfo->link, 0x00, 0xa0);
	dsi_generic_write_seq(pinfo->link, 0xd6, 0x7a, 0x79, 0x74, 0x8c, 0x8c, 0x92, 0x97, 0x9b, 0x97, 0x8f, 0x80, 0x77);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0xb0);
	dsi_generic_write_seq(pinfo->link, 0xd6, 0x7e, 0x7d, 0x81, 0x7a, 0x7a, 0x7b, 0x7c, 0x81, 0x84, 0x85, 0x80, 0x82);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0xc0);
	dsi_generic_write_seq(pinfo->link, 0xd6, 0x7d, 0x7d, 0x78, 0x8a, 0x89, 0x8f, 0x97, 0x97, 0x8f, 0x8c, 0x80, 0x7a);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0xd0);
	dsi_generic_write_seq(pinfo->link, 0xd6, 0x7e, 0x7d, 0x81, 0x7c, 0x79, 0x7b, 0x7c, 0x80, 0x84, 0x85, 0x80, 0x82);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0xe0);
	dsi_generic_write_seq(pinfo->link, 0xd6, 0x7b, 0x7b, 0x7b, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0xf0);
	dsi_generic_write_seq(pinfo->link, 0xd6, 0x7e, 0x7e, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0x00);
	dsi_generic_write_seq(pinfo->link, 0xd7, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0x10);
	dsi_generic_write_seq(pinfo->link, 0xd7, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80);
	/* CE parameter end*/
	dsi_dcs_write_seq(pinfo->link, 0x00, 0x00);
	dsi_generic_write_seq(pinfo->link, 0xff, 0x00, 0x00, 0x00);
	dsi_dcs_write_seq(pinfo->link, 0x00, 0x80);
	dsi_generic_write_seq(pinfo->link, 0xff, 0x00, 0x00);
	dsi_dcs_write_seq(pinfo->link, 0x91, 0x00);

	err = mipi_dsi_dcs_write(pinfo->link, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
		(u8[]){ 0xff}, 1);
	err = mipi_dsi_dcs_write(pinfo->link, MIPI_DCS_WRITE_CONTROL_DISPLAY,
		(u8[]){ 0x24}, 1);
	err = mipi_dsi_dcs_write(pinfo->link, MIPI_DCS_WRITE_POWER_SAVE,
		(u8[]){ 0x00}, 1);

	err = mipi_dsi_dcs_exit_sleep_mode(pinfo->link);
	if (err < 0) {
		DRM_DEV_ERROR(panel->dev, "Failed to exit sleep mode: %d\n", err);
		return err;
	}
	msleep(210);

	err = mipi_dsi_dcs_set_display_on(pinfo->link);
	if (err < 0) {
		DRM_DEV_ERROR(panel->dev, "Failed to set display on: %d\n", err);
		return err;
	}
	msleep(90);

	pinfo->prepared = true;

	return 0;

poweroff:
    DRM_DEV_ERROR(panel->dev, "goto poweroff");
	gpiod_set_value(pinfo->reset_gpio, 1);
	return err;
}


static int ebbg_panel_enable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);

	if (pinfo->enabled)
		return 0;

	pinfo->enabled = true;

	return 0;
}

static int ebbg_panel_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct panel_info *pinfo = to_panel_info(panel);
	const struct drm_display_mode *m = pinfo->desc->display_mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, m);
	if (!mode) {
		DRM_DEV_ERROR(panel->dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay, drm_mode_vrefresh(m));
		return -ENOMEM;
	}

	connector->display_info.width_mm = pinfo->desc->width_mm;
	connector->display_info.height_mm = pinfo->desc->height_mm;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs panel_funcs = {
	.disable = ebbg_panel_disable,
	.unprepare = ebbg_panel_unprepare,
	.prepare = ebbg_panel_prepare,
	.enable = ebbg_panel_enable,
	.get_modes = ebbg_panel_get_modes,
};

static const struct drm_display_mode ebbg_panel_default_mode = {
	.clock		= (1080 + 28 + 4 + 16) * (2246 + 120 + 4 + 12) * 60 / 1000,

	.hdisplay	= 1080,
	.hsync_start	= 1080 + 28,
	.hsync_end	= 1080 + 28 + 4,
	.htotal		= 1080 + 28 + 4 + 16,

	.vdisplay	= 2246,
	.vsync_start	= 2246 + 120,
	.vsync_end	= 2246 + 120 + 4,
	.vtotal		= 2246 + 120 + 4 + 12,

	.width_mm = 68,
	.height_mm = 141,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct panel_desc ebbg_panel_desc = {
	.display_mode = &ebbg_panel_default_mode,

	.width_mm = 68,
	.height_mm = 141,

	.mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_VIDEO
			| MIPI_DSI_MODE_VIDEO_HSE
			| MIPI_DSI_CLOCK_NON_CONTINUOUS
			| MIPI_DSI_MODE_VIDEO_BURST,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};


static const struct of_device_id panel_of_match[] = {
	{ .compatible = "ebbg,ft8719",
	  .data = &ebbg_panel_desc
	},
	{
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, panel_of_match);


static int panel_pinctrl_init(struct panel_info *panel)
{
	struct device *dev = &panel->link->dev;
	int rc = 0;

	panel->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(panel->pinctrl)) {
		rc = PTR_ERR(panel->pinctrl);
		pr_err("failed to get pinctrl, rc=%d\n", rc);
		goto error;
	}

	panel->active = pinctrl_lookup_state(panel->pinctrl,
							"panel_active");
	if (IS_ERR_OR_NULL(panel->active)) {
		rc = PTR_ERR(panel->active);
		pr_err("failed to get pinctrl active state, rc=%d\n", rc);
		goto error;
	}

	panel->suspend =
		pinctrl_lookup_state(panel->pinctrl, "panel_suspend");

	if (IS_ERR_OR_NULL(panel->suspend)) {
		rc = PTR_ERR(panel->suspend);
		pr_err("failed to get pinctrl suspend state, rc=%d\n", rc);
		goto error;
	}

error:
	return rc;
}

static int panel_add(struct panel_info *pinfo)
{
	struct device *dev = &pinfo->link->dev;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++)
		pinfo->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(pinfo->supplies),
				      pinfo->supplies);
	if (ret < 0)
		return ret;

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pinfo->reset_gpio)) {
		DRM_DEV_ERROR(dev, "cannot get reset gpio %ld\n",
			PTR_ERR(pinfo->reset_gpio));
		return PTR_ERR(pinfo->reset_gpio);
	}

	ret = panel_pinctrl_init(pinfo);
	if (ret < 0)
		return ret;

	drm_panel_init(&pinfo->base, dev, &panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&pinfo->base);
	return 0;
}

static void panel_del(struct panel_info *pinfo)
{
	if (pinfo->base.dev)
		drm_panel_remove(&pinfo->base);
}


static int ebbg_panel_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	int err;
	u8 brightness;

	brightness = (u8)backlight_get_brightness(bl);

	err = mipi_dsi_dcs_set_display_brightness(dsi, brightness);
	if (err < 0)
		return err;

	return 0;
}

static const struct backlight_ops ebbg_panel_bl_ops = {
	.update_status = ebbg_panel_bl_update_status,
};

static struct backlight_device *
ebbg_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.brightness = 255,
		.max_brightness = 255,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
						&ebbg_panel_bl_ops, &props);
}


static int panel_probe(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo;
	const struct panel_desc *desc;
	int err;

	pinfo = devm_kzalloc(&dsi->dev, sizeof(*pinfo), GFP_KERNEL);
	if (!pinfo)
		return -ENOMEM;

	desc = of_device_get_match_data(&dsi->dev);
	dsi->mode_flags = desc->mode_flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;
	pinfo->desc = desc;
	pinfo->link = dsi;

	mipi_dsi_set_drvdata(dsi, pinfo);


	pinfo->base.backlight = ebbg_create_backlight(dsi);
	if (IS_ERR(pinfo->base.backlight))
		return dev_err_probe(&dsi->dev, PTR_ERR(pinfo->base.backlight),
					"Failed to create backlight\n");

	err = panel_add(pinfo);
	if (err < 0)
		return err;

	err = mipi_dsi_attach(dsi);
	return err;
}

static int panel_remove(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	int err;

	err = ebbg_panel_unprepare(&pinfo->base);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to unprepare panel: %d\n",
				err);

	err = ebbg_panel_disable(&pinfo->base);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to disable panel: %d\n", err);

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to detach from DSI host: %d\n",
				err);
	panel_del(pinfo);

	return 0;
}

static void panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);

	ebbg_panel_disable(&pinfo->base);
	ebbg_panel_unprepare(&pinfo->base);
}

static struct mipi_dsi_driver panel_driver = {
	.driver = {
		.name = "panel-ebbg-ft8719",
		.of_match_table = panel_of_match,
	},
	.probe = panel_probe,
	.remove = panel_remove,
	.shutdown = panel_shutdown,
};
module_mipi_dsi_driver(panel_driver);

MODULE_AUTHOR("Sumit Semwal <sumit.semwal@linaro.org>");
MODULE_DESCRIPTION("EBBG FT8719 MIPI-DSI LCD panel");
MODULE_LICENSE("GPL");
