/*
 * Copyright (c) 2025 Emcraft Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BQ25895 Datasheet: https://www.ti.com/lit/gpn/bq25895
 */

#define DT_DRV_COMPAT ti_bq25895

#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bq25895, CONFIG_CHARGER_LOG_LEVEL);

/* registers. Note: datasheet uses plain numbers, names are best guesses */
#define BQ25895_ILIM_CTL  0x00
#define BQ25895_VINDPM_OS 0x01
#define BQ25895_CHG_CTL   0x02
#define BQ25895_VSYS_CTL  0x03
#define BQ25895_ICHG_CTL  0x04
#define BQ25895_ITERM_CTL 0x05
#define BQ25895_VREG_CTL  0x06
#define BQ25895_TMR_CTL   0x07
#define BQ25895_IRCOMP    0x08
#define BQ25895_BATFET    0x09
#define BQ25895_BOOSTV    0x0A
#define BQ25895_STAT      0x0B
#define BQ25895_ERR       0x0C
#define BQ25895_VINDPM    0x0D
#define BQ25895_BATV      0x0E
#define BQ25895_SYSV      0x0F
#define BQ25895_TSPCT     0x10
#define BQ25895_VBUSV     0x11
#define BQ25895_ICHGR     0x12
#define BQ25895_IDPM      0x13
#define BQ25895_CTL       0x14

/* Register bits */
#define BQ25895_ILIM_CTL_EN_HIZ (1 << 7)

#define BQ25895_CHG_CTL_CONV_RATE (1 << 6)

#define BQ25895_VSYS_CTL_CHG_CONFIG (1 << 4)

#define BQ25895_TMR_CTL_WATCHDOG_DISABLE (0 << 4)
#define BQ25895_TMR_CTL_WATCHDOG_40      (1 << 4)
#define BQ25895_TMR_CTL_WATCHDOG_80      (2 << 4)
#define BQ25895_TMR_CTL_WATCHDOG_160     (3 << 4)
#define BQ25895_TMR_CTL_WATCHDOG_MASK    (3 << 4)

#define BQ25895_STAT_CHRG_STAT_NOT_CHARGING (0 << 3)
#define BQ25895_STAT_CHRG_STAT_PRE_CHARGE   (1 << 3)
#define BQ25895_STAT_CHRG_STAT_FAST_CHARGE  (2 << 3)
#define BQ25895_STAT_CHRG_STAT_DONE         (3 << 3)
#define BQ25895_STAT_CHRG_STAT_MASK         (3 << 3)

#define BQ25895_ERR_WATCHDOG_FAULT      (1 << 7)
#define BQ25895_ERR_BOOST_FAULT         (1 << 6)
#define BQ25895_ERR_CHRG_FAULT_MASK     (3 << 4)
#define BQ25895_ERR_CHRG_FAULT_NORMAL   (0 << 4)
#define BQ25895_ERR_CHRG_FAULT_INPUT    (1 << 4)
#define BQ25895_ERR_CHRG_FAULT_TERMAL   (2 << 4)
#define BQ25895_ERR_CHRG_FAULT_TIMER    (3 << 4)
#define BQ25895_ERR_BAT_FAULT           (1 << 3)
#define BQ25895_ERR_NTC_FAULT_MASK      (7 << 0)
#define BQ25895_ERR_NTC_FAULT_NORMAL    (0 << 0)
#define BQ25895_ERR_NTC_FAULT_BUCKCOLD  (1 << 0)
#define BQ25895_ERR_NTC_FAULT_BUCKHOT   (2 << 0)
#define BQ25895_ERR_NTC_FAULT_BOOSTCOLD (5 << 0)
#define BQ25895_ERR_NTC_FAULT_BOOSTHOT  (6 << 0)

#define BQ25895_VINDPM_FORCE_VINDPM (1 << 7)

#define BQ25895_BATV_MASK (0x3F << 0)

#define BQ25895_VBUSV_VBUS_GD (1 << 7)

#define BQ25895_CTL_PN_MSK     (7 << 3)
#define BQ25895_CTL_PN_BQ25895 (7 << 3)

struct bq25895_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec ce_gpio;
	struct gpio_dt_spec int_gpio;
	int inlim;
	int vindpm_os;
	int vsysmin;
	int ichg;
	int iprechg;
	int iterm;
	int vreg;
	int bat_comp;
	int vclamp;
	int treg;
	int boostv;
	int vindpm;
};

struct bq25895_data {
	const struct device *dev;
	struct gpio_callback gpio_cb;
	struct k_work_delayable int_routine_work;
	enum charger_status charger_status;
	enum charger_online charger_online;
	charger_status_notifier_t charger_status_notifier;
	charger_online_notifier_t charger_online_notifier;
};

static int bq25895_charge_enable(const struct device *dev, const bool enable)
{
	const struct bq25895_config *cfg = dev->config;
	uint8_t value = enable ? 0 : BQ25895_VSYS_CTL_CHG_CONFIG;
	int ret;

	ret = i2c_reg_update_byte_dt(&cfg->i2c, BQ25895_VSYS_CTL, BQ25895_VSYS_CTL_CHG_CONFIG,
				     value);
	if (ret < 0) {
		return ret;
	}
	if (gpio_is_ready_dt(&cfg->ce_gpio)) {
		ret = gpio_pin_set_dt(&cfg->ce_gpio, enable);
	}

	return ret;
}

static int bq25895_get_online(const struct device *dev, enum charger_online *online)
{
	const struct bq25895_config *cfg = dev->config;
	uint8_t val;
	int ret;

	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_VBUSV, &val);
	if (ret < 0) {
		return ret;
	}
	if ((val & BQ25895_VBUSV_VBUS_GD) != 0x00) {
		*online = CHARGER_ONLINE_FIXED;
	} else {
		*online = CHARGER_ONLINE_OFFLINE;
	}
	/* CHARGER_ONLINE_PROGRAMMABLE: Read REG0B (BQ25895_STAT)
	 * and return that value for USB?
	 */
	return 0;
}

static int bq25895_poll_online(const struct device *dev, enum charger_online *online)
{
	struct bq25895_data *data = dev->data;
	int ret;

	ret = bq25895_get_online(dev, online);
	if (ret == 0) {
		if (data->charger_online != *online) {
			data->charger_online = *online;
			if (data->charger_online_notifier != NULL) {
				data->charger_online_notifier(*online);
			}
		}
	}
	return ret;
}

static int bq25895_get_status(const struct device *dev, enum charger_status *status)
{
	const struct bq25895_config *cfg = dev->config;
	uint8_t stat;
	uint8_t val;
	int ret;

	/* Have Vin? */
	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_VBUSV, &val);
	if (ret < 0) {
		return ret;
	}
	if ((val & BQ25895_VBUSV_VBUS_GD) == 0x00) {
		*status = CHARGER_STATUS_DISCHARGING;
		return 0;
	}
	/* HIZ mode? */
	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_ILIM_CTL, &val);
	if (ret < 0) {
		return ret;
	}
	if ((val & BQ25895_ILIM_CTL_EN_HIZ) != 0x00) {
		*status = CHARGER_STATUS_DISCHARGING;
		return 0;
	}

	/* Charging disabled? */
	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_CHG_CTL, &val);
	if (ret < 0) {
		return ret;
	}
	if ((val & BQ25895_VSYS_CTL_CHG_CONFIG) == 0x00) {
		*status = CHARGER_STATUS_NOT_CHARGING;
		return 0;
	}

	/* Charger status */
	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_STAT, &stat);
	if (ret < 0) {
		return ret;
	}
	stat &= BQ25895_STAT_CHRG_STAT_MASK;

	switch (stat) {
	case BQ25895_STAT_CHRG_STAT_NOT_CHARGING:
		*status = CHARGER_STATUS_NOT_CHARGING;
		break;
	case BQ25895_STAT_CHRG_STAT_PRE_CHARGE:
	case BQ25895_STAT_CHRG_STAT_FAST_CHARGE:
		*status = CHARGER_STATUS_CHARGING;
		break;
	case BQ25895_STAT_CHRG_STAT_DONE:
		*status = CHARGER_STATUS_FULL;
		break;
	}

	return 0;
}

static int bq25895_poll_status(const struct device *dev, enum charger_status *status)
{
	struct bq25895_data *data = dev->data;
	int ret;

	ret = bq25895_get_status(dev, status);
	if (ret == 0) {
		if (data->charger_status != *status) {
			data->charger_status = *status;
			if (data->charger_status_notifier != NULL) {
				data->charger_status_notifier(*status);
			}
		}
	}
	return ret;
}

static int bq25895_get_present(const struct device *dev, bool *status)
{
	const struct bq25895_config *cfg = dev->config;
	uint8_t val;
	int ret;

	/* poll battery voltage */
	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_BATV, &val);
	if (ret < 0) {
		return ret;
	}
	val &= BQ25895_BATV_MASK;

	/* 2.35V threshold: offset 2.304V, need 46mV => 000 0010 */
	*status = val > 2;
	return 0;
}

static int bq25895_get_type(const struct device *dev, enum charger_charge_type *type)
{
	const struct bq25895_config *cfg = dev->config;
	uint8_t val;
	int ret;

	*type = CHARGER_CHARGE_TYPE_UNKNOWN;
	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_STAT, &val);
	if (ret < 0) {
		return ret;
	}
	switch (val & BQ25895_STAT_CHRG_STAT_MASK) {
	case BQ25895_STAT_CHRG_STAT_NOT_CHARGING:
		*type = CHARGER_CHARGE_TYPE_BYPASS;
		break;
	case BQ25895_STAT_CHRG_STAT_DONE:
		*type = CHARGER_CHARGE_TYPE_NONE;
		break;
	case BQ25895_STAT_CHRG_STAT_FAST_CHARGE:
		/* This is adaptive close to full capacity, but
		 * detecting that will require additional reads
		 */
		*type = CHARGER_CHARGE_TYPE_FAST;
		break;
	case BQ25895_STAT_CHRG_STAT_PRE_CHARGE:
		/* Treat pre-charge as trickle charge:
		 * constant low current mode
		 */
		*type = CHARGER_CHARGE_TYPE_TRICKLE;
		break;
	}
	return 0;
}

static int bq25895_get_health(const struct device *dev, enum charger_health *health)
{
	const struct bq25895_config *cfg = dev->config;
	bool bp;
	uint8_t val, chg, ntc;
	int ret;

	ret = bq25895_get_present(dev, &bp);
	if (ret < 0) {
		return ret;
	}

	/* read twice to get current state */
	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_ERR, &val);
	if (ret < 0) {
		return ret;
	}
	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_ERR, &val);
	if (ret < 0) {
		return ret;
	}

	*health = CHARGER_HEALTH_UNKNOWN;
	chg = val & BQ25895_ERR_CHRG_FAULT_MASK;
	/* Do not care about boost/buck, only cold/hot */
	ntc = val & BQ25895_ERR_NTC_FAULT_MASK & 0x3;
	if (!bp) {
		*health = CHARGER_HEALTH_NO_BATTERY;
	} else if (val & BQ25895_ERR_BAT_FAULT) {
		*health = CHARGER_HEALTH_OVERVOLTAGE;
	} else if (chg == BQ25895_ERR_CHRG_FAULT_TIMER) {
		*health = CHARGER_HEALTH_SAFETY_TIMER_EXPIRE;
	} else if (val & BQ25895_ERR_WATCHDOG_FAULT) {
		*health = CHARGER_HEALTH_WATCHDOG_TIMER_EXPIRE;
	} else if ((chg == BQ25895_ERR_CHRG_FAULT_INPUT) || (val & BQ25895_ERR_BOOST_FAULT)) {
		*health = CHARGER_HEALTH_UNSPEC_FAILURE;
	} else if (chg == BQ25895_ERR_CHRG_FAULT_TERMAL) {
		*health = CHARGER_HEALTH_OVERHEAT;
	} else if (ntc == BQ25895_ERR_NTC_FAULT_BUCKCOLD) {
		*health = CHARGER_HEALTH_COLD;
	} else if (ntc == BQ25895_ERR_NTC_FAULT_BUCKHOT) {
		*health = CHARGER_HEALTH_HOT;
	} else if (val == 0) {
		*health = CHARGER_HEALTH_GOOD;
	}
	return 0;
}

static int bq25895_get_prop(const struct device *dev, charger_prop_t prop,
			    union charger_propval *val)
{
	switch (prop) {
		/* Not implemented for now:
		 *   CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA,
		 *   CHARGER_PROP_PRECHARGE_CURRENT_UA,
		 *   CHARGER_PROP_TERM_CURRENT_UA,
		 *   CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV
		 */
	case CHARGER_PROP_ONLINE:
		return bq25895_poll_online(dev, &val->online);
	case CHARGER_PROP_PRESENT:
		return bq25895_get_present(dev, &val->present);
	case CHARGER_PROP_STATUS:
		return bq25895_poll_status(dev, &val->status);
	case CHARGER_PROP_CHARGE_TYPE:
		return bq25895_get_type(dev, &val->charge_type);
	case CHARGER_PROP_HEALTH:
		return bq25895_get_health(dev, &val->health);
	default:
		return -ENOTSUP;
	}
}

static int bq25895_set_prop(const struct device *dev, charger_prop_t prop,
			    const union charger_propval *val)
{
	struct bq25895_data *data = dev->data;

	switch (prop) {
		/* Not implemented for now:
		 *   CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA,
		 *   CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV
		 */
	case CHARGER_PROP_STATUS_NOTIFICATION:
		data->charger_status_notifier = val->status_notification;
		return 0;

	case CHARGER_PROP_ONLINE_NOTIFICATION:
		data->charger_online_notifier = val->online_notification;
		return 0;

	default:
		return -ENOTSUP;
	}
}

static const struct charger_driver_api bq25895_api = {
	.get_property = bq25895_get_prop,
	.set_property = bq25895_set_prop,
	.charge_enable = bq25895_charge_enable,
};

static uint8_t encodeval(int val, int offset, int digit_val, uint8_t size, uint8_t shift)
{
	int dv;
	uint8_t enc;

	val -= offset;
	enc = 0;
	for (dv = digit_val << (size - 1); dv >= digit_val; dv >>= 1) {
		enc <<= 1;
		if (val >= dv) {
			val -= dv;
			enc |= 1;
		}
	}
	return enc << shift;
}

static void bq25895_gpio_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	struct bq25895_data *data = CONTAINER_OF(cb, struct bq25895_data, gpio_cb);
	int ret;

	ret = k_work_reschedule(&data->int_routine_work, K_NO_WAIT);
	if (ret < 0) {
		LOG_WRN("Could not submit int work: %d", ret);
	}
}

static int bq25895_check_faults(const struct device *dev)
{
	const struct bq25895_config *cfg = dev->config;
	uint8_t val;
	int ret;

	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_ERR, &val);
	if (ret < 0) {
		return ret;
	}
	if (val) {
		LOG_WRN("Fault: %02X\n", val);
	}
	return 0;
}

#define BQ25895_POLL_DELAY K_SECONDS(1)

static void bq25895_int_routine_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct bq25895_data *data = CONTAINER_OF(dwork, struct bq25895_data, int_routine_work);
	int ret;
	enum charger_status charger_status;
	enum charger_online charger_online;

	/* Handle latched fault state */
	ret = bq25895_check_faults(data->dev);
	if (ret == 0) {
		/* Poll 2nd time to get current fault state */
		ret = bq25895_check_faults(data->dev);
	}

	/* Get status/online and notify */
	ret = bq25895_poll_status(data->dev, &charger_status);
	ret = bq25895_poll_online(data->dev, &charger_online);

	ret = k_work_reschedule(&data->int_routine_work, BQ25895_POLL_DELAY);
	if (ret < 0) {
		LOG_WRN("Could not submit poll work: %d", ret);
	}
}

static int bq25895_configure_interrupt_pin(const struct device *dev)
{
	struct bq25895_data *data = dev->data;
	const struct bq25895_config *cfg = dev->config;
	int ret;

	if (!gpio_is_ready_dt(&cfg->int_gpio)) {
		LOG_ERR("Interrupt GPIO device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Could not configure interrupt GPIO");
		return ret;
	}

	gpio_init_callback(&data->gpio_cb, bq25895_gpio_callback, BIT(cfg->int_gpio.pin));
	ret = gpio_add_callback_dt(&cfg->int_gpio, &data->gpio_cb);
	if (ret < 0) {
		LOG_ERR("Could not add interrupt GPIO callback");
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Could not configure interrupt");
		return ret;
	}

	return 0;
}

static int do_set_param_w_en(const struct bq25895_config *cfg, int field, int min, int max,
			     unsigned int offset, unsigned int digit_val, unsigned int size,
			     unsigned int shift, unsigned int reg, unsigned int en_m,
			     unsigned int en_val)
{
	uint8_t val;
	int ret;

	if (IN_RANGE(field, min, max)) {
		val = encodeval(field, offset, digit_val, size, shift);
		val &= ~en_m;
		val |= en_val;
		ret = i2c_reg_update_byte_dt(&cfg->i2c, reg, ((1 << size) - 1) << shift, val);
		if (ret < 0) {
			return ret;
		}
	}
	return 0;
}

#define set_param_w_en(name, min, max, offset, digit_val, size, shift, reg, en_m, en_val)          \
	do_set_param_w_en(cfg, cfg->name, min, max, offset, digit_val, size, shift, reg, en_m,     \
			  en_val)

#define set_parameter(name, min, max, offset, digit_val, size, shift, reg)                         \
	set_param_w_en(name, min, max, offset, digit_val, size, shift, reg, 0, 0)

static int bq25895_init(const struct device *dev)
{
	const struct bq25895_config *cfg = dev->config;
	struct bq25895_data *data = dev->data;
	uint8_t val;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C device not ready");
		return -ENODEV;
	}

	data->dev = dev;

	ret = i2c_reg_read_byte_dt(&cfg->i2c, BQ25895_CTL, &val);
	if (ret < 0) {
		LOG_ERR("Device ID read failed: %d", ret);
		return ret;
	}

	val &= BQ25895_CTL_PN_MSK;
	if (val != BQ25895_CTL_PN_BQ25895) {
		LOG_ERR("Invalid device id: %02x", val);
		return -EINVAL;
	}

	/* Disable the watchdog */
	ret = i2c_reg_update_byte_dt(&cfg->i2c, BQ25895_TMR_CTL, BQ25895_TMR_CTL_WATCHDOG_MASK,
				     BQ25895_TMR_CTL_WATCHDOG_DISABLE);
	if (ret < 0) {
		LOG_ERR("Stop WDT failed: %d", ret);
		return ret;
	}

	/* Start cyclic ADC, so we can "detect" battery */
	ret = i2c_reg_update_byte_dt(&cfg->i2c, BQ25895_CHG_CTL, BQ25895_CHG_CTL_CONV_RATE,
				     BQ25895_CHG_CTL_CONV_RATE);
	if (ret < 0) {
		LOG_ERR("Start cyclic ADC failed: %d", ret);
		return ret;
	}

	ret = set_parameter(inlim, 100, 32500, 100, 50, 6, 0, BQ25895_ILIM_CTL);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(vindpm_os, 0, 3100, 0, 100, 5, 0, BQ25895_VINDPM_OS);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(vsysmin, 3000, 3700, 3000, 100, 3, 1, BQ25895_VSYS_CTL);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(ichg, 0, 5056000, 0, 64000, 7, 0, BQ25895_ICHG_CTL);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(iprechg, 64000, 1024000, 64000, 64000, 4, 4, BQ25895_ITERM_CTL);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(iterm, 64000, 1024000, 64000, 64000, 4, 0, BQ25895_ITERM_CTL);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(vreg, 3840000, 4608000, 3840000, 16000, 6, 2, BQ25895_VREG_CTL);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(bat_comp, 0, 140, 0, 20, 3, 5, BQ25895_IRCOMP);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(vclamp, 0, 224, 0, 32, 3, 2, BQ25895_IRCOMP);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(treg, 60, 120, 60, 20, 2, 0, BQ25895_IRCOMP);
	if (ret < 0) {
		return ret;
	}
	ret = set_parameter(boostv, 4550, 5510, 4550, 64, 4, 4, BQ25895_BOOSTV);
	if (ret < 0) {
		return ret;
	}
	ret = set_param_w_en(vindpm, 3900, 15300, 2600, 100, 7, 0, BQ25895_VINDPM,
			     BQ25895_VINDPM_FORCE_VINDPM, BQ25895_VINDPM_FORCE_VINDPM);
	if (ret < 0) {
		return ret;
	}

	k_work_init_delayable(&data->int_routine_work, bq25895_int_routine_work_handler);

	ret = bq25895_configure_interrupt_pin(dev);
	if (ret < 0) {
		LOG_ERR("Could not set interrupt GPIO callback: %d", ret);
		return ret;
	}

	return 0;
}

#define LOCAL_INST_PROP_INIT(inst, prop, propname) .prop = DT_INST_PROP_OR(inst, propname, -1)

#define CHARGER_BQ25895_INIT(inst)                                                                 \
	static struct bq25895_data bq25895_data_##inst;                                            \
	static const struct bq25895_config bq25895_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.ce_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, ce_gpios, {0}),                          \
		.int_gpio = GPIO_DT_SPEC_INST_GET(inst, irq_gpios),                                \
		LOCAL_INST_PROP_INIT(inst, inlim, inlim),                                          \
		LOCAL_INST_PROP_INIT(inst, vindpm_os, vindpm_os),                                  \
		LOCAL_INST_PROP_INIT(inst, vsysmin, vsysmin),                                      \
		LOCAL_INST_PROP_INIT(inst, ichg, constant_charge_current_max_microamp),            \
		LOCAL_INST_PROP_INIT(inst, iprechg, precharge_current_microamp),                   \
		LOCAL_INST_PROP_INIT(inst, iterm, charge_term_current_microamp),                   \
		LOCAL_INST_PROP_INIT(inst, vreg, constant_charge_voltage_max_microvolt),           \
		LOCAL_INST_PROP_INIT(inst, bat_comp, batcomp),                                     \
		LOCAL_INST_PROP_INIT(inst, vclamp, vclamp),                                        \
		LOCAL_INST_PROP_INIT(inst, treg, treg),                                            \
		LOCAL_INST_PROP_INIT(inst, boostv, boostv),                                        \
		LOCAL_INST_PROP_INIT(inst, vindpm, vindpm),                                        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, bq25895_init, NULL, &bq25895_data_##inst,                      \
			      &bq25895_config_##inst, POST_KERNEL, CONFIG_CHARGER_INIT_PRIORITY,   \
			      &bq25895_api);

DT_INST_FOREACH_STATUS_OKAY(CHARGER_BQ25895_INIT)
