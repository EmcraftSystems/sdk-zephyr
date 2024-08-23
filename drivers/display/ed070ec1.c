/*
 * Copyright (c) 2024 Emcraft Systems
 * Copyright (c) 2022 Andreas Sandberg
 * Copyright (c) 2018-2020 PHYTEC Messtechnik GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_LEVEL CONFIG_DISPLAY_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ed070ec1);

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/byteorder.h>

#include <nrfx_spim.h>

#include <zephyr/display/ed070ec1.h>
#include "ed070ec1_regs.h"

/**
 * ED070EC1 compatible EPD controller driver.
 */

#define EPD_PANEL_NUMOF_ROWS_PER_PAGE 8
#define ED070EC1_PANEL_FIRST_PAGE     0
#define ED070EC1_PANEL_FIRST_GATE     0
#define ED070EC1_PIXELS_PER_BYTE      8
#define ED070EC1_DEFAULT_TR_VALUE     25U
#define ED070EC1_TR_SCALE_FACTOR      256U

enum ed070ec1_profile_type {
	ED070EC1_PROFILE_FULL = 0,
	ED070EC1_PROFILE_PARTIAL,
	ED070EC1_NUM_PROFILES,
	ED070EC1_PROFILE_INVALID = ED070EC1_NUM_PROFILES,
};

/* Delays */
#define ED070_RESET_DELAY   3000
#define MSG_SEND_DELAY      1000
#define MYSTERY_SETUP_DELAY 10000
#define ACTIVATION_DELAY    20000

/* Pins */
/** @brief Symbol specifying pin number for MOSI. */
#define MOSI_PIN (9)
/** @brief Symbol specifying pin number for MISO. */
#define SCK_PIN  (8)
#define DCX_PIN  (12)
#define CS_PIN   (11)

static K_SEM_DEFINE(transfer_finished, 0, 1);

struct ed070ec1_quirks {
	/* Gates */
	uint16_t max_width;
	/* Sources */
	uint16_t max_height;
	/* Width (bits) of integer type representing an x coordinate */
	uint8_t pp_width_bits;
	/* Width (bits) of integer type representing a y coordinate */
	uint8_t pp_height_bits;

	/*
	 * Device specific flags to be included in
	 * ED070EC1_CMD_UPDATE_CTRL2 for a full refresh.
	 */
	uint8_t ctrl2_full;
	/*
	 * Device specific flags to be included in
	 * ED070EC1_CMD_UPDATE_CTRL2 for a partial refresh.
	 */
	uint8_t ctrl2_partial;
};

struct ed070ec1_data {
	bool read_supported;
	uint8_t scan_mode;
	bool blanking_on;
	enum ed070ec1_profile_type profile;
	nrfx_spim_t spim_inst;
};

struct ed070ec1_dt_array {
	uint8_t *data;
	uint8_t len;
};

struct ed070ec1_profile {
	struct ed070ec1_dt_array lut;
	struct ed070ec1_dt_array gdv;
	struct ed070ec1_dt_array sdv;
	uint8_t vcom;
	uint8_t bwf;
	uint8_t dummy_line;
	uint8_t gate_line_width;

	bool override_vcom;
	bool override_bwf;
	bool override_dummy_line;
	bool override_gate_line_width;
};

struct ed070ec1_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec dc_gpio;
	struct gpio_dt_spec busy_gpio;
	struct gpio_dt_spec reset_gpio;

	const struct ed070ec1_quirks *quirks;

	struct ed070ec1_dt_array softstart;

	const struct ed070ec1_profile *profiles[ED070EC1_NUM_PROFILES];

	bool orientation;
	uint16_t height;
	uint16_t width;
	uint8_t tssv;
};

static int ed070ec1_set_profile(const struct device *dev, enum ed070ec1_profile_type type);

static inline void ed070ec1_busy_wait(const struct device *dev)
{
	const struct ed070ec1_config *config = dev->config;
	int pin = gpio_pin_get_dt(&config->busy_gpio);

	while (pin > 0) {
		__ASSERT(pin >= 0, "Failed to get pin level");
		k_msleep(ED070EC1_BUSY_DELAY);
		pin = gpio_pin_get_dt(&config->busy_gpio);
	}
}

static inline int ed070ec1_write_cmd(const struct device *dev, uint8_t cmd, const uint8_t *data,
				     size_t len)
{
	const struct ed070ec1_data *dev_data = dev->data;
	static uint8_t buf[8];
	uint8_t size;

	ed070ec1_busy_wait(dev);

	buf[0] = cmd;
	size = sizeof(cmd);

	nrfx_spim_xfer_desc_t spim_xfer_desc = NRFX_SPIM_XFER_TX(buf, size);

	nrfx_spim_xfer_dcx(&dev_data->spim_inst, &spim_xfer_desc, 0, 1);
	NRFX_DELAY_US(MSG_SEND_DELAY);
	if (len > 0) {
		nrfx_spim_xfer_desc_t spim_xfer_desc1 = NRFX_SPIM_XFER_TX(data, len);

		nrfx_spim_xfer_dcx(&dev_data->spim_inst, &spim_xfer_desc1, 0, 0);
		if (k_sem_take(&transfer_finished, K_MSEC(100)) != 0) {
			LOG_ERR("SPIM transfer timeout\n");
			return -1;
		}
	}

	return 0;
}

static inline int ed070ec1_write_uint8(const struct device *dev, uint8_t cmd, uint8_t data)
{
	return ed070ec1_write_cmd(dev, cmd, &data, 1);
}

static inline int ed070ec1_read_cmd(const struct device *dev, uint8_t cmd, uint8_t *data,
				    size_t len)
{
	const struct ed070ec1_config *config = dev->config;
	const struct ed070ec1_data *dev_data = dev->data;
	struct spi_buf buf = {.buf = &cmd, .len = sizeof(cmd)};
	struct spi_buf_set buf_set = {.buffers = &buf, .count = 1};
	int err = 0;

	if (!dev_data->read_supported) {
		return -ENOTSUP;
	}

	ed070ec1_busy_wait(dev);

	err = gpio_pin_set_dt(&config->dc_gpio, 1);
	if (err < 0) {
		return err;
	}

	err = spi_write_dt(&config->bus, &buf_set);
	if (err < 0) {
		goto spi_out;
	}

	if (data != NULL) {
		buf.buf = data;
		buf.len = len;

		err = gpio_pin_set_dt(&config->dc_gpio, 0);
		if (err < 0) {
			goto spi_out;
		}

		err = spi_read_dt(&config->bus, &buf_set);
		if (err < 0) {
			goto spi_out;
		}
	}

spi_out:
	spi_release_dt(&config->bus);
	return err;
}

static inline size_t push_x_param(const struct device *dev, uint8_t *data, uint16_t x)
{
	const struct ed070ec1_config *config = dev->config;

	if (config->quirks->pp_width_bits == 8) {
		data[0] = (uint8_t)x;
		return 1;
	}

	if (config->quirks->pp_width_bits == 16) {
		sys_put_le16(sys_cpu_to_le16(x), data);
		return 2;
	}

	LOG_ERR("Unsupported pp_width_bits %u", config->quirks->pp_width_bits);
	return 0;
}

static inline size_t push_y_param(const struct device *dev, uint8_t *data, uint16_t y)
{
	const struct ed070ec1_config *config = dev->config;

	if (config->quirks->pp_height_bits == 8) {
		data[0] = (uint8_t)y;
		return 1;
	}

	if (config->quirks->pp_height_bits == 16) {
		sys_put_le16(sys_cpu_to_le16(y), data);
		return 2;
	}

	LOG_ERR("Unsupported pp_height_bitsa %u", config->quirks->pp_height_bits);
	return 0;
}

static inline int ed070ec1_set_ram_param(const struct device *dev, uint16_t sx, uint16_t ex,
					 uint16_t sy, uint16_t ey)
{
	int err;
	uint8_t tmp[4];
	size_t len;

	LOG_DBG("sx: %d ex: %d sy: %d ey: %d", sx, ex, sy, ey);
	len = push_x_param(dev, tmp, sx);
	len += push_x_param(dev, tmp + len, ex);
	err = ed070ec1_write_cmd(dev, ED070EC1_CMD_RAM_XPOS_CTRL, tmp, len);
	if (err < 0) {
		return err;
	}

	len = push_y_param(dev, tmp, sy);
	len += push_y_param(dev, tmp + len, ey);
	err = ed070ec1_write_cmd(dev, ED070EC1_CMD_RAM_YPOS_CTRL, tmp, len);
	if (err < 0) {
		return err;
	}

	return 0;
}

static inline int ed070ec1_set_ram_ptr(const struct device *dev, uint16_t x, uint16_t y)
{
	int err;
	uint8_t tmp[2];
	size_t len;

	len = push_x_param(dev, tmp, x);
	err = ed070ec1_write_cmd(dev, ED070EC1_CMD_RAM_XPOS_CNTR, tmp, len);
	if (err < 0) {
		return err;
	}

	len = push_y_param(dev, tmp, y);
	return ed070ec1_write_cmd(dev, ED070EC1_CMD_RAM_YPOS_CNTR, tmp, len);
}

static int ed070ec1_activate(const struct device *dev, uint8_t ctrl2)
{
	int err;

	err = ed070ec1_write_uint8(dev, ED070EC1_CMD_UPDATE_CTRL2, ctrl2);
	if (err < 0) {
		return err;
	}

	return ed070ec1_write_cmd(dev, ED070EC1_CMD_MASTER_ACTIVATION, NULL, 0);
}

static int ed070ec1_update_display(const struct device *dev)
{
	const struct ed070ec1_config *config = dev->config;
	const struct ed070ec1_data *data = dev->data;
	const struct ed070ec1_profile *p = config->profiles[data->profile];
	const struct ed070ec1_quirks *quirks = config->quirks;
	const bool load_lut = !p || p->lut.len == 0;
	const bool load_temp = load_lut && config->tssv;
	const bool partial = data->profile == ED070EC1_PROFILE_PARTIAL;
	const uint8_t update_cmd = ED070EC1_CTRL2_ENABLE_CLK | ED070EC1_CTRL2_ENABLE_ANALOG |
				   (load_lut ? ED070EC1_CTRL2_LOAD_LUT : 0) |
				   (load_temp ? ED070EC1_CTRL2_LOAD_TEMPERATURE : 0) |
				   (partial ? quirks->ctrl2_partial : quirks->ctrl2_full) |
				   ED070EC1_CTRL2_DISABLE_ANALOG | ED070EC1_CTRL2_DISABLE_CLK;

	return ed070ec1_activate(dev, update_cmd);
}

static int ed070ec1_blanking_off(const struct device *dev)
{
	struct ed070ec1_data *data = dev->data;

	if (data->blanking_on) {
		data->blanking_on = false;
		return ed070ec1_update_display(dev);
	}

	return 0;
}

static int ed070ec1_blanking_on(const struct device *dev)
{
	struct ed070ec1_data *data = dev->data;

	if (!data->blanking_on) {
		if (ed070ec1_set_profile(dev, ED070EC1_PROFILE_FULL)) {
			return -EIO;
		}
	}

	data->blanking_on = true;

	return 0;
}

static int ed070ec1_set_window(const struct device *dev, const uint16_t x, const uint16_t y,
			       const struct display_buffer_descriptor *desc)
{
	const struct ed070ec1_config *config = dev->config;
	int err;
	uint16_t x_start;
	uint16_t x_end;
	uint16_t y_start;
	uint16_t y_end;
	uint16_t panel_h = config->height - config->height % EPD_PANEL_NUMOF_ROWS_PER_PAGE;

	if (desc->pitch < desc->width) {
		LOG_ERR("Pitch is smaller than width");
		return -EINVAL;
	}

	if (desc->pitch > desc->width) {
		LOG_ERR("Unsupported mode");
		return -ENOTSUP;
	}

	if ((y + desc->height) > panel_h) {
		LOG_ERR("Buffer out of bounds (height)");
		return -EINVAL;
	}

	if ((x + desc->width) > config->width) {
		LOG_ERR("Buffer out of bounds (width)");
		return -EINVAL;
	}

	if ((desc->height % EPD_PANEL_NUMOF_ROWS_PER_PAGE) != 0U) {
		LOG_ERR("Buffer height not multiple of %d", EPD_PANEL_NUMOF_ROWS_PER_PAGE);
		return -EINVAL;
	}

	if ((y % EPD_PANEL_NUMOF_ROWS_PER_PAGE) != 0U) {
		LOG_ERR("Y coordinate not multiple of %d", EPD_PANEL_NUMOF_ROWS_PER_PAGE);
		return -EINVAL;
	}

	/* TBD: Support set window */
	x_start = 0;
	x_end = desc->width - 1;
	y_start = y;
	y_end = y + desc->height;

	err = ed070ec1_set_ram_param(dev, x_start, x_end, y_start, y_end);
	if (err < 0) {
		return err;
	}

	err = ed070ec1_set_ram_ptr(dev, x_start, y_start);
	if (err < 0) {
		return err;
	}

	return 0;
}

static int ed070ec1_write(const struct device *dev, const uint16_t x, const uint16_t y,
			  const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct ed070ec1_config *config = dev->config;
	const struct ed070ec1_data *data = dev->data;
	const bool have_partial_refresh = config->profiles[ED070EC1_PROFILE_PARTIAL] != NULL;
	const bool partial_refresh = !data->blanking_on && have_partial_refresh;
	const size_t buf_len = MIN(desc->buf_size, desc->height * desc->width / 8);
	int err;

	if (buf == NULL || buf_len == 0U) {
		LOG_ERR("Display buffer is not available");
		return -EINVAL;
	}

	if (partial_refresh) {
		/*
		 * Request the partial profile. This operation becomes
		 * a no-op if the profile is already active.
		 */
		err = ed070ec1_set_profile(dev, ED070EC1_PROFILE_PARTIAL);
		if (err < 0) {
			return -EIO;
		}
	}

	err = ed070ec1_set_window(dev, x, y, desc);
	if (err < 0) {
		return err;
	}

	err = ed070ec1_write_cmd(dev, ED070EC1_CMD_WRITE_RAM, (uint8_t *)buf, buf_len);
	if (err < 0) {
		return err;
	}

	if (!data->blanking_on) {
		err = ed070ec1_update_display(dev);
		if (err < 0) {
			return err;
		}
	}

	if (data->blanking_on && have_partial_refresh) {
		/*
		 * We will trigger a full refresh when blanking is
		 * turned off. The controller won't keep track of the
		 * old frame buffer, which is needed to perform a
		 * partial update, when this happens. Maintain the old
		 * frame buffer manually here to make sure future
		 * partial updates will work as expected.
		 */
		err = ed070ec1_write_cmd(dev, ED070EC1_CMD_WRITE_RED_RAM, (uint8_t *)buf, buf_len);
		if (err < 0) {
			return err;
		}
	} else if (partial_refresh) {
		/*
		 * We just performed a partial refresh. After the
		 * refresh, the controller swaps the black/red buffers
		 * containing the current and new image. We need to
		 * perform a second write here to ensure that future
		 * updates work on an up-to-date framebuffer.
		 */
		err = ed070ec1_write_cmd(dev, ED070EC1_CMD_WRITE_RAM, (uint8_t *)buf, buf_len);
		if (err < 0) {
			return err;
		}
	}

	return 0;
}

int ed070ec1_read_ram(const struct device *dev, enum ed070ec1_ram ram_type, const uint16_t x,
		      const uint16_t y, const struct display_buffer_descriptor *desc, void *buf)
{
	const struct ed070ec1_data *data = dev->data;
	const size_t buf_len = MIN(desc->buf_size, desc->height * desc->width / 8);
	int err;
	uint8_t ram_ctrl;

	if (!data->read_supported) {
		return -ENOTSUP;
	}

	switch (ram_type) {
	case ED070EC1_RAM_BLACK:
		ram_ctrl = ED070EC1_RAM_READ_CTRL_BLACK;
		break;

	case ED070EC1_RAM_RED:
		ram_ctrl = ED070EC1_RAM_READ_CTRL_RED;
		break;

	default:
		return -EINVAL;
	}

	if (buf == NULL || buf_len == 0U) {
		LOG_ERR("Display buffer is not available");
		return -EINVAL;
	}

	err = ed070ec1_set_window(dev, x, y, desc);
	if (err < 0) {
		return err;
	}

	err = ed070ec1_write_cmd(dev, ED070EC1_CMD_RAM_READ_CTRL, &ram_ctrl, sizeof(ram_ctrl));
	if (err < 0) {
		return err;
	}

	err = ed070ec1_read_cmd(dev, ED070EC1_CMD_READ_RAM, (uint8_t *)buf, buf_len);
	if (err < 0) {
		return err;
	}

	return 0;
}

static int ed070ec1_read(const struct device *dev, const uint16_t x, const uint16_t y,
			 const struct display_buffer_descriptor *desc, void *buf)
{
	return ed070ec1_read_ram(dev, ED070EC1_RAM_BLACK, x, y, desc, buf);
}

static void *ed070ec1_get_framebuffer(const struct device *dev)
{
	LOG_ERR("not supported");
	return NULL;
}

static int ed070ec1_set_brightness(const struct device *dev, const uint8_t brightness)
{
	LOG_WRN("not supported");
	return -ENOTSUP;
}

static int ed070ec1_set_contrast(const struct device *dev, uint8_t contrast)
{
	LOG_WRN("not supported");
	return -ENOTSUP;
}

static void ed070ec1_get_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	const struct ed070ec1_config *config = dev->config;

	memset(caps, 0, sizeof(struct display_capabilities));
	caps->x_resolution = config->width;
	caps->y_resolution = config->height - config->height % EPD_PANEL_NUMOF_ROWS_PER_PAGE;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO10;
	caps->current_pixel_format = PIXEL_FORMAT_MONO10;
	caps->screen_info = SCREEN_INFO_MONO_VTILED | SCREEN_INFO_MONO_MSB_FIRST | SCREEN_INFO_EPD;
}

static int ed070ec1_set_orientation(const struct device *dev,
				    const enum display_orientation orientation)
{
	LOG_ERR("Unsupported");
	return -ENOTSUP;
}

static int ed070ec1_set_pixel_format(const struct device *dev, const enum display_pixel_format pf)
{
	if (pf == PIXEL_FORMAT_MONO10) {
		return 0;
	}

	LOG_ERR("not supported");
	return -ENOTSUP;
}

static int ed070ec1_clear_cntlr_mem(const struct device *dev, uint8_t ram_cmd)
{
	const struct ed070ec1_config *config = dev->config;
	uint16_t panel_h = config->height / EPD_PANEL_NUMOF_ROWS_PER_PAGE;
	uint16_t last_gate = config->width - 1;
	uint8_t clear_page[64];
	int err;

	/*
	 * Clear unusable memory area when the resolution of the panel is not
	 * multiple of an octet.
	 */
	if (config->height % EPD_PANEL_NUMOF_ROWS_PER_PAGE) {
		panel_h += 1;
	}
	err = ed070ec1_set_ram_param(dev, ED070EC1_PANEL_FIRST_PAGE, panel_h - 1, last_gate,
				     ED070EC1_PANEL_FIRST_GATE);
	if (err < 0) {
		return err;
	}

	err = ed070ec1_set_ram_ptr(dev, ED070EC1_PANEL_FIRST_PAGE, last_gate);
	if (err < 0) {
		return err;
	}

	memset(clear_page, 0xff, sizeof(clear_page));
	for (int h = 0; h < panel_h; h++) {
		size_t x = config->height;

		while (x) {
			size_t l = MIN(x, sizeof(clear_page));

			x -= l;
			err = ed070ec1_write_cmd(dev, ram_cmd, clear_page, l);
			if (err < 0) {
				return err;
			}
		}
	}

	return 0;
}

static inline int ed070ec1_load_ws_from_otp_tssv(const struct device *dev)
{
	const struct ed070ec1_config *config = dev->config;

	/*
	 * Controller has an integrated temperature sensor or external
	 * temperature sensor is connected to the controller.
	 */
	LOG_INF("Select and load WS from OTP");
	return ed070ec1_write_uint8(dev, ED070EC1_CMD_TSENSOR_SELECTION, config->tssv);
}

static inline int ed070ec1_load_ws_from_otp(const struct device *dev)
{
	int16_t t = (ED070EC1_DEFAULT_TR_VALUE * ED070EC1_TR_SCALE_FACTOR);
	uint8_t tmp[2];
	int err;

	LOG_INF("Load default WS (25 degrees Celsius) from OTP");

	err = ed070ec1_activate(dev, ED070EC1_CTRL2_ENABLE_CLK);
	if (err < 0) {
		return err;
	}

	/* Load temperature value */
	sys_put_be16(t, tmp);
	err = ed070ec1_write_cmd(dev, ED070EC1_CMD_TSENS_CTRL, tmp, 2);
	if (err < 0) {
		return err;
	}

	err = ed070ec1_activate(dev, ED070EC1_CTRL2_DISABLE_CLK);
	if (err < 0) {
		return err;
	}

	return 0;
}

static int ed070ec1_load_lut(const struct device *dev, const struct ed070ec1_dt_array *lut)
{
	const struct ed070ec1_config *config = dev->config;

	if (lut && lut->len) {
		LOG_DBG("Using user-provided LUT");
		return ed070ec1_write_cmd(dev, ED070EC1_CMD_UPDATE_LUT, lut->data, lut->len);
	}
	if (config->tssv) {
		return ed070ec1_load_ws_from_otp_tssv(dev);
	} else {
		return ed070ec1_load_ws_from_otp(dev);
	}
}

static int ed070ec1_set_profile(const struct device *dev, enum ed070ec1_profile_type type)
{
	const struct ed070ec1_config *config = dev->config;
	struct ed070ec1_data *data = dev->data;
	const struct ed070ec1_profile *p;
	const uint16_t last_gate = config->height - 1;
	uint8_t gdo[3];
	size_t gdo_len;
	int err = 0;

	if (type >= ED070EC1_NUM_PROFILES) {
		return -EINVAL;
	}

	p = config->profiles[type];

	/*
	 * The full profile is the only one that always exists. If it
	 * hasn't been specified, we use the defaults.
	 */
	if (!p && type != ED070EC1_PROFILE_FULL) {
		return -ENOENT;
	}

	if (type == data->profile) {
		return 0;
	}

	/*
	 * Perform a soft reset to make sure registers are reset. This
	 * will leave the RAM contents intact.
	 */
	err = ed070ec1_write_cmd(dev, ED070EC1_CMD_SW_RESET, NULL, 0);
	if (err < 0) {
		return err;
	}

	gdo_len = push_y_param(dev, gdo, last_gate);
	gdo[gdo_len++] = 0U;
	err = ed070ec1_write_cmd(dev, ED070EC1_CMD_GDO_CTRL, gdo, gdo_len);
	if (err < 0) {
		return err;
	}

	if (config->softstart.len) {
		err = ed070ec1_write_cmd(dev, ED070EC1_CMD_SOFTSTART, config->softstart.data,
					 config->softstart.len);
		if (err < 0) {
			return err;
		}
	}

	err = ed070ec1_load_lut(dev, p ? &p->lut : NULL);
	if (err < 0) {
		return err;
	}

	if (p && p->override_dummy_line) {
		err = ed070ec1_write_uint8(dev, ED070EC1_CMD_DUMMY_LINE, p->dummy_line);
		if (err < 0) {
			return err;
		}
	}

	if (p && p->override_gate_line_width) {
		err = ed070ec1_write_uint8(dev, ED070EC1_CMD_GATE_LINE_WIDTH,
					   p->override_gate_line_width);
		if (err < 0) {
			return err;
		}
	}

	if (p && p->gdv.len) {
		LOG_DBG("Setting GDV");
		err = ed070ec1_write_cmd(dev, ED070EC1_CMD_GDV_CTRL, p->gdv.data, p->gdv.len);
		if (err < 0) {
			return err;
		}
	}

	if (p && p->sdv.len) {
		LOG_DBG("Setting SDV");
		err = ed070ec1_write_cmd(dev, ED070EC1_CMD_SDV_CTRL, p->sdv.data, p->sdv.len);
		if (err < 0) {
			return err;
		}
	}

	if (p && p->override_vcom) {
		LOG_DBG("Setting VCOM");
		err = ed070ec1_write_cmd(dev, ED070EC1_CMD_VCOM_VOLTAGE, &p->vcom, 1);
		if (err < 0) {
			return err;
		}
	}

	if (p && p->override_bwf) {
		LOG_DBG("Setting BWF");
		err = ed070ec1_write_cmd(dev, ED070EC1_CMD_BWF_CTRL, &p->bwf, 1);
		if (err < 0) {
			return err;
		}
	}

	data->profile = type;

	return 0;
}

static int ed070ec1_controller_init(const struct device *dev)
{
	const struct ed070ec1_config *config = dev->config;
	struct ed070ec1_data *data = dev->data;
	int err;

	data->blanking_on = false;
	data->profile = ED070EC1_PROFILE_INVALID;

	nrf_gpio_pin_clear(config->reset_gpio.pin);
	k_msleep(ED070EC1_RESET_DELAY);
	nrf_gpio_pin_set(config->reset_gpio.pin);
	k_msleep(ED070EC1_RESET_DELAY);

	data->scan_mode = ED070EC1_DATA_ENTRY_XIYDY;

	err = ed070ec1_set_profile(dev, ED070EC1_PROFILE_FULL);
	if (err < 0) {
		return err;
	}

	/* TBD: Clear memory */

	err = ed070ec1_update_display(dev);
	if (err < 0) {
		return err;
	}

	return 0;
}


/**
 * @brief Function for handling SPIM driver events.
 *
 * @param[in] p_event   Pointer to the SPIM driver event.
 * @param[in] p_context Pointer to the context passed from the driver.
 */
static void spim_handler(nrfx_spim_evt_t const *p_event, void *p_context)
{
	if (p_event->type == NRFX_SPIM_EVENT_DONE) {
		k_sem_give(&transfer_finished);
	}
}

#define MOSI_PIN (9)
/** @brief Symbol specifying pin number for MISO. */
#define SCK_PIN  (8)
#define DCX_PIN  (12)
#define CS_PIN   (11)

#define SPIM_INST_IDX 4

static int ed070ec1_init(const struct device *dev)
{
	const struct ed070ec1_config *config = dev->config;
	struct ed070ec1_data *data = dev->data;
	int err;
	nrfx_err_t status;
	(void)status;

	nrf_gpio_cfg(config->reset_gpio.pin, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
		     NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);

	nrfx_spim_t spim_inst = NRFX_SPIM_INSTANCE(SPIM_INST_IDX);

	data->spim_inst = spim_inst;
	nrfx_spim_config_t spim_config =
		NRFX_SPIM_DEFAULT_CONFIG(SCK_PIN, MOSI_PIN, MOSI_PIN, CS_PIN);

	spim_config.dcx_pin = DCX_PIN;
	spim_config.frequency = 8000000;
	spim_config.mode = NRF_SPIM_MODE_0;
	spim_config.use_hw_ss = true;
	char p_context[20] = "initial";

	NRFX_DELAY_US(50000);
	status = nrfx_spim_init(&data->spim_inst, &spim_config, spim_handler, (void *)p_context);

	err = gpio_pin_configure_dt(&config->busy_gpio, GPIO_INPUT);
	if (err < 0) {
		LOG_ERR("Failed to configure busy GPIO");
		return err;
	}

	if (config->width > config->quirks->max_width ||
	    config->height > config->quirks->max_height) {
		LOG_ERR("Display size out of range.");
		return -EINVAL;
	}

	return ed070ec1_controller_init(dev);
}

static struct display_driver_api ed070ec1_driver_api = {
	.blanking_on = ed070ec1_blanking_on,
	.blanking_off = ed070ec1_blanking_off,
	.write = ed070ec1_write,
	.read = ed070ec1_read,
	.get_framebuffer = ed070ec1_get_framebuffer,
	.set_brightness = ed070ec1_set_brightness,
	.set_contrast = ed070ec1_set_contrast,
	.get_capabilities = ed070ec1_get_capabilities,
	.set_pixel_format = ed070ec1_set_pixel_format,
	.set_orientation = ed070ec1_set_orientation,
};

#if DT_HAS_COMPAT_STATUS_OKAY(solomon_ed070ec1)
static struct ed070ec1_quirks quirks_solomon_ed070ec1 = {
	.max_width = 960,
	.max_height = 640,
	.pp_width_bits = 16,
	.pp_height_bits = 16,
	.ctrl2_full = ED070EC1_GEN2_CTRL2_DISPLAY,
	.ctrl2_partial = ED070EC1_GEN2_CTRL2_DISPLAY | ED070EC1_GEN2_CTRL2_MODE2,
};
#endif

#define SOFTSTART_ASSIGN(n)                                                                        \
	.softstart = {                                                                             \
		.data = softstart_##n,                                                             \
		.len = sizeof(softstart_##n),                                                      \
	},

#define ED070EC1_MAKE_ARRAY_OPT(n, p) static uint8_t data_##n##_##p[] = DT_PROP_OR(n, p, {})

#define ED070EC1_ASSIGN_ARRAY(n, p)                                                                \
	{                                                                                          \
		.data = data_##n##_##p,                                                            \
		.len = sizeof(data_##n##_##p),                                                     \
	}

#define ED070EC1_PROFILE(n)                                                                        \
	ED070EC1_MAKE_ARRAY_OPT(n, lut);                                                           \
	ED070EC1_MAKE_ARRAY_OPT(n, gdv);                                                           \
	ED070EC1_MAKE_ARRAY_OPT(n, sdv);                                                           \
                                                                                                   \
	static const struct ed070ec1_profile ed070ec1_profile_##n = {                              \
		.lut = ED070EC1_ASSIGN_ARRAY(n, lut),                                              \
		.gdv = ED070EC1_ASSIGN_ARRAY(n, gdv),                                              \
		.sdv = ED070EC1_ASSIGN_ARRAY(n, sdv),                                              \
		.vcom = DT_PROP_OR(n, vcom, 0),                                                    \
		.override_vcom = DT_NODE_HAS_PROP(n, vcom),                                        \
		.bwf = DT_PROP_OR(n, border_waveform, 0),                                          \
		.override_bwf = DT_NODE_HAS_PROP(n, border_waveform),                              \
		.dummy_line = DT_PROP_OR(n, dummy_line, 0),                                        \
		.override_dummy_line = DT_NODE_HAS_PROP(n, dummy_line),                            \
		.gate_line_width = DT_PROP_OR(n, gate_line_width, 0),                              \
		.override_gate_line_width = DT_NODE_HAS_PROP(n, gate_line_width),                  \
	};

#define _ED070EC1_PROFILE_PTR(n) &ed070ec1_profile_##n

#define ED070EC1_PROFILE_PTR(n) COND_CODE_1(DT_NODE_EXISTS(n), (_ED070EC1_PROFILE_PTR(n)), NULL)

#define ED070EC1_DEFINE(n, quirks_ptr)                                                             \
	ED070EC1_MAKE_ARRAY_OPT(n, softstart);                                                     \
                                                                                                   \
	DT_FOREACH_CHILD(n, ED070EC1_PROFILE);                                                     \
                                                                                                   \
	static const struct ed070ec1_config ed070ec1_cfg_##n = {                                   \
		.bus = SPI_DT_SPEC_GET(                                                            \
			n, SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_HOLD_ON_CS | SPI_LOCK_ON,    \
			0),                                                                        \
		.reset_gpio = GPIO_DT_SPEC_GET(n, reset_gpios),                                    \
		.dc_gpio = GPIO_DT_SPEC_GET(n, dc_gpios),                                          \
		.busy_gpio = GPIO_DT_SPEC_GET(n, busy_gpios),                                      \
		.quirks = quirks_ptr,                                                              \
		.height = DT_PROP(n, height),                                                      \
		.width = DT_PROP(n, width),                                                        \
		.orientation = DT_PROP(n, orientation_flipped),                                    \
		.tssv = DT_PROP_OR(n, tssv, 0),                                                    \
		.softstart = ED070EC1_ASSIGN_ARRAY(n, softstart),                                  \
		.profiles =                                                                        \
			{                                                                          \
				[ED070EC1_PROFILE_FULL] = ED070EC1_PROFILE_PTR(DT_CHILD(n, full)), \
				[ED070EC1_PROFILE_PARTIAL] =                                       \
					ED070EC1_PROFILE_PTR(DT_CHILD(n, partial)),                \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
	static struct ed070ec1_data ed070ec1_data_##n;                                             \
                                                                                                   \
	DEVICE_DT_DEFINE(n, ed070ec1_init, NULL, &ed070ec1_data_##n, &ed070ec1_cfg_##n,            \
			 POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &ed070ec1_driver_api)

DT_FOREACH_STATUS_OKAY_VARGS(solomon_ed070ec1, ED070EC1_DEFINE, &quirks_solomon_ed070ec1);
