/* ST Microelectronics LIS2DS12 3-axis accelerometer driver
 *
 * Copyright (c) 2019 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Datasheet:
 * https://www.st.com/resource/en/datasheet/lis2ds12.pdf
 */

#define DT_DRV_COMPAT st_lis2ds12

#include <zephyr/logging/log.h>
#include "lis2ds12.h"

LOG_MODULE_DECLARE(LIS2DS12, CONFIG_SENSOR_LOG_LEVEL);

static void lis2ds12_gpio_callback(const struct device *dev,
				   struct gpio_callback *cb, uint32_t pins)
{
	struct lis2ds12_data *data =
		CONTAINER_OF(cb, struct lis2ds12_data, gpio_cb);
	const struct lis2ds12_config *cfg = data->dev->config;
	int ret;

	ARG_UNUSED(pins);

	ret = gpio_pin_interrupt_configure_dt(&cfg->gpio_int, GPIO_INT_DISABLE);
	if (ret < 0) {
		LOG_ERR("%s: Not able to configure pin_int", dev->name);
	}

#if defined(CONFIG_LIS2DS12_TRIGGER_OWN_THREAD)
	k_sem_give(&data->trig_sem);
#elif defined(CONFIG_LIS2DS12_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&data->work);
#endif
}

static void lis2ds12_handle_int(const struct device *dev)
{
	const struct lis2ds12_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	lis2ds12_all_sources_t sources;
	lis2ds12_status_t istatus;
	int ret;
	struct lis2ds12_data *data = dev->data;
	lis2ds12_wake_up_src_t wake_up_src;

	if (data->trigger->type == SENSOR_TRIG_FREEFALL) {
		lis2ds12_read_reg(ctx, LIS2DS12_STATUS_DUP, (uint8_t *)&istatus, 1);
		if (istatus.ff_ia) {
			if (data->handler != NULL) {
				data->handler(dev, data->trigger);
			}
		}
	} else {
		lis2ds12_all_sources_get(ctx, &sources);

		if (data->trigger->type == SENSOR_TRIG_DATA_READY && sources.status_dup.drdy) {
			if (data->handler != NULL) {
				data->handler(dev, data->trigger);
			}
		}
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->gpio_int,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("%s: Not able to configure pin_int", dev->name);
	}

	if (data->trigger->type == SENSOR_TRIG_FREEFALL) {
		lis2ds12_read_reg(ctx, LIS2DS12_WAKE_UP_SRC,
				  (uint8_t *)&wake_up_src, 1);
	}
}

#ifdef CONFIG_LIS2DS12_TRIGGER_OWN_THREAD
static void lis2ds12_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct lis2ds12_data *data = p1;

	while (1) {
		k_sem_take(&data->trig_sem, K_FOREVER);
		lis2ds12_handle_int(data->dev);
	}
}
#endif

#ifdef CONFIG_LIS2DS12_TRIGGER_GLOBAL_THREAD
static void lis2ds12_work_cb(struct k_work *work)
{
	struct lis2ds12_data *data =
		CONTAINER_OF(work, struct lis2ds12_data, work);

	lis2ds12_handle_int(data->dev);
}
#endif

static int lis2ds12_init_interrupt(const struct device *dev)
{
	const struct lis2ds12_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	lis2ds12_pin_int1_route_t route;
	struct lis2ds12_data *data = dev->data;
	int err = 0;

	/* Enable pulsed mode */
	if (data->trigger->type == SENSOR_TRIG_DATA_READY) {
		err = lis2ds12_int_notification_set(ctx, LIS2DS12_INT_PULSED);
	} else if (data->trigger->type == SENSOR_TRIG_FREEFALL) {
		err = lis2ds12_int_notification_set(ctx, LIS2DS12_INT_LATCHED);
	}
	if (err < 0) {
		return err;
	}

	/* route data-ready interrupt on int1 */
	err = lis2ds12_pin_int1_route_get(ctx, &route);
	if (err < 0) {
		return err;
	}

	if (data->trigger->type == SENSOR_TRIG_DATA_READY) {
		route.int1_drdy = 1;
	} else if (data->trigger->type == SENSOR_TRIG_FREEFALL) {
		route.int1_ff = 1;
	}

	err = lis2ds12_pin_int1_route_set(ctx, route);
	if (err < 0) {
		return err;
	}

	return 0;
}

static int lis2ds12_ff_init(const struct device *dev)
{
	const struct lis2ds12_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	struct lis2ds12_data *lis2ds12 = dev->data;
	uint16_t duration;
	int ret;

	ret = lis2ds12_ff_threshold_set(ctx, cfg->ff_ths);
	if (ret < 0) {
		LOG_ERR("%s: ff threshold init error", dev->name);
		return ret;
	}

	duration = (LIS2DS12_REG_TO_ODR(lis2ds12->odr) * cfg->ff_dur) / 1000;

	ret = lis2ds12_ff_dur_set(ctx, duration);
	if (ret < 0) {
		LOG_ERR("%s: ff duration init error", dev->name);
		return ret;
	}

	LOG_DBG("%s: set FF parameters threshold %d, duration %d (%dms)", dev->name, cfg->ff_ths,
		duration, cfg->ff_dur);

	return 0;
}

int lis2ds12_trigger_init(const struct device *dev)
{
	struct lis2ds12_data *data = dev->data;
	const struct lis2ds12_config *cfg = dev->config;
	int ret;

	/* setup data ready gpio interrupt (INT1 or INT2) */
	if (!gpio_is_ready_dt(&cfg->gpio_int)) {
		if (cfg->gpio_int.port) {
			LOG_ERR("%s: device %s is not ready", dev->name,
						cfg->gpio_int.port->name);
			return -ENODEV;
		}

		LOG_DBG("%s: gpio_int not defined in DT", dev->name);
		return 0;
	}

	data->dev = dev;

	ret = gpio_pin_configure_dt(&cfg->gpio_int, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Could not configure gpio");
		return ret;
	}

	LOG_INF("%s: int on %s.%02u", dev->name, cfg->gpio_int.port->name,
				      cfg->gpio_int.pin);

	gpio_init_callback(&data->gpio_cb,
			   lis2ds12_gpio_callback,
			   BIT(cfg->gpio_int.pin));

	ret = gpio_add_callback(cfg->gpio_int.port, &data->gpio_cb);
	if (ret < 0) {
		LOG_ERR("Could not set gpio callback");
		return ret;
	}

#if defined(CONFIG_LIS2DS12_TRIGGER_OWN_THREAD)
	k_sem_init(&data->trig_sem, 0, K_SEM_MAX_LIMIT);

	k_thread_create(&data->thread, data->thread_stack,
			CONFIG_LIS2DS12_THREAD_STACK_SIZE,
			lis2ds12_thread,
			data, NULL, NULL,
			K_PRIO_COOP(CONFIG_LIS2DS12_THREAD_PRIORITY),
			0, K_NO_WAIT);
#elif defined(CONFIG_LIS2DS12_TRIGGER_GLOBAL_THREAD)
	data->work.handler = lis2ds12_work_cb;
#endif

	return gpio_pin_interrupt_configure_dt(&cfg->gpio_int,
					       GPIO_INT_EDGE_TO_ACTIVE);
}

int lis2ds12_trigger_set(const struct device *dev,
			 const struct sensor_trigger *trig,
			 sensor_trigger_handler_t handler)
{
	struct lis2ds12_data *data = dev->data;
	const struct lis2ds12_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	int16_t raw[3];
	int ret;

	__ASSERT_NO_MSG((trig->type == SENSOR_TRIG_DATA_READY) ||
			(trig->type == SENSOR_TRIG_FREEFALL));

	if (trig->type == SENSOR_TRIG_FREEFALL) {
		ret = lis2ds12_ff_init(dev);
		if (ret < 0) {
			return ret;
		}
	}

	if (data->trigger != NULL) {
		LOG_ERR("another trigger is already set");
		return -EBUSY;
	}

	if (cfg->gpio_int.port == NULL) {
		LOG_ERR("trigger_set is not supported");
		return -ENOTSUP;
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->gpio_int, GPIO_INT_DISABLE);
	if (ret < 0) {
		LOG_ERR("%s: Not able to configure pin_int", dev->name);
		return ret;
	}

	data->handler = handler;
	if (handler == NULL) {
		LOG_WRN("lis2ds12: no handler");
		return 0;
	}

	/* re-trigger lost interrupt */
	lis2ds12_acceleration_raw_get(ctx, raw);

	data->trigger = trig;

	lis2ds12_init_interrupt(dev);
	return gpio_pin_interrupt_configure_dt(&cfg->gpio_int,
					       GPIO_INT_EDGE_TO_ACTIVE);
}
