#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <errno.h>
#include <init.h>
#include <fs.h>

#define LFS_LOG_REGISTER
#include <lfs_util.h>

#include <lfs.h>
#include <fs/littlefs.h>
#include <flash.h>
#include <flash_map.h>

struct lfs_file_cache_t {
	lfs_file_t file;
	struct lfs_file_config config;
	uint8_t cache[CONFIG_FS_LITTLEFS_CACHE_SIZE];
};

#define LFS_FILE(fp) (((struct lfs_file_cache_t *)(fp->filep))->file)

/* Global memory pool for open files and dirs */
K_MEM_SLAB_DEFINE(lfs_file_pool, sizeof(struct lfs_file_cache_t),
		  CONFIG_FS_LITTLEFS_NUM_FILES, 4);
K_MEM_SLAB_DEFINE(lfs_dir_pool, sizeof(lfs_dir_t),
		  CONFIG_FS_LITTLEFS_NUM_DIRS, 4);

static int translate_error(int error)
{
	if (error >= 0)
		return error;

	switch (error) {
	case LFS_ERR_IO:       // Error during device operation
		return -EIO;
	case LFS_ERR_CORRUPT:  // Corrupted
		return -EIO;
	case LFS_ERR_NOENT:    // No directory entry
		return -ENOENT;
	case LFS_ERR_EXIST:    // Entry already exists
		return -EEXIST;
	case LFS_ERR_NOTDIR:   // Entry is not a dir
		return -ENOTDIR;
	case LFS_ERR_ISDIR:    // Entry is a dir
		return -EISDIR;
	case LFS_ERR_NOTEMPTY:	// Dir is not empty
		return -ENOTEMPTY;
	case LFS_ERR_BADF:	// Bad file number
		return -EBADF;
	case LFS_ERR_FBIG:	// File too large
		return -EFBIG;
	case LFS_ERR_INVAL:	// Invalid parameter
		return -EINVAL;
	case LFS_ERR_NOSPC:	// No space left on device
		return -ENOSPC;
	case LFS_ERR_NOMEM:	// No more memory available
		return -ENOMEM;
	default:
		return -EIO;
	}
}

int lfs_api_read(const struct lfs_config *c, lfs_block_t block,
                 lfs_off_t off, void *buffer, lfs_size_t size)
{
	const struct flash_area *fa = c->context;
	int offset = block * c->block_size + off;
	return flash_area_read(fa, offset, buffer, size);
}

int lfs_api_prog(const struct lfs_config *c, lfs_block_t block,
                 lfs_off_t off, const void *buffer, lfs_size_t size)
{
	const struct flash_area *fa = c->context;
	int offset = block * c->block_size + off;

	/* Writes can't cross page boundaries on some flash chips.
	   This really should be handled by a lower-level flash
	   driver layer in Zephyr, but it's not... */
	int ret = 0;
	while (size && ret == 0) {
		/* LFS should ensure this is true */
		__ASSERT_NO_MSG(size == c->prog_size);
		ret = flash_area_write(fa, offset, buffer, c->prog_size);
		offset += c->prog_size;
		buffer = (uint8_t *)buffer + c->prog_size;
		size -= c->prog_size;
	}
	return ret;
}

int lfs_api_erase(const struct lfs_config *c, lfs_block_t block)
{
	const struct flash_area *fa = c->context;
	int offset = block * c->block_size;
	int ret = flash_area_erase(fa, offset, c->block_size);
	return ret;
}

int lfs_api_sync(const struct lfs_config *c)
{
        return 0;
}

/* Zephyr VFS passes full paths -- this corrects them to just be the
   part specific to this filesystem.  This should be fixed
   elsewhere... */
static const char *xxx_fix_path(const struct fs_mount_t *mp, const char *path)
{
	const char *newpath = &path[mp->mountp_len];
	if (*newpath == '\0')
		return "/";
	return newpath;
}

static int littlefs_open(struct fs_file_t *fp, const char *path)
{
	path = xxx_fix_path(fp->mp, path);
	int flags = LFS_O_CREAT | LFS_O_RDWR;

	if (k_mem_slab_alloc(&lfs_file_pool, &fp->filep, K_NO_WAIT) != 0)
		return -ENOMEM;

	/* Use cache inside the slab allocation, instead of letting
	   littlefs allocate it from the heap. */
	struct lfs_file_cache_t *fc = fp->filep;
	fc->config = (struct lfs_file_config) {
		.buffer = fc->cache,
	};
	memset(&fc->file, 0, sizeof(struct lfs_file));
	int ret = lfs_file_opencfg(fp->mp->fs_data, &fc->file,
				   path, flags, &fc->config);
	if (ret < 0)
		k_mem_slab_free(&lfs_file_pool, &fp->filep);
	return translate_error(ret);
}

static int littlefs_close(struct fs_file_t *fp)
{
	int ret = lfs_file_close(fp->mp->fs_data, &LFS_FILE(fp));
	k_mem_slab_free(&lfs_file_pool, &fp->filep);
	return translate_error(ret);
}

static int littlefs_unlink(struct fs_mount_t *mountp, const char *path)
{
	path = xxx_fix_path(mountp, path);
	int ret = lfs_remove(mountp->fs_data, path);
	return translate_error(ret);
}

static int littlefs_rename(struct fs_mount_t *mountp, const char *from,
			   const char *to)
{
	int ret = lfs_rename(mountp->fs_data, from, to);
	return translate_error(ret);
}

static ssize_t littlefs_read(struct fs_file_t *fp, void *ptr, size_t len)
{
	int ret = lfs_file_read(fp->mp->fs_data, &LFS_FILE(fp), ptr, len);
	return translate_error(ret);
}

static ssize_t littlefs_write(struct fs_file_t *fp, const void *ptr, size_t len)
{
	int ret = lfs_file_write(fp->mp->fs_data, &LFS_FILE(fp), ptr, len);
	return translate_error(ret);
}

static int littlefs_seek(struct fs_file_t *fp, off_t off, int whence)
{
	_Static_assert((FS_SEEK_SET == LFS_SEEK_SET) &&
		       (FS_SEEK_CUR == LFS_SEEK_CUR) &&
		       (FS_SEEK_END == LFS_SEEK_END), "Flag mismatch");
	int ret = lfs_file_seek(fp->mp->fs_data, &LFS_FILE(fp), off, whence);
	/* XXX Zephyr API is bad; successful seek is documented to return 0! */
	if (ret > 0)
		return 0;
	return translate_error(ret);
}

static off_t littlefs_tell(struct fs_file_t *fp)
{
	int ret = lfs_file_tell(fp->mp->fs_data, &LFS_FILE(fp));
	return translate_error(ret);
}

static int littlefs_truncate(struct fs_file_t *fp, off_t length)
{
	int ret = lfs_file_truncate(fp->mp->fs_data, &LFS_FILE(fp), length);
	return translate_error(ret);
}

static int littlefs_sync(struct fs_file_t *fp)
{
	int ret = lfs_file_sync(fp->mp->fs_data, &LFS_FILE(fp));
	return translate_error(ret);
}

static int littlefs_mkdir(struct fs_mount_t *mountp, const char *path)
{
	path = xxx_fix_path(mountp, path);
	int ret = lfs_mkdir(mountp->fs_data, path);
	return translate_error(ret);
}

static int littlefs_opendir(struct fs_dir_t *dp, const char *path)
{
	path = xxx_fix_path(dp->mp, path);
	if (k_mem_slab_alloc(&lfs_dir_pool, &dp->dirp, K_NO_WAIT) != 0)
		return -ENOMEM;

	memset(dp->dirp, 0, sizeof(struct lfs_dir));
	int ret = lfs_dir_open(dp->mp->fs_data, dp->dirp, path);
	if (ret < 0)
		k_mem_slab_free(&lfs_dir_pool, &dp->dirp);
	return translate_error(ret);
}

static void info_to_dirent(const struct lfs_info *info, struct fs_dirent *entry)
{
	entry->type = ((info->type == LFS_TYPE_DIR) ?
		       FS_DIR_ENTRY_DIR : FS_DIR_ENTRY_FILE);
	entry->size = info->size;
	strncpy(entry->name, info->name, sizeof(entry->name));
	entry->name[sizeof(entry->name) - 1] = '\0';
}

static int littlefs_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
	struct lfs_info info;
	int ret = lfs_dir_read(dp->mp->fs_data, dp->dirp, &info);
	if (ret > 0) {
		info_to_dirent(&info, entry);
		ret = 0;
	} else if (ret == 0) {
		entry->name[0] = '\0';
	}

	return translate_error(ret);
}

static int littlefs_closedir(struct fs_dir_t *dp)
{
	int ret = lfs_dir_close(dp->mp->fs_data, dp->dirp);
	k_mem_slab_free(&lfs_dir_pool, &dp->dirp);
	return translate_error(ret);
}

static int littlefs_stat(struct fs_mount_t *mountp,
			 const char *path, struct fs_dirent *entry)
{
	path = xxx_fix_path(mountp, path);
	struct lfs_info info;
	int ret = lfs_stat(mountp->fs_data, path, &info);
	if (ret >= 0)
		info_to_dirent(&info, entry);
	return translate_error(ret);
}

static int littlefs_statvfs(struct fs_mount_t *mountp,
			    const char *path, struct fs_statvfs *stat)
{
	path = xxx_fix_path(mountp, path);
	struct lfs *lfs = mountp->fs_data;

	stat->f_bsize = lfs->cfg->prog_size;
	stat->f_frsize = lfs->cfg->block_size;
	stat->f_blocks = lfs->cfg->block_count;

	int ret = lfs_fs_size(lfs);
	if (ret >= 0)
		stat->f_bfree = stat->f_blocks - ret;

	return translate_error(ret);
}

/* Return minimum page size in a flash area.  There's no flash_area
 * API to implement this, so we have to make one here. */
struct get_page_ctx {
	const struct flash_area *fa;
	int min_size;
};
static bool get_page_cb(const struct flash_pages_info *info, void *ctxp)
{
	struct get_page_ctx *ctx = ctxp;

	int info_start = info->start_offset;
	int info_end   = info->start_offset + info->size - 1;
	int area_start = ctx->fa->fa_off;
	int area_end   = ctx->fa->fa_off + ctx->fa->fa_size - 1;

	/* Ignore pages outside the area */
	if (info_end < area_start)
		return true;
	if (info_start > area_end)
		return false;

	if (info->size > ctx->min_size)
		ctx->min_size = info->size;

	return true;
}
static int xxx_flash_area_page_size(const struct flash_area *fa)
{
	/* Iterate over all pages in the flash_area, and return the
	   largest page size we see in this area. */
	struct get_page_ctx ctx = {
		.fa = fa,
		.min_size = 0,
	};
	struct device *dev = flash_area_get_device(fa);
	flash_page_foreach(dev, get_page_cb, &ctx);
	return ctx.min_size ?: -1;
}

static int littlefs_mount(struct fs_mount_t *mountp)
{
	int ret;
	struct fs_littlefs_t *param = mountp->fs_data;
	int flash_area_id = (int) mountp->storage_dev;
	struct device *dev;

	/* Open flash area */
	ret = flash_area_open(flash_area_id, &param->fa);
	if (ret < 0 || param->fa == NULL) {
		LOG_ERR("can't open flash area %d", flash_area_id);
		return -ENODEV;
	}

	dev = flash_area_get_device(param->fa);
	if (dev == NULL) {
		LOG_ERR("can't get flash device");
		return -ENODEV;
	}

	/* Figure out flash parameters (harder than it should be!) */
	int read_size = 1;
	int prog_size = flash_area_align(param->fa);
	int block_size = xxx_flash_area_page_size(param->fa);
	int block_count = param->fa->fa_size / block_size;

	/* Ensure that sizes work out */
	__ASSERT(param->fa->fa_size % block_size == 0,
		 "partition size must be multiple of page size");
	__ASSERT(block_size % prog_size == 0,
		 "erase size must be multiple of write size");
	__ASSERT(((CONFIG_FS_LITTLEFS_CACHE_SIZE % prog_size) == 0) &&
		 ((block_size % CONFIG_FS_LITTLEFS_CACHE_SIZE) == 0),
		 "invalid cache size");
	__ASSERT((CONFIG_FS_LITTLEFS_LOOKAHEAD_SIZE % 8) == 0,
		 "invalid lookahead size");

	/* Build littlefs config */
	param->cfg = (struct lfs_config) {
		.context = (void *)param->fa,
		.read = lfs_api_read,
		.prog = lfs_api_prog,
		.erase = lfs_api_erase,
		.sync = lfs_api_sync,
		.read_size = read_size,
		.prog_size = prog_size,
		.block_size = block_size,
		.block_count = block_count,
		.block_cycles = CONFIG_FS_LITTLEFS_BLOCK_CYCLES,
		.cache_size = CONFIG_FS_LITTLEFS_CACHE_SIZE,
		.lookahead_size = CONFIG_FS_LITTLEFS_LOOKAHEAD_SIZE,
		.read_buffer = param->read_buffer,
		.prog_buffer = param->prog_buffer,
		.lookahead_buffer = param->lookahead_buffer
	};

	/* Mount it, formatting if needed. */
	ret = lfs_mount(&param->lfs, &param->cfg);
	if (ret < 0) {
		LOG_WRN("can't mount (%d); formatting", ret);
		ret = lfs_format(&param->lfs, &param->cfg);
		if (ret < 0) {
			LOG_ERR("format failed (%d)", ret);
			return -EIO;
		}
		ret = lfs_mount(&param->lfs, &param->cfg);
		if (ret < 0) {
			LOG_ERR("remount after format failed (%d)", ret);
			return -EIO;
		}
	}
	LOG_INF("filesystem mounted!");

	return 0;
}

static int littlefs_unmount(struct fs_mount_t *mountp)
{
	struct fs_littlefs_t *param = mountp->fs_data;

	lfs_unmount(&param->lfs);
	flash_area_close(param->fa);
	return 0;
}

/* File system interface */
static struct fs_file_system_t littlefs_fs = {
	.open = littlefs_open,
	.close = littlefs_close,
	.read = littlefs_read,
	.write = littlefs_write,
	.lseek = littlefs_seek,
	.tell = littlefs_tell,
	.truncate = littlefs_truncate,
	.sync = littlefs_sync,
	.opendir = littlefs_opendir,
	.readdir = littlefs_readdir,
	.closedir = littlefs_closedir,
	.mount = littlefs_mount,
	.unmount = littlefs_unmount,
	.unlink = littlefs_unlink,
	.rename = littlefs_rename,
	.mkdir = littlefs_mkdir,
	.stat = littlefs_stat,
	.statvfs = littlefs_statvfs,
};

static int littlefs_init(struct device *dev)
{
	ARG_UNUSED(dev);
	return fs_register(FS_LITTLEFS, &littlefs_fs);
}

SYS_INIT(littlefs_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
