/*
 * Copyright (c) 2025 Emcraft Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#ifndef ZEPHYR_LOG_BACKEND_FS_H_
#define ZEPHYR_LOG_BACKEND_FS_H_

#include <zephyr/sys/hash_map.h>

struct file_list_item {
	sys_snode_t node;
	uint32_t bid;
	uint64_t ts;
};

const struct log_backend *log_backend_fs_get(void);
uint32_t log_backend_fs_get_boot_id(void);
int log_backend_fs_clear_logs(bool activate);
struct file_list_item *log_backend_fs_flist_get_next(struct file_list_item *item, bool cleanup);
int log_backend_fs_flist_get_max_ts(struct sys_hashmap *map);

#endif  /* ZEPHYR_LOG_BACKEND_FS_H_ */
