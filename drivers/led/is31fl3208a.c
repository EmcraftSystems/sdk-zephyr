/*
 * Copyright (c) 2025 Emcraft Systems
 */

#define DT_DRV_COMPAT issi_is31fl3208a

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#define IS31FL3208A_REG_SHUTDOWN	0x00
#define IS31FL3208A_REG_PWM_FIRST	0x01
#define IS31FL3208A_REG_PWM_LAST	0x12
#define IS31FL3208A_REG_UPDATE		0x13
#define IS31FL3208A_REG_LC_FIRST	0x14
#define IS31FL3208A_REG_LC_LAST		0x25
#define IS31FL3208A_REG_GLOBAL_CONTROL	0x26
#define IS31FL3208A_REG_FREQ		0x27
#define IS31FL3208A_REG_RESET		0x2F

#define IS31FL3208A_MAX_LEDS		18

LOG_MODULE_REGISTER(is31fl3208a, CONFIG_LED_LOG_LEVEL);

struct is31fl3208a_cfg {
	struct i2c_dt_spec i2c;
	uint8_t num_leds;
	uint8_t max_output_current;
	const struct led_info *leds_info;
};

static const struct led_info *is31fl3208a_led_to_info(
			const struct is31fl3208a_cfg *config, uint32_t led)
{
	if (led < config->num_leds) {
		return &config->leds_info[led];
	}

	return NULL;
}

static int is31fl3208a_get_info(const struct device *dev, uint32_t led,
			   const struct led_info **info)
{
	const struct is31fl3208a_cfg *config = dev->config;
	const struct led_info *led_info = is31fl3208a_led_to_info(config, led);

	if (!led_info) {
		return -EINVAL;
	}

	*info = led_info;

	return 0;
}

static int is31fl3208a_write_buffer(const struct i2c_dt_spec *i2c,
				    const uint8_t *buffer, uint32_t num_bytes)
{
	int status;

	status = i2c_write_dt(i2c, buffer, num_bytes);
	if (status < 0) {
		LOG_ERR("Could not write buffer: %i", status);
		return status;
	}

	return 0;
}

static int is31fl3208a_write_reg(const struct i2c_dt_spec *i2c, uint8_t reg,
				 uint8_t val)
{
	uint8_t buffer[2] = {reg, val};

	return is31fl3208a_write_buffer(i2c, buffer, sizeof(buffer));
}

static int is31fl3208a_update_pwm(const struct i2c_dt_spec *i2c)
{
	return is31fl3208a_write_reg(i2c, IS31FL3208A_REG_UPDATE, 0);
}

static uint8_t is31fl3208a_brightness_to_pwm(uint8_t brightness)
{
	return (0xFFU * brightness) / 100;
}

static int is31fl3208a_led_write_channels(const struct device *dev,
					  uint32_t start_channel,
					  uint32_t num_channels,
					  const uint8_t *buf)
{
	const struct is31fl3208a_cfg *config = dev->config;
	uint8_t i2c_buffer[IS31FL3208A_MAX_LEDS + 1];
	int status;
	int i;

	if (num_channels == 0) {
		return 0;
	}

	if ((start_channel + num_channels) > (IS31FL3208A_MAX_LEDS - 1)) {
		return -EINVAL;
	}

	i2c_buffer[0] = IS31FL3208A_REG_PWM_FIRST + start_channel;
	for (i = 0; i < num_channels; i++) {
		if (buf[i] > 100) {
			return -EINVAL;
		}
		i2c_buffer[i + 1] = is31fl3208a_brightness_to_pwm(buf[i]);
	}

	status = is31fl3208a_write_buffer(&config->i2c, i2c_buffer,
					  num_channels + 1);
	if (status < 0) {
		return status;
	}

	return is31fl3208a_update_pwm(&config->i2c);
}

static int is31fl3208a_led_set_brightness(const struct device *dev,
					  uint32_t led, uint8_t value)
{
	const struct is31fl3208a_cfg *config = dev->config;
	const struct led_info *led_info = is31fl3208a_led_to_info(config, led);
	int status = 0;
	uint8_t pwm_value;
	uint8_t pwm_reg = IS31FL3208A_REG_PWM_FIRST;

	if (!led_info) {
		return -ENODEV;
	}

	if (value > 100) {
		return -EINVAL;
	}

	pwm_reg = IS31FL3208A_REG_PWM_FIRST + led_info->index;
	pwm_value = is31fl3208a_brightness_to_pwm(value);
	status = is31fl3208a_write_reg(&config->i2c, pwm_reg, pwm_value);
	if (status < 0) {
		return status;
	}

	return is31fl3208a_update_pwm(&config->i2c);
}

static int is31fl3208a_led_on(const struct device *dev, uint32_t led)
{
	return is31fl3208a_led_set_brightness(dev, led, 100);
}

static int is31fl3208a_led_off(const struct device *dev, uint32_t led)
{
	return is31fl3208a_led_set_brightness(dev, led, 0);
}

static int is31fl3208a_init_registers(const struct device *dev)
{
	int i;
	int status;
	const struct is31fl3208a_cfg *config = dev->config;

	status = is31fl3208a_write_reg(&(config->i2c), IS31FL3208A_REG_RESET, 0);
	if (status < 0) {
		return status;
	}

	status = is31fl3208a_write_reg(&(config->i2c), IS31FL3208A_REG_SHUTDOWN, 1);
	if (status < 0) {
		return status;
	}

	for (i = IS31FL3208A_REG_LC_FIRST;
	     i <= IS31FL3208A_REG_LC_LAST;
	     i++) {
		status = is31fl3208a_write_reg(&(config->i2c), i, config->max_output_current);
		if (status < 0) {
			return status;
		}
	}


	status = is31fl3208a_write_reg(&(config->i2c), IS31FL3208A_REG_UPDATE, 0);
	if (status < 0) {
		return status;
	}

	return status;
}

static int is31fl3208a_init(const struct device *dev)
{
	const struct is31fl3208a_cfg *config = dev->config;

	LOG_DBG("Initializing @0x%x...", config->i2c.addr);

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C device not ready");
		return -ENODEV;
	}

	return is31fl3208a_init_registers(dev);
}

static const struct led_driver_api is31fl3208a_led_api = {
	.set_brightness = is31fl3208a_led_set_brightness,
	.on = is31fl3208a_led_on,
	.off = is31fl3208a_led_off,
	.get_info = is31fl3208a_get_info,
	.write_channels = is31fl3208a_led_write_channels
};

#define COLOR_MAPPING(led_node_id)						\
	const uint8_t color_mapping_##led_node_id[] =				\
		DT_PROP(led_node_id, color_mapping);

#define LED_INFO(led_node_id)							\
	{									\
		.label		= DT_PROP(led_node_id, label),			\
		.index		= DT_PROP(led_node_id, index),			\
		.num_colors	=						\
			DT_PROP_LEN(led_node_id, color_mapping),		\
		.color_mapping	= color_mapping_##led_node_id,			\
	},

#define IS31FL3208A_INIT(id) \
	DT_INST_FOREACH_CHILD(id, COLOR_MAPPING)				\
										\
	static const struct led_info is31fl3208a_leds_##id[] = {		\
		DT_INST_FOREACH_CHILD(id, LED_INFO)				\
	};									\
	static const struct is31fl3208a_cfg is31fl3208a_##id##_cfg = {		\
		.i2c = I2C_DT_SPEC_INST_GET(id),				\
		.num_leds = ARRAY_SIZE(is31fl3208a_leds_##id),			\
		.leds_info = is31fl3208a_leds_##id,				\
		.max_output_current = DT_INST_PROP(id, max_output_current),	\
	};									\
	DEVICE_DT_INST_DEFINE(id, &is31fl3208a_init, NULL, NULL,		\
		&is31fl3208a_##id##_cfg, POST_KERNEL,				\
		CONFIG_LED_INIT_PRIORITY, &is31fl3208a_led_api);

DT_INST_FOREACH_STATUS_OKAY(IS31FL3208A_INIT)
