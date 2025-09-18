/*
 * Copyright (c) 2025 Emcraft Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#ifndef ZEPHYR_LOG_BACKEND_FS_H_
#define ZEPHYR_LOG_BACKEND_FS_H_

const struct log_backend *log_backend_fs_get(void);
uint32_t log_backend_fs_get_boot_id(void);
int log_backend_fs_clear_logs(bool activate);

#endif  /* ZEPHYR_LOG_BACKEND_FS_H_ */
