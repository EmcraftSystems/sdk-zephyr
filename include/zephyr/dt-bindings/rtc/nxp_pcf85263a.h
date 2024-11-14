/*
 *
 * Copyright (c) 2024 Emcraft Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_NXP_PCF85263A_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_NXP_PCF85263A_H_

#define INTx_WATCHDOG_IE          (1 << 0)
#define INTx_BATTERY_SWITCH_IE    (1 << 1)
#define INTx_TIMESTAMP_IE         (1 << 2)
#define INTx_ALARM2_IE            (1 << 3)
#define INTx_ALARM1_IE            (1 << 4)
#define INTx_OFFSET_CORRECTION_IE (1 << 5)
#define INTx_PERIODIC_INTERRUPT   (1 << 6)
#define INTx_LEVEL_MODE           (1 << 7)

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_NXP_PCF85263A_H_ */
