#ifndef ZEPHYR_INCLUDE_FS_LITTLEFS_H_
#define ZEPHYR_INCLUDE_FS_LITTLEFS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <kernel.h>
#include <lfs.h>

/**
 * @brief Filesystem info structure for LittleFS mount
 *
 * @param lfs    Internal LittleFS data
 * @param cfg    Internal LittleFS configuration
 *
 */
struct fs_littlefs_t {
	/* These structures are filled automatically at mount. */
	struct lfs lfs;
	struct lfs_config cfg;
	const struct flash_area *fa;
	struct k_mutex mutex;

	/* Static buffers */
	uint8_t read_buffer[CONFIG_FS_LITTLEFS_CACHE_SIZE];
	uint8_t prog_buffer[CONFIG_FS_LITTLEFS_CACHE_SIZE];
	uint64_t lookahead_buffer[CONFIG_FS_LITTLEFS_LOOKAHEAD_SIZE / 8];
};

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_FS_FS_INTERFACE_H_ */
