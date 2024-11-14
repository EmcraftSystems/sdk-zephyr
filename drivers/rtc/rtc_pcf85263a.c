/*
 *
 * Copyright (c) 2024 Emcraft Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#define DT_DRV_COMPAT nxp_pcf85263a

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/dt-bindings/rtc/nxp_pcf85263a.h>

LOG_MODULE_REGISTER(pcf85263a, CONFIG_RTC_LOG_LEVEL);

/* PCF85263A register addresses */
#define PCF85263A_DATE_TIME             0x01U
#define PCF85263A_ALARM1                0x08U
#define PCF85263A_ALARM2                0x0DU
#define PCF85263A_ALARM_ENABLE          0x10U
#define PCF85263A_OFFSET                0x24U
#define PCF85263A_OSC                   0x25U
#define PCF85263A_BATTERY_SWITCH        0x26U
#define PCF85263A_PIN_IO                0x27U
#define PCF85263A_FUNC                  0x28U
#define PCF85263A_INTA_CONTROL          0x29U
#define PCF85263A_INTB_CONTROL          0x2AU
#define PCF85263A_FLAGS                 0x2BU
#define PCF85263A_STOP_ENABLE           0x2EU

/* Time and date/alarm register bits */
#define PCF85263A_SECONDS_OS            BIT(7)
#define PCF85263A_SECONDS_MASK          GENMASK(6, 0)
#define PCF85263A_MINUTES_MASK          GENMASK(6, 0)
#define PCF85263A_HOURS_AMPM            BIT(5)
#define PCF85263A_HOURS_12H_MASK        GENMASK(4, 0)
#define PCF85263A_HOURS_24H_MASK        GENMASK(5, 0)
#define PCF85263A_DAYS_MASK             GENMASK(5, 0)
#define PCF85263A_WEEKDAYS_MASK         GENMASK(2, 0)
#define PCF85263A_MONTHS_MASK           GENMASK(4, 0)
#define PCF85263A_YEARS_MASK            GENMASK(7, 0)

/* Oscillator control register bits */
#define PCF85263A_AE_WDAY_A2E           BIT(7)
#define PCF85263A_AE_HR_A2E             BIT(6)
#define PCF85263A_AE_MIN_A2E            BIT(5)
#define PCF85263A_AE_MON_A1E            BIT(4)
#define PCF85263A_AE_DAY_A1E            BIT(3)
#define PCF85263A_AE_HR_A1E             BIT(2)
#define PCF85263A_AE_MIN_A1E            BIT(1)
#define PCF85263A_AE_SEC_A1E            BIT(0)

/* PINIO register bits */
#define PCF85263A_PINIO_CLKPM           BIT(7)
#define PCF85263A_PINIO_TSPULL          BIT(6)
#define PCF85263A_PINIO_TSL             BIT(5)
#define PCF85263A_PINIO_TSIM            BIT(4)
#define PCF85263A_PINIO_TSPM            GENMASK(3, 2)
#define PCF85263A_PINIO_INTAPM          GENMASK(1, 0)

/* TSPM field */
#define PCF85263A_TSPM_INPUT            0x03U
#define PCF85263A_TSPM_CLK              0x02U
#define PCF85263A_TSPM_INTB             0x01U
#define PCF85263A_TSPM_DISABLE          0x00U

/* INTAPM field */
#define PCF85263A_INTAPM_DISABLE        0x03U
#define PCF85263A_INTAPM_INTA           0x02U
#define PCF85263A_INTAPM_BATT           0x01U
#define PCF85263A_INTAPM_CLK            0x00U

#define PCF85263A_FLAGS_PIF             BIT(7)
#define PCF85263A_FLAGS_A2F             BIT(6)
#define PCF85263A_FLAGS_A1F             BIT(5)
#define PCF85263A_FLAGS_WDF             BIT(4)
#define PCF85263A_FLAGS_BSF             BIT(3)
#define PCF85263A_FLAGS_T3F             BIT(2)
#define PCF85263A_FLAGS_T2F             BIT(1)
#define PCF85263A_FLAGS_T1F             BIT(0)

/* Stop enable register bits */
#define PCF85263A_STOP_ENABLE_STOP      BIT(0)

/* Alarm IDs for PCF85263A */
#define PCF85263A_RTC_ALARM1_ID         0
#define PCF85263A_RTC_ALARM2_ID         1

/* Number of bytes used by PCF85263A alarms */
#define PCF85263A_RTC_ALARM1_LEN        5
#define PCF85263A_RTC_ALARM2_LEN        3

/* RTC alarm time fields supported by the PCF85263A */
#define PCF85263A_RTC_ALARM1_TIME_MASK                             \
	(RTC_ALARM_TIME_MASK_SECOND | RTC_ALARM_TIME_MASK_MINUTE | \
	 RTC_ALARM_TIME_MASK_HOUR | RTC_ALARM_TIME_MASK_MONTHDAY      | \
	 RTC_ALARM_TIME_MASK_MONTH)

#define PCF85263A_RTC_ALARM2_TIME_MASK                           \
	(RTC_ALARM_TIME_MASK_MINUTE | RTC_ALARM_TIME_MASK_HOUR | \
	 RTC_ALARM_TIME_MASK_WEEKDAY)

/* The PCF85263A only supports two-digit years, calculate offset to use */
#define PCF85263A_YEARS_OFFSET (2000 - 1900)

/* The PCF85263A enumerates months 1 to 12, RTC API uses 0 to 11 */
#define PCF85263A_MONTHS_OFFSET 1

/* Helper macro to guard inta-gpios related code */
#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(inta_gpios) && \
	(defined(CONFIG_RTC_ALARM) || defined(CONFIG_RTC_UPDATE))
#define PCF85263A_INTA_GPIOS_IN_USE 1
#endif

struct pcf85263a_config {
	const struct i2c_dt_spec i2c;
#ifdef PCF85263A_INTA_GPIOS_IN_USE
	struct gpio_dt_spec inta;
#endif /* PCF85263A_INTA_GPIOS_IN_USE */
	uint8_t inta_mode;
};

struct pcf85263a_data {
	struct k_mutex lock;
#if PCF85263A_INTA_GPIOS_IN_USE
	struct gpio_callback inta_callback;
	struct k_thread inta_thread;
	struct k_sem inta_sem;

	K_KERNEL_STACK_MEMBER(inta_stack, CONFIG_RTC_PCF85263A_THREAD_STACK_SIZE);
#ifdef CONFIG_RTC_ALARM
	rtc_alarm_callback alarm_callback;
	void *alarm_user_data;
#endif /* CONFIG_RTC_ALARM */
#endif /* PCF85263A_INTA_GPIOS_IN_USE */
};

static int pcf85263a_read_regs(const struct device *dev, uint8_t addr, void *buf, size_t len)
{
	const struct pcf85263a_config *config = dev->config;
	int err;

	err = i2c_burst_read_dt(&config->i2c, addr, buf, len);
	if (err != 0) {
		LOG_ERR("failed to read reg addr 0x%02x, len %d (err %d)", addr, len, err);
		return err;
	}

	return 0;
}

static int pcf85263a_read_reg8(const struct device *dev, uint8_t addr, uint8_t *val)
{
	return pcf85263a_read_regs(dev, addr, val, sizeof(*val));
}

static int pcf85263a_write_regs(const struct device *dev, uint8_t addr, void *buf, size_t len)
{
	const struct pcf85263a_config *config = dev->config;
	int err;

	err = i2c_burst_write_dt(&config->i2c, addr, buf, len);
	if (err != 0) {
		LOG_ERR("failed to write reg addr 0x%02x, len %d (err %d)", addr, len, err);
		return err;
	}

	return 0;
}

static int pcf85263a_write_reg8(const struct device *dev, uint8_t addr, uint8_t val)
{
	return pcf85263a_write_regs(dev, addr, &val, sizeof(val));
}

#if PCF85263A_INTA_GPIOS_IN_USE
static void pcf85263a_inta_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const struct device *dev = p1;
	struct pcf85263a_data *data = dev->data;
	rtc_alarm_callback alarm_callback = NULL;
	void *alarm_user_data = NULL;
	uint8_t flags = 0;
	uint8_t alarm_mask = 0;
	int err;

	while (true) {
		alarm_mask = 0;
		k_sem_take(&data->inta_sem, K_FOREVER);
		k_mutex_lock(&data->lock, K_FOREVER);

		err = pcf85263a_read_reg8(dev, PCF85263A_FLAGS, &flags);
		if (err != 0) {
			goto unlock;
		}

#ifdef CONFIG_RTC_ALARM
		/* Clear alarm interrupt flags and assign a callback */
		if (((flags & PCF85263A_FLAGS_A1F) != 0) &&
		    (data->alarm_callback != NULL)) {
			alarm_callback = data->alarm_callback;
			alarm_user_data = data->alarm_user_data;
			if ((flags & (PCF85263A_FLAGS_A1F)) != 0) {
				flags &= ~(PCF85263A_FLAGS_A1F);
				alarm_mask |= PCF85263A_RTC_ALARM1_ID;
			}
		}
#endif /* CONFIG_RTC_ALARM */

		err = pcf85263a_write_reg8(dev, PCF85263A_FLAGS, flags);
		if (err != 0) {
			goto unlock;
		}

		/* Check if interrupt occurred between FLAGS register read/write */
		err = pcf85263a_read_reg8(dev, PCF85263A_FLAGS, &flags);
		if (err != 0) {
			goto unlock;
		}

		if (((flags & (PCF85263A_FLAGS_A1F)) != 0) &&
		    (alarm_callback != NULL)) {
			/* Another interrupt occurred while servicing this one */
			k_sem_give(&data->inta_sem);
		}

unlock:
		k_mutex_unlock(&data->lock);

		if (alarm_callback != NULL) {
			alarm_callback(dev, alarm_mask, alarm_user_data);
			alarm_callback = NULL;
		}
	}
}

static void pcf85263a_inta_callback_handler(const struct device *port, struct gpio_callback *cb,
					  gpio_port_pins_t pins)
{
	struct pcf85263a_data *data = CONTAINER_OF(cb, struct pcf85263a_data, inta_callback);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	k_sem_give(&data->inta_sem);
}
#endif /* PCF85263A_INTA_GPIOS_IN_USE */

static int pcf85263a_set_time(const struct device *dev, const struct rtc_time *timeptr)
{
	struct pcf85263a_data *data = dev->data;
	uint8_t regs[7];
	int err;

	if (timeptr->tm_year < PCF85263A_YEARS_OFFSET ||
	    timeptr->tm_year > PCF85263A_YEARS_OFFSET + 99) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	LOG_DBG("set time: year = %d, mon = %d, mday = %d, wday = %d, hour = %d, "
		"min = %d, sec = %d",
		timeptr->tm_year, timeptr->tm_mon, timeptr->tm_mday, timeptr->tm_wday,
		timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec);

	regs[0] = bin2bcd(timeptr->tm_sec) & PCF85263A_SECONDS_MASK;
	regs[1] = bin2bcd(timeptr->tm_min) & PCF85263A_MINUTES_MASK;
	regs[2] = bin2bcd(timeptr->tm_hour) & PCF85263A_HOURS_24H_MASK;
	regs[3] = bin2bcd(timeptr->tm_mday) & PCF85263A_DAYS_MASK;
	regs[4] = bin2bcd(timeptr->tm_wday) & PCF85263A_WEEKDAYS_MASK;
	regs[5] = bin2bcd(timeptr->tm_mon + PCF85263A_MONTHS_OFFSET) & PCF85263A_MONTHS_MASK;
	regs[6] = bin2bcd(timeptr->tm_year - PCF85263A_YEARS_OFFSET) & PCF85263A_YEARS_MASK;

	/* Write registers PCF85263A_SECONDS through PCF85263A_YEARS */
	err = pcf85263a_write_regs(dev, PCF85263A_DATE_TIME, &regs, sizeof(regs));
	if (err != 0) {
		goto unlock;
	}

unlock:
	k_mutex_unlock(&data->lock);

	return err;
}

static int pcf85263a_get_time(const struct device *dev, struct rtc_time *timeptr)
{
	uint8_t regs[7];
	int err;

	/* Read registers PCF85263A_SECONDS through PCF85263A_YEARS */
	err = pcf85263a_read_regs(dev, PCF85263A_DATE_TIME, &regs, sizeof(regs));
	if (err != 0) {
		return err;
	}

	if ((regs[0] & PCF85263A_SECONDS_OS) != 0) {
		LOG_WRN("oscillator stopped or interrupted");
		return -ENODATA;
	}

	memset(timeptr, 0U, sizeof(*timeptr));
	timeptr->tm_sec = bcd2bin(regs[0] & PCF85263A_SECONDS_MASK);
	timeptr->tm_min = bcd2bin(regs[1] & PCF85263A_MINUTES_MASK);
	timeptr->tm_hour = bcd2bin(regs[2] & PCF85263A_HOURS_24H_MASK);
	timeptr->tm_mday = bcd2bin(regs[3] & PCF85263A_DAYS_MASK);
	timeptr->tm_wday = bcd2bin(regs[4] & PCF85263A_WEEKDAYS_MASK);
	timeptr->tm_mon = bcd2bin(regs[5] & PCF85263A_MONTHS_MASK) - PCF85263A_MONTHS_OFFSET;
	timeptr->tm_year = bcd2bin(regs[6] & PCF85263A_YEARS_MASK) + PCF85263A_YEARS_OFFSET;
	timeptr->tm_yday = -1;
	timeptr->tm_isdst = -1;

	LOG_DBG("get time: year = %d, mon = %d, mday = %d, wday = %d, hour = %d, "
		"min = %d, sec = %d",
		timeptr->tm_year, timeptr->tm_mon, timeptr->tm_mday, timeptr->tm_wday,
		timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec);

	return 0;
}

#ifdef CONFIG_RTC_ALARM
static int pcf85263a_alarm_get_supported_fields(const struct device *dev, uint16_t id,
						uint16_t *mask)
{
	ARG_UNUSED(dev);

	if (id != PCF85263A_RTC_ALARM1_ID) {
		LOG_ERR("invalid ID %d", id);
		return -EINVAL;
	}

	*mask = PCF85263A_RTC_ALARM1_TIME_MASK;

	return 0;
}

static int pcf85263a_alarm_set_time(const struct device *dev, uint16_t id, uint16_t mask,
				  const struct rtc_time *timeptr)
{
	struct pcf85263a_data *data = dev->data;
	uint8_t reg_ae;
	uint8_t regs[5];
	uint8_t reg_len = 0;
	uint8_t alarm_offset;
	int err = 0;

	if (id != PCF85263A_RTC_ALARM1_ID) {
		LOG_ERR("invalid ID %d", id);
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	err = pcf85263a_read_regs(dev, PCF85263A_ALARM_ENABLE, &reg_ae, sizeof(reg_ae));
	if (err != 0) {
		goto unlock;
	}

	reg_len = PCF85263A_RTC_ALARM1_LEN;
	alarm_offset = PCF85263A_ALARM1;

	if ((mask & ~(PCF85263A_RTC_ALARM1_TIME_MASK)) != 0U) {
		LOG_ERR("unsupported alarm field mask 0x%04x for ID %d", mask, id);
		err = -EINVAL;
		goto unlock;
	}

	if ((mask & RTC_ALARM_TIME_MASK_SECOND) != 0U) {
		regs[0] = bin2bcd(timeptr->tm_sec) & PCF85263A_SECONDS_MASK;
		reg_ae |= PCF85263A_AE_SEC_A1E;
	} else {
		regs[0] = 0;
		reg_ae &= ~(PCF85263A_AE_SEC_A1E);
	}

	if ((mask & RTC_ALARM_TIME_MASK_MINUTE) != 0U) {
		regs[1] = bin2bcd(timeptr->tm_min) & PCF85263A_MINUTES_MASK;
		reg_ae |= PCF85263A_AE_MIN_A1E;
	} else {
		regs[1] = 0;
		reg_ae &= ~(PCF85263A_AE_MIN_A1E);
	}

	if ((mask & RTC_ALARM_TIME_MASK_HOUR) != 0U) {
		regs[2] = bin2bcd(timeptr->tm_hour) & PCF85263A_HOURS_24H_MASK;
		reg_ae |= PCF85263A_AE_HR_A1E;
	} else {
		regs[2] = 0;
		reg_ae &= ~(PCF85263A_AE_HR_A1E);
	}

	if ((mask & RTC_ALARM_TIME_MASK_MONTHDAY) != 0U) {
		regs[3] = bin2bcd(timeptr->tm_mday) & PCF85263A_DAYS_MASK;
		reg_ae |= PCF85263A_AE_DAY_A1E;
	} else {
		regs[3] = 0;
		reg_ae &= ~(PCF85263A_AE_DAY_A1E);
	}

	if ((mask & RTC_ALARM_TIME_MASK_MONTH) != 0U) {
		regs[4] = bin2bcd(timeptr->tm_mon) & PCF85263A_MONTHS_MASK;
		reg_ae |= PCF85263A_AE_MON_A1E;
	} else {
		regs[4] = 0;
		reg_ae &= ~(PCF85263A_AE_MON_A1E);
	}

	/* Write alarm registers */
	err = pcf85263a_write_regs(dev, alarm_offset, &regs, reg_len);
	if (err != 0) {
		goto unlock;
	}

	/* Write alarm enable register */
	err = pcf85263a_write_regs(dev, PCF85263A_ALARM_ENABLE, &reg_ae, sizeof(reg_ae));

unlock:
	k_mutex_unlock(&data->lock);
	return err;
}

static int pcf85263a_alarm_get_time(const struct device *dev, uint16_t id, uint16_t *mask,
				  struct rtc_time *timeptr)
{
	uint8_t reg_ae;
	uint8_t regs[5];
	int err = 0;

	if (id != PCF85263A_RTC_ALARM1_ID) {
		LOG_ERR("invalid ID %d", id);
		return -EINVAL;
	}

	err = pcf85263a_read_regs(dev, PCF85263A_ALARM_ENABLE, &reg_ae, sizeof(reg_ae));
	if (err != 0) {
		return err;
	}

	memset(timeptr, 0U, sizeof(*timeptr));
	*mask = 0U;

	err = pcf85263a_read_regs(dev, PCF85263A_ALARM1, &regs, PCF85263A_RTC_ALARM1_LEN);
	if (err != 0) {
		return err;
	}

	if ((reg_ae & PCF85263A_AE_SEC_A1E) != 0) {
		timeptr->tm_sec = bcd2bin(regs[0] & PCF85263A_SECONDS_MASK);
		*mask |= RTC_ALARM_TIME_MASK_SECOND;
	}

	if ((reg_ae & PCF85263A_AE_MIN_A1E) != 0) {
		timeptr->tm_min = bcd2bin(regs[1] & PCF85263A_MINUTES_MASK);
		*mask |= RTC_ALARM_TIME_MASK_MINUTE;
	}

	if ((reg_ae & PCF85263A_AE_HR_A1E) != 0) {
		timeptr->tm_hour = bcd2bin(regs[2] & PCF85263A_HOURS_24H_MASK);
		*mask |= RTC_ALARM_TIME_MASK_HOUR;
	}

	if ((reg_ae & PCF85263A_AE_DAY_A1E) != 0) {
		timeptr->tm_mday = bcd2bin(regs[3] & PCF85263A_DAYS_MASK);
		*mask |= RTC_ALARM_TIME_MASK_MONTHDAY;
	}

	if ((reg_ae & PCF85263A_AE_MON_A1E) != 0) {
		timeptr->tm_mon = bcd2bin(regs[4] & PCF85263A_MONTHS_MASK);
		*mask |= RTC_ALARM_TIME_MASK_MONTH;
	}

	return 0;
}

static int pcf85263a_alarm_is_pending(const struct device *dev, uint16_t id)
{
	struct pcf85263a_data *data = dev->data;
	uint8_t reg;
	uint8_t alarm_flag;
	int err;

	if (id > PCF85263A_RTC_ALARM2_ID) {
		LOG_ERR("invalid ID %d", id);
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	err = pcf85263a_read_reg8(dev, PCF85263A_FLAGS, &reg);
	if (err != 0) {
		goto unlock;
	}

	if (id == PCF85263A_RTC_ALARM1_ID) {
		alarm_flag = PCF85263A_FLAGS_A1F;
	} else {
		alarm_flag = PCF85263A_FLAGS_A2F;
	}

	if ((reg & alarm_flag) != 0) {
		/* Clear alarm flag */
		reg &= ~(alarm_flag);

		err = pcf85263a_write_reg8(dev, PCF85263A_FLAGS, reg);
		if (err != 0) {
			goto unlock;
		}

		/* Alarm pending */
		err = 1;
	}

unlock:
	k_mutex_unlock(&data->lock);

	return err;
}

#if PCF85263A_INTA_GPIOS_IN_USE
static int pcf85263a_alarm_set_callback(const struct device *dev, uint16_t id,
				      rtc_alarm_callback callback, void *user_data)
{
	const struct pcf85263a_config *config = dev->config;
	struct pcf85263a_data *data = dev->data;
	int err = 0;

	if (config->inta.port == NULL) {
		return -ENOTSUP;
	}

	if (id != PCF85263A_RTC_ALARM1_ID) {
		LOG_ERR("invalid ID %d", id);
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	data->alarm_callback = callback;
	data->alarm_user_data = user_data;

	k_mutex_unlock(&data->lock);

	/* Use edge interrupts to avoid multiple GPIO IRQs while servicing the IRQ in the thread */
	err = gpio_pin_interrupt_configure_dt(&config->inta,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (err != 0) {
		LOG_ERR("failed to enable GPIO IRQ (err %d)", err);
		goto exit;
	}

	/* Wake up the INTA thread since the alarm flag may already be set */
	k_sem_give(&data->inta_sem);

exit:
	return err;
}
#endif /* PCF85263A_INTA_GPIOS_IN_USE */
#endif /* CONFIG_RTC_ALARM */

static int pcf85263a_init(const struct device *dev)
{
	const struct pcf85263a_config *config = dev->config;
	struct pcf85263a_data *data = dev->data;
	uint8_t regs[8];
	int err;

	k_mutex_init(&data->lock);

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

#if PCF85263A_INTA_GPIOS_IN_USE
	k_tid_t tid;

	if (config->inta.port != NULL) {
		k_sem_init(&data->inta_sem, 0, INT_MAX);

		if (!gpio_is_ready_dt(&config->inta)) {
			LOG_ERR("GPIO not ready");
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&config->inta, GPIO_INPUT);
		if (err != 0) {
			LOG_ERR("failed to configure GPIO (err %d)", err);
			return -ENODEV;
		}

		gpio_init_callback(&data->inta_callback, pcf85263a_inta_callback_handler,
				   BIT(config->inta.pin));

		err = gpio_add_callback_dt(&config->inta, &data->inta_callback);
		if (err != 0) {
			LOG_ERR("failed to add GPIO callback (err %d)", err);
			return -ENODEV;
		}

		/*
		 * Create a thread to process RTC interrupts
		 * This way we can ensure that we don't stall the system work queue,
		 * which is usually used for interrupt processing
		 */
		tid = k_thread_create(&data->inta_thread, data->inta_stack,
				      K_THREAD_STACK_SIZEOF(data->inta_stack),
				      pcf85263a_inta_thread, (void *)dev, NULL,
				      NULL, CONFIG_RTC_PCF85263A_THREAD_PRIO, 0, K_NO_WAIT);
		k_thread_name_set(tid, "pcf85263a");
	}
#endif /* PCF85263A_INTA_GPIOS_IN_USE */

	err = pcf85263a_read_regs(dev, PCF85263A_OFFSET, &regs, sizeof(regs));
	if (err != 0) {
		return -err;
	}

	if (config->inta_mode != 0) {
		regs[3] &= ~(PCF85263A_PINIO_INTAPM);
		regs[3] |= PCF85263A_INTAPM_INTA;

		regs[5] = config->inta_mode;
	}

	regs[7] &= ~(PCF85263A_FLAGS_PIF | PCF85263A_FLAGS_WDF | PCF85263A_FLAGS_BSF |
			PCF85263A_FLAGS_T1F | PCF85263A_FLAGS_T2F | PCF85263A_FLAGS_T3F);

	err = pcf85263a_write_regs(dev, PCF85263A_OFFSET, &regs, sizeof(regs));
	if (err != 0) {
		return -err;
	}

	return 0;
}

static const struct rtc_driver_api pcf85263a_driver_api = {
	.set_time = pcf85263a_set_time,
	.get_time = pcf85263a_get_time,
#ifdef CONFIG_RTC_ALARM
	.alarm_get_supported_fields = pcf85263a_alarm_get_supported_fields,
	.alarm_set_time = pcf85263a_alarm_set_time,
	.alarm_get_time = pcf85263a_alarm_get_time,
	.alarm_is_pending = pcf85263a_alarm_is_pending,
#if PCF85263A_INTA_GPIOS_IN_USE
	.alarm_set_callback = pcf85263a_alarm_set_callback,
#endif /* PCF85263A_INTA_GPIOS_IN_USE */
#endif /* CONFIG_RTC_ALARM */
};

#define PCF85263A_INIT(inst)                                                                 \
	static const struct pcf85263a_config pcf85263a_config_##inst = {                     \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                           \
		.inta_mode = DT_INST_PROP_OR(inst, inta_mode, {0}),                          \
		IF_ENABLED(PCF85263A_INTA_GPIOS_IN_USE,                                      \
			   (.inta = GPIO_DT_SPEC_INST_GET_OR(inst, inta_gpios, {0})))};      \
                                                                                             \
	static struct pcf85263a_data pcf85263a_data_##inst;                                  \
                                                                                             \
                                                                                             \
	DEVICE_DT_INST_DEFINE(inst, &pcf85263a_init, NULL,                                   \
			      &pcf85263a_data_##inst, &pcf85263a_config_##inst, POST_KERNEL, \
			      CONFIG_RTC_INIT_PRIORITY, &pcf85263a_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PCF85263A_INIT)
