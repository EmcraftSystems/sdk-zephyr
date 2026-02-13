/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output_dict.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_backend_fs.h>
#include <assert.h>
#include <zephyr/fs/fs.h>

#define MAX_PATH_LEN         256
#define MAX_FLASH_WRITE_SIZE 256
#define LOG_PREFIX_LEN       (sizeof(CONFIG_LOG_BACKEND_FS_FILE_PREFIX) - 1)

#if defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
#define UPTIME_LEN           10
#define BOOT_NUM_LEN         4
#define LOG_FILE_MAX_AGE_MS  (CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION_INTERVAL * 60 * 60 * 1000)
#define NEW_FILE_INTERVAL_MS (CONFIG_LOG_BACKEND_FS_MAX_FILE_TIME * 60 * 1000)
#else
#define MAX_FILE_NUMERAL 9999
#define FILE_NUMERAL_LEN 4
#endif

#define THREAD_PRIORITY (K_LOWEST_APPLICATION_THREAD_PRIO)

struct logfs_msg {
	uint8_t buf[MAX_FLASH_WRITE_SIZE];
	uint32_t len;
};

#define MSG_SIZE 4

sys_slist_t file_list = SYS_SLIST_STATIC_INIT(&file_list);
struct k_mutex file_list_lock;

K_MSGQ_DEFINE(log_msgq, sizeof(struct logfs_msg), MSG_SIZE, 1);
K_THREAD_STACK_DEFINE(logfs_queue_stack_area, CONFIG_LOG_BACKEND_FS_STACK_SIZE);
static struct k_work_q logfs_queue_work_q;
static struct k_work logfs_msg_work;

enum backend_fs_state {
	BACKEND_FS_NOT_INITIALIZED = 0,
	BACKEND_FS_CORRUPTED,
	BACKEND_FS_OK
};

static struct fs_file_t fs_file;
static enum backend_fs_state backend_state = BACKEND_FS_NOT_INITIALIZED;
static int file_ctr, newest;

static int allocate_new_file(struct fs_file_t *file);
static int del_oldest_log(void);
#ifndef CONFIG_LOG_BACKEND_FS_TESTSUITE
static uint32_t log_format_current = CONFIG_LOG_BACKEND_FS_OUTPUT_DEFAULT;
#if !defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
static int oldest;
#endif
#endif
static char fname[MAX_PATH_LEN];
#if defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
static uint32_t current_boot_number;
static int64_t current_boot_start_uptime;
static int64_t last_rotation_time;
#else
static int get_log_file_id(struct fs_dirent *ent);
#endif

static int check_log_volume_available(void)
{
	int index = 0;
	char const *name;
	int rc = 0;

	while (rc == 0) {
		rc = fs_readmount(&index, &name);
		if (rc == 0) {
			if (strncmp(CONFIG_LOG_BACKEND_FS_DIR, name, strlen(name)) == 0) {
				return 0;
			}
		}
	}

	return -ENOENT;
}

static int create_log_dir(const char *path)
{
	const char *next;
	const char *last = path + (strlen(path) - 1);
	char w_path[MAX_PATH_LEN];
	int rc, len;
	struct fs_dir_t dir;

	fs_dir_t_init(&dir);

	/* the fist directory name is the mount point*/
	/* the firs path's letter might be meaningless `/`, let's skip it */
	next = strchr(path + 1, '/');
	if (!next) {
		return 0;
	}

	while (true) {
		next++;
		if (next > last) {
			return 0;
		}
		next = strchr(next, '/');
		if (!next) {
			next = last;
			len = last - path + 1;
		} else {
			len = next - path;
		}

		memcpy(w_path, path, len);
		w_path[len] = 0;

		rc = fs_opendir(&dir, w_path);
		if (rc) {
			/* assume directory doesn't exist */
			rc = fs_mkdir(w_path);
			if (rc) {
				break;
			}
		}
		rc = fs_closedir(&dir);
		if (rc) {
			break;
		}
	}

	return rc;
}

static int file_list_add_item(uint32_t boot_id, uint64_t timestamp)
{
	struct file_list_item *item = k_malloc(sizeof(struct file_list_item));
	struct file_list_item *pn;
	struct file_list_item *prev = NULL;

	if (!item) {
		printf("%s: malloc error\n", __func__);
		return -ENOMEM;
	}

	item->bid = boot_id;
	item->ts = timestamp;

	k_mutex_lock(&file_list_lock, K_FOREVER);

	/* the list is ordered by boot id and timestamp */
	SYS_SLIST_FOR_EACH_CONTAINER(&file_list, pn, node) {
		if (pn->bid < item->bid || (pn->bid == item->bid && pn->ts < item->ts)) {
			prev = pn;
		} else {
			break;
		}
	}

	if (!prev) {
		/* no previous file found, append to the end of list */
		sys_slist_append(&file_list, &item->node);
	} else {
		/* insert by order */
		sys_slist_insert(&file_list, &prev->node, &item->node);
	}

	k_mutex_unlock(&file_list_lock);

	return 0;
}

static int file_list_update(void)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	uint32_t bid;
	uint64_t ts;
	int rc = 0;
	int cnt = 0;

	fs_dir_t_init(&dir);

	rc = fs_opendir(&dir, CONFIG_LOG_BACKEND_FS_DIR);
	if (rc) {
		printf("%s: cannot open log dir\n", __func__);
		return 0;
	}

	while (true) {
		rc = fs_readdir(&dir, &ent);
		if (rc < 0) {
			break;
		}
		if (ent.name[0] == 0) {
			break;
		}

		rc = sscanf(ent.name, "log.%u_%llu", &bid, &ts);
		if (rc == 2 && !file_list_add_item(bid, ts)) {
			cnt++;
		}
	}

	(void)fs_closedir(&dir);
	return cnt;
}

int log_backend_fs_flist_get_max_ts(struct sys_hashmap *map)
{
	struct file_list_item *pn;
	struct file_list_item *prev = NULL;

	sys_hashmap_clear(map, NULL, NULL);

	k_mutex_lock(&file_list_lock, K_FOREVER);

	if (sys_slist_is_empty(&file_list)) {
		k_mutex_unlock(&file_list_lock);
		return -ENODATA;
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&file_list, pn, node) {
		if (prev && pn->bid > prev->bid) {
			sys_hashmap_insert(map, prev->bid, prev->ts, NULL);
		}
		prev = pn;
	}

	/* last record is always highest timestamp */
	sys_hashmap_insert(map, prev->bid, prev->ts, NULL);

	k_mutex_unlock(&file_list_lock);

	return 0;
}

static struct file_list_item *file_list_get_oldest_file(void)
{
	struct file_list_item *cur = NULL;

	/* oldest file is always in the beginning of the list */
	cur = SYS_SLIST_PEEK_HEAD_CONTAINER(&file_list, cur, node);

	return cur;
}

#if defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
static int file_list_remove_file(struct file_list_item *item)
{
	char fname[MAX_PATH_LEN];
	int rc;

	snprintf(fname, sizeof(fname), "%s%s%04u_%010llu", CONFIG_LOG_BACKEND_FS_DIR,
		 CONFIG_LOG_BACKEND_FS_FILE_PREFIX, item->bid, item->ts);

	rc = fs_unlink(fname);
	if (rc) {
		printf("%s: cannot unlink %s (rc = %d)\n", __func__, fname, rc);
	} else {
		sys_slist_find_and_remove(&file_list, &item->node);
		k_free(item);
		file_ctr--;
	}

	return rc;
}
#endif

struct file_list_item *log_backend_fs_flist_get_next(struct file_list_item *item, bool cleanup)
{
	struct file_list_item *pn;
	struct file_list_item *next_item = NULL;

	k_mutex_lock(&file_list_lock, K_FOREVER);

	SYS_SLIST_FOR_EACH_CONTAINER(&file_list, pn, node) {
		if (pn->bid > item->bid || (pn->bid == item->bid && pn->ts > item->ts)) {
			next_item = pn;
			break;
		}
	}

#if defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
	if (next_item != NULL && cleanup) {
		/* We found the next file and the application
		 * requested a cleanup of the previous ones.
		 * Remove them all to speed up the file system.
		 */
		struct file_list_item *pns;

		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&file_list, pn, pns, node) {
			if (next_item == pn) {
				break;
			}
			file_list_remove_file(pn);
		}
	}
#endif

	k_mutex_unlock(&file_list_lock);

	return next_item;
}

#if !defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
static int check_log_file_exist(int num)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	int rc;

	fs_dir_t_init(&dir);

	rc = fs_opendir(&dir, CONFIG_LOG_BACKEND_FS_DIR);
	if (rc) {
		return -EIO;
	}

	while (true) {
		rc = fs_readdir(&dir, &ent);
		if (rc < 0) {
			rc = -EIO;
			goto close_dir;
		}
		if (ent.name[0] == 0) {
			break;
		}

		rc = get_log_file_id(&ent);

		if (rc == num) {
			rc = 1;
			goto close_dir;
		}
	}

	rc = 0;

close_dir:
	(void)fs_closedir(&dir);

	return rc;
}
#endif
int write_log_to_file(uint8_t *data, size_t length, void *ctx)
{
	int rc;
	struct fs_file_t *f = &fs_file;

	if (backend_state == BACKEND_FS_NOT_INITIALIZED) {
		if (check_log_volume_available()) {
			return length;
		}
		rc = create_log_dir(CONFIG_LOG_BACKEND_FS_DIR);
		if (!rc) {
			rc = allocate_new_file(&fs_file);
		}
		backend_state = (rc ? BACKEND_FS_CORRUPTED : BACKEND_FS_OK);
	}

	if (backend_state == BACKEND_FS_OK) {

		/* Check if new data overwrites max file size.
		 * If so, create new log file.
		 */
		int size = fs_tell(f);

		if (size < 0) {
			backend_state = BACKEND_FS_CORRUPTED;

			return length;
		} else if ((size + length) > CONFIG_LOG_BACKEND_FS_FILE_SIZE ||
			   (k_uptime_get() - last_rotation_time) > NEW_FILE_INTERVAL_MS) {

			rc = allocate_new_file(f);

			if (rc < 0) {
				goto on_error;
			}
		}

		rc = fs_write(f, data, length);
		if (rc >= 0) {
			if (IS_ENABLED(CONFIG_LOG_BACKEND_FS_OVERWRITE) && (rc != length)) {
				del_oldest_log();

				return 0;
			}
			/* If overwrite is disabled, full memory
			 * cause the log record abandonment.
			 */
			length = rc;
		} else {
#if !defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
			rc = check_log_file_exist(newest);

			if (rc == 0) {
				/* file was lost somehow
				 * try to get a new one
				 */
				file_ctr--;
				rc = allocate_new_file(f);
				if (rc < 0) {
					goto on_error;
				}
			} else if (rc < 0) {
				/* fs is corrupted*/
				goto on_error;
			}
#endif
			length = 0;
		}
	}

	return length;

on_error:
	backend_state = BACKEND_FS_CORRUPTED;
	return length;
}

static int logfs_producer(uint8_t *data, size_t length, void *ctx)
{
	struct logfs_msg msg;
	int ret;

	memcpy(msg.buf, data, length);
	msg.len = length;

	ret = k_msgq_put(&log_msgq, &msg, K_NO_WAIT);
	if (ret) {
		/* msgq is full */
		k_yield();
		return 0;
	}

	ret = k_work_submit_to_queue(&logfs_queue_work_q, &logfs_msg_work);
	if (ret < 0) {
		return 0;
	}

	return length;
}

static void logfs_consumer(struct k_work *work)
{
	struct logfs_msg msg;
	int rc;

	while (k_msgq_get(&log_msgq, &msg, K_NO_WAIT) == 0) {

		/* call the actual fs write routine */
		write_log_to_file((uint8_t *)&msg.buf, msg.len, NULL);
	}

	/* sync changes to fs */
	rc = fs_sync(&fs_file);

	if (rc < 0) {
		backend_state = BACKEND_FS_CORRUPTED;
	}
}

#if !defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
static int get_log_file_id(struct fs_dirent *ent)
{
	size_t len;
	int num;

	if (ent->type != FS_DIR_ENTRY_FILE) {
		return -1;
	}

	len = strlen(ent->name);

	if (len != LOG_PREFIX_LEN + FILE_NUMERAL_LEN) {
		return -1;
	}

	if (memcmp(ent->name, CONFIG_LOG_BACKEND_FS_FILE_PREFIX, LOG_PREFIX_LEN) != 0) {
		return -1;
	}

	num = atoi(ent->name + LOG_PREFIX_LEN);

	if (num <= MAX_FILE_NUMERAL && num >= 0) {
		return num;
	}

	return -1;
}
#else
/**
 * Determine current boot number by finding the highest boot
 * number in existing logs + 1
 *
 * @return Current boot number
 */
static uint32_t determine_boot_number(void)
{
	uint32_t max_boot_num = 0;
	struct file_list_item *pn;

	pn = SYS_SLIST_PEEK_TAIL_CONTAINER(&file_list, pn, node);
	if (pn) {
		max_boot_num = pn->bid;
	}

	return max_boot_num + 1;
}

uint32_t log_backend_fs_get_boot_id(void)
{
	if (backend_state == BACKEND_FS_OK) {
		return current_boot_number;
	} else {
		return -1;
	}
}

#endif /* CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION */

static uint64_t get_logs_duration(void);
static int allocate_new_file(struct fs_file_t *file)
{
	/* In case of no log file or current file fills up
	 * create new log file.
	 */
	int rc;
	struct fs_statvfs stat;
	int curr_file_num;

#if defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
	int64_t uptime;
#else
	struct fs_dirent ent;
	off_t file_size;
#endif

	assert(file);

	if (backend_state == BACKEND_FS_NOT_INITIALIZED) {
#if defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
		if (!file_ctr) {
			file_ctr = file_list_update();
		}

		while (file_ctr >= CONFIG_LOG_BACKEND_FS_FILES_LIMIT) {
			rc = del_oldest_log();
			if (rc) {
				goto out;
			}
		}

		current_boot_number = determine_boot_number();
		uptime = k_uptime_get();

		/* Create first file for this boot */
		snprintf(fname, sizeof(fname), "%s/%s%04u_%010lld", CONFIG_LOG_BACKEND_FS_DIR,
			 CONFIG_LOG_BACKEND_FS_FILE_PREFIX, current_boot_number, uptime);

		rc = fs_open(file, fname, FS_O_CREATE | FS_O_WRITE);
		if (rc < 0) {
			goto out;
		}

		if (!file_list_add_item(current_boot_number, uptime)) {
			file_ctr++;
		}

		backend_state = BACKEND_FS_OK;
		last_rotation_time = uptime;
		current_boot_start_uptime = uptime;
		goto out;
#else
		/* Search for the last used log number. */
		struct fs_dir_t dir;
		int file_num = 0;

		fs_dir_t_init(&dir);
		curr_file_num = 0;
		int max = 0, min = MAX_FILE_NUMERAL;

		rc = fs_opendir(&dir, CONFIG_LOG_BACKEND_FS_DIR);

		while (rc >= 0) {
			rc = fs_readdir(&dir, &ent);
			if ((rc < 0) || (ent.name[0] == 0)) {
				break;
			}

			file_num = get_log_file_id(&ent);
			if (file_num >= 0) {

				if (file_num > max) {
					max = file_num;
				}

				if (file_num < min) {
					min = file_num;
				}
				++file_ctr;
			}
		}

		oldest = min;

		if ((file_ctr > 1) && ((max - min) > 2 * CONFIG_LOG_BACKEND_FS_FILES_LIMIT)) {
			/* oldest log is in the range around the min */
			newest = min;
			oldest = max;
			(void)fs_closedir(&dir);
			rc = fs_opendir(&dir, CONFIG_LOG_BACKEND_FS_DIR);

			while (rc == 0) {
				rc = fs_readdir(&dir, &ent);
				if ((rc < 0) || (ent.name[0] == 0)) {
					break;
				}

				file_num = get_log_file_id(&ent);
				if (file_num < min + CONFIG_LOG_BACKEND_FS_FILES_LIMIT) {
					if (newest < file_num) {
						newest = file_num;
					}
				}

				if (file_num > max - CONFIG_LOG_BACKEND_FS_FILES_LIMIT) {
					if (oldest > file_num) {
						oldest = file_num;
					}
				}
			}
		} else {
			newest = max;
			oldest = min;
		}

		(void)fs_closedir(&dir);
		if (rc < 0) {
			goto out;
		}

		curr_file_num = newest;

		/* Is there space left in the newest file? */
		snprintf(fname, sizeof(fname), "%s/%s%04d", CONFIG_LOG_BACKEND_FS_DIR,
			 CONFIG_LOG_BACKEND_FS_FILE_PREFIX, curr_file_num);
		rc = fs_open(file, fname, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
		if (rc < 0) {
			goto out;
		}
		file_size = fs_tell(file);
		if (IS_ENABLED(CONFIG_LOG_BACKEND_FS_APPEND_TO_NEWEST_FILE) &&
		    file_size < CONFIG_LOG_BACKEND_FS_FILE_SIZE) {
			/* There is space left to log to the latest file, no need to create
			 * a new one or delete old ones at this point.
			 */
			if (file_ctr == 0) {
				++file_ctr;
			}
			backend_state = BACKEND_FS_OK;
			goto out;
		} else {
			fs_close(file);
			if (file_ctr >= 1) {
				curr_file_num++;
				if (curr_file_num > MAX_FILE_NUMERAL) {
					curr_file_num = 0;
				}
			}
			backend_state = BACKEND_FS_OK;
		}
#endif
	} else {
		fs_close(file);
#if !defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
		curr_file_num = newest;
		curr_file_num++;
		if (curr_file_num > MAX_FILE_NUMERAL) {
			curr_file_num = 0;
		}
#endif
	}

	rc = fs_statvfs(CONFIG_LOG_BACKEND_FS_DIR, &stat);

	/* Check if there is enough space to write file or max files number
	 * is not exceeded.
	 */
	while ((file_ctr >= CONFIG_LOG_BACKEND_FS_FILES_LIMIT) ||
	       ((stat.f_bfree * stat.f_frsize) <= CONFIG_LOG_BACKEND_FS_FILE_SIZE) ||
	       get_logs_duration() > (uint64_t)LOG_FILE_MAX_AGE_MS) {
		if (IS_ENABLED(CONFIG_LOG_BACKEND_FS_OVERWRITE)) {
			rc = del_oldest_log();
			if (rc < 0) {
				goto out;
			}
#if !defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
			rc = fs_statvfs(CONFIG_LOG_BACKEND_FS_DIR, &stat);
			if (rc < 0) {
				goto out;
			}
#endif
		} else {
			return -ENOSPC;
		}
	}

#if defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
	uptime = k_uptime_get();
	snprintf(fname, sizeof(fname), "%s/%s%04u_%010lld", CONFIG_LOG_BACKEND_FS_DIR,
		 CONFIG_LOG_BACKEND_FS_FILE_PREFIX, current_boot_number, uptime);
#else
	snprintf(fname, sizeof(fname), "%s/%s%04d", CONFIG_LOG_BACKEND_FS_DIR,
		 CONFIG_LOG_BACKEND_FS_FILE_PREFIX, curr_file_num);
#endif

	rc = fs_open(file, fname, FS_O_CREATE | FS_O_WRITE);
	if (rc < 0) {
		goto out;
	}
	if (!file_list_add_item(current_boot_number, uptime)) {
		file_ctr++;
	}

	last_rotation_time = uptime;
	newest = curr_file_num;

out:

	return rc;
}

static uint64_t get_logs_duration(void)
{
	uint64_t session_start_time = -1, session_end_time = -1;
	uint64_t total_duration = 0;
	int boot_num = -1;
	struct file_list_item *pn = NULL;

	SYS_SLIST_FOR_EACH_CONTAINER(&file_list, pn, node) {
		if (boot_num == pn->bid && pn->ts > session_end_time) {
			session_end_time = pn->ts;
		} else {
			if (boot_num != -1) {
				total_duration += ((session_end_time - session_start_time) +
						   NEW_FILE_INTERVAL_MS / 2);
			}
			session_start_time = pn->ts;
			session_end_time = pn->ts;
			boot_num = pn->bid;
		}
	}

	/* Add last boot as well*/
	total_duration += (session_end_time - session_start_time);

	return total_duration;
}

static int del_oldest_log(void)
{
	int rc = 0;
#if !defined(CONFIG_LOG_BACKEND_FS_TIME_BASED_ROTATION)
	while (true) {
		snprintf(fname, sizeof(fname), "%s/%s%04d", CONFIG_LOG_BACKEND_FS_DIR,
			 CONFIG_LOG_BACKEND_FS_FILE_PREFIX, oldest);
		rc = fs_unlink(fname);

		if ((rc == 0) || (rc == -ENOENT)) {
			oldest++;
			if (oldest > MAX_FILE_NUMERAL) {
				oldest = 0;
			}

			if (rc == 0) {
				--file_ctr;
				break;
			}
		} else {
			break;
		}
	}
#else
	struct file_list_item *item;

	item = file_list_get_oldest_file();
	if (item) {
		rc = file_list_remove_file(item);
	}
#endif
	return rc;
}

int log_backend_fs_clear_logs(bool activate)
{
	struct fs_dir_t dir;
	struct fs_dirent dirent;
	int rc;
	char path[sizeof(CONFIG_LOG_BACKEND_FS_DIR) + MAX_PATH_LEN + 1];
	struct k_work_sync sync;
	const struct log_backend *backend = log_backend_fs_get();

	/* Deactivate backend and stop the producer */
	log_backend_deactivate(backend);

	/* Cancel consumer */
	k_work_cancel_sync(&logfs_msg_work, &sync);

	/* Close current opened file */
	fs_close(&fs_file);

	fs_dir_t_init(&dir);

	rc = fs_opendir(&dir, CONFIG_LOG_BACKEND_FS_DIR);
	if (rc) {
		return -EIO;
	}

	while (true) {
		rc = fs_readdir(&dir, &dirent);
		if (rc < 0) {
			rc = -EIO;
			break;
		}
		if (dirent.name[0] == 0) {
			break;
		}

		snprintf(path, sizeof(CONFIG_LOG_BACKEND_FS_DIR) + MAX_PATH_LEN + 1, "%s/%s",
			 CONFIG_LOG_BACKEND_FS_DIR, dirent.name);

		(void)fs_unlink(path);
	}

	(void)fs_closedir(&dir);

	backend_state = BACKEND_FS_NOT_INITIALIZED;

	/* Optionally re-activate the backend */
	if (activate) {
		log_backend_activate(backend, NULL);
	}

	return rc;
}

BUILD_ASSERT(!IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE),
	     "Immediate logging is not supported by LOG FS backend.");

#ifndef CONFIG_LOG_BACKEND_FS_TESTSUITE

static uint8_t __aligned(4) buf[MAX_FLASH_WRITE_SIZE];
LOG_OUTPUT_DEFINE(log_output, logfs_producer, buf, MAX_FLASH_WRITE_SIZE);

static void log_backend_fs_init(const struct log_backend *const backend)
{
	int rc;
	struct k_work_queue_config cfg = {
		.name = "log_fs",
	};

	k_work_queue_init(&logfs_queue_work_q);
	k_work_queue_start(&logfs_queue_work_q, logfs_queue_stack_area,
			   K_THREAD_STACK_SIZEOF(logfs_queue_stack_area), THREAD_PRIORITY, &cfg);
	k_work_init(&logfs_msg_work, logfs_consumer);

	rc = k_mutex_init(&file_list_lock);
	if (rc != 0) {
		printf("%s: mutex init failed, rc = %d\n", __func__, rc);
		return;
	}

	if (backend_state == BACKEND_FS_NOT_INITIALIZED) {
		if (check_log_volume_available()) {
			/* will try to re-init later */
			return;
		}
		rc = create_log_dir(CONFIG_LOG_BACKEND_FS_DIR);
		if (!rc) {
			rc = allocate_new_file(&fs_file);
		}
		backend_state = (rc ? BACKEND_FS_CORRUPTED : BACKEND_FS_OK);
	}
}

static void panic(struct log_backend const *const backend)
{
	/* In case of panic deinitialize backend. It is better to keep
	 * current data rather than log new and risk of failure.
	 */
	log_backend_deactivate(backend);
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);

	if (IS_ENABLED(CONFIG_LOG_BACKEND_FS_OUTPUT_DICTIONARY)) {
		log_dict_output_dropped_process(&log_output, cnt);
	} else {
		log_backend_std_dropped(&log_output, cnt);
	}
}

static void process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	uint32_t flags = log_backend_std_get_flags();

	log_format_func_t log_output_func = log_format_func_t_get(log_format_current);

	log_output_func(&log_output, &msg->log, flags);
}

static int format_set(const struct log_backend *const backend, uint32_t log_type)
{
	log_format_current = log_type;
	return 0;
}

static const struct log_backend_api log_backend_fs_api = {
	.process = process,
	.panic = panic,
	.init = log_backend_fs_init,
	.dropped = dropped,
	.format_set = format_set,
};

LOG_BACKEND_DEFINE(log_backend_fs, log_backend_fs_api, IS_ENABLED(CONFIG_LOG_BACKEND_FS_AUTOSTART));

const struct log_backend *log_backend_fs_get(void)
{
	return &log_backend_fs;
}
#endif
