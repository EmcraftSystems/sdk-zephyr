/*
 * Copyright (c) 2022 Andreas Sandberg
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DISPLAY_ED070EC1_H_
#define ZEPHYR_INCLUDE_DISPLAY_ED070EC1_H_

#include <zephyr/drivers/display.h>

/**
 * ED070EC1 RAM type for direct RAM access
 */
enum ed070ec1_ram {
	/** The black RAM buffer. This is typically the buffer used to
	 * compose the contents that will be displayed after the next
	 * refresh.
	 */
	ED070EC1_RAM_BLACK = 0,
	/* The red RAM buffer. This is typically the old frame buffer
	 * when performing partial refreshes or an additional color
	 * channel.
	 */
	ED070EC1_RAM_RED,
};

/**
 * @brief Read data directly from the display controller's internal
 * RAM.
 *
 * @param dev Pointer to device structure
 * @param ram_type Type of RAM to read from
 * @param x Coordinate relative to the upper left corner
 * @param y Coordinate relative to the upper left corner
 * @param desc Structure describing the buffer layout
 * @param buf Output buffer
 *
 * @retval 0 on success, negative errno on failure.
 */
int ed070ec1_read_ram(const struct device *dev, enum ed070ec1_ram ram_type, const uint16_t x,
		      const uint16_t y, const struct display_buffer_descriptor *desc, void *buf);

#endif /* ZEPHYR_INCLUDE_DISPLAY_ED070EC1_H_ */
