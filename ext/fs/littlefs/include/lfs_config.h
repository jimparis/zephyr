#ifndef LFS_CONFIG_H
#define LFS_CONFIG_H

/* The only functions we care about replacing here are LFS_DEBUG,
   LFS_WARN, and LFS_ERROR.  So do some preprocessor trickery to
   pull in the original lfs_util.h again, then replace just those
   macros. */

#undef LFS_CONFIG
#undef LFS_UTIL_H
#include <lfs_util.h>

#undef LFS_DEBUG
#undef LFS_WARN
#undef LFS_ERROR

#define LFS_DEBUG LOG_DBG
#define LFS_WARN  LOG_WRN
#define LFS_ERROR LOG_ERR

#include <logging/log.h>
#ifdef LFS_LOG_REGISTER
LOG_MODULE_REGISTER(littlefs, CONFIG_FS_LOG_LEVEL);
#else
LOG_MODULE_DECLARE(littlefs);
#endif

#endif
