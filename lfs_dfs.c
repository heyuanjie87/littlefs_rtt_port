/*

 * File      : dfs_lfs.c

 */

#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_fs.h>
#include <dfs_file.h>
#include "lfs.h"

#include <rtdbg.h>

#define RT_DFS_LFS_DRIVES 1
#ifndef LFS_READ_SIZE
#define LFS_READ_SIZE 128
#endif
#ifndef LFS_PROG_SIZE
#define LFS_PROG_SIZE 256
#endif

#ifndef LFS_BLOCK_SIZE
#define LFS_BLOCK_SIZE 512
#endif

#ifndef LFS_LOOKAHEAD
#define LFS_LOOKAHEAD 512
#endif

typedef struct _dfs_lfs_s
{
    struct lfs lfs;
    struct lfs_config cfg;
} dfs_lfs_t;

typedef struct _dfs_lfs_fd_s
{
    struct lfs *lfs;
    union {
        struct lfs_file file;
        struct lfs_dir dir;
    } u;
} dfs_lfs_fd_t;

/*
* lfs flash operations
*/
static int _lfs_flash_read(const struct lfs_config *c, lfs_block_t block,
                           lfs_off_t off, void *buffer, lfs_size_t size)
{
    rt_mtd_t *mtd_nor;

    RT_ASSERT(c != RT_NULL);
    RT_ASSERT(c->context != RT_NULL);

    mtd_nor = (rt_mtd_t *)c->context;
    rt_mtd_read(mtd_nor, block * c->block_size + off, buffer, size);

    return LFS_ERR_OK;
}

static int _lfs_flash_prog(const struct lfs_config *c, lfs_block_t block,
                           lfs_off_t off, const void *buffer, lfs_size_t size)
{
    rt_mtd_t *mtd_nor;

    RT_ASSERT(c != RT_NULL);
    RT_ASSERT(c->context != RT_NULL);

    mtd_nor = (rt_mtd_t *)c->context;
    rt_mtd_write(mtd_nor, block * c->block_size + off, buffer, size);

    return LFS_ERR_OK;
}

static int _lfs_flash_erase(const struct lfs_config *c, lfs_block_t block)
{
    rt_mtd_t *mtd_nor;

    RT_ASSERT(c != RT_NULL);
    RT_ASSERT(c->context != RT_NULL);

    mtd_nor = (rt_mtd_t *)c->context;

    rt_mtd_erase(mtd_nor, block * c->block_size, c->block_size);

    return LFS_ERR_OK;
}

static int _lfs_flash_sync(const struct lfs_config *c)
{
    return LFS_ERR_OK;
}

static int lfs_result_to_dfs(int result)
{
    int status = 0;

    switch (result)
    {
    case LFS_ERR_OK:
        break;
    case LFS_ERR_IO:
        status = -EIO;
        break; // Error during device operation
    case LFS_ERR_NOENT:
        status = -ENOENT;
        break; // No directory entry
    case LFS_ERR_EXIST:
        status = -EEXIST;
        break; // Entry already exists
    case LFS_ERR_NOTDIR:
        status = -ENOTDIR;
        break; // Entry is not a dir
    case LFS_ERR_ISDIR:
        status = -EISDIR;
        break; // Entry is a dir
    case LFS_ERR_NOTEMPTY:
        status = -ENOTEMPTY;
        break; // Dir is not empty
    case LFS_ERR_BADF:
        status = -EBADF;
        break; // Bad file number
    case LFS_ERR_INVAL:
        status = -EINVAL;
        break; // Invalid parameter
    case LFS_ERR_NOSPC:
        status = -ENOSPC;
        break; // No space left on device
    case LFS_ERR_NOMEM:
        status = -ENOMEM;
        break; // No more memory available
    case LFS_ERR_CORRUPT:
        status = -52;
        break; // Corrupted
    default:
        status = -EIO;
        break;
    }

    return status;
}

static void _dfs_lfs_load_config(dfs_lfs_t *dfs_lfs, rt_mtd_t *mtd_nor)
{
    dfs_lfs->cfg.context = (void *)mtd_nor;

    dfs_lfs->cfg.read_size = 1;

    if (dfs_lfs->cfg.read_size < LFS_READ_SIZE)
    {
        dfs_lfs->cfg.read_size = LFS_READ_SIZE;
    }

    dfs_lfs->cfg.prog_size = 1;
    if (dfs_lfs->cfg.prog_size < LFS_PROG_SIZE)
    {
        dfs_lfs->cfg.prog_size = LFS_PROG_SIZE;
    }

    dfs_lfs->cfg.block_size = mtd_nor->block_size;
    if (dfs_lfs->cfg.block_size < LFS_BLOCK_SIZE)
    {
        dfs_lfs->cfg.block_size = LFS_BLOCK_SIZE;
    }

    dfs_lfs->cfg.block_count = mtd_nor->size / mtd_nor->block_size;
    dfs_lfs->cfg.lookahead_size = 32 * ((dfs_lfs->cfg.block_count + 31) / 32);
    dfs_lfs->cfg.block_cycles = 500;

    if (dfs_lfs->cfg.lookahead_size > LFS_LOOKAHEAD)
    {
        dfs_lfs->cfg.lookahead_size = LFS_LOOKAHEAD;
    }

    dfs_lfs->cfg.read = &_lfs_flash_read;
    dfs_lfs->cfg.prog = &_lfs_flash_prog;
    dfs_lfs->cfg.erase = &_lfs_flash_erase;
    dfs_lfs->cfg.sync = &_lfs_flash_sync;
}

static int dfs_lfs_mount(struct dfs_filesystem *dfs,
                         unsigned long rwflag, const void *data)
{
    int result;
    dfs_lfs_t *dfs_lfs;
    rt_mtd_t *mtd;

    /* Check Device Type */
    if (dfs->dev_id->type != RT_Device_Class_MTD)
    {
        LOG_E("must be MTD!\n");
        return -EINVAL;
    }
    mtd = (rt_mtd_t *)dfs->dev_id;

    /*create lfs handle */
    dfs_lfs = rt_malloc(sizeof(dfs_lfs_t));
    if (dfs_lfs == RT_NULL)
    {
        LOG_E("no memory!\n");
        return -ENOMEM;
    }

    rt_memset(dfs_lfs, 0, sizeof(dfs_lfs_t));
    _dfs_lfs_load_config(dfs_lfs, mtd);

    /* mount lfs*/
    result = lfs_mount(&dfs_lfs->lfs, &dfs_lfs->cfg);
    if (result == LFS_ERR_OK)
    {
        dfs->data = (void *)dfs_lfs;
        return 0;
    }

    /* release memory */
    rt_free(dfs_lfs);

    return -EIO;
}

static int dfs_lfs_unmount(struct dfs_filesystem *dfs)
{
    dfs_lfs_t *dfs_lfs;

    dfs_lfs = (dfs_lfs_t *)dfs->data;

    dfs->data = RT_NULL;
    lfs_unmount(&dfs_lfs->lfs);
    rt_free(dfs_lfs);

    return 0;
}

static int dfs_lfs_mkfs(rt_device_t dev_id)
{
    dfs_lfs_t *dfs_lfs;
    rt_mtd_t *mtd_nor;
    int result;

    /* Check Device Type */
    if (dev_id->type != RT_Device_Class_MTD)
    {
        LOG_E("must be MTD!\n");
        return -EINVAL;
    }
    mtd_nor = (rt_mtd_t *)dev_id;

    dfs_lfs = rt_malloc(sizeof(dfs_lfs_t));
    if (!dfs_lfs)
    {
        LOG_E("no memory!\n");
        return -ENOMEM;
    }
    rt_memset(dfs_lfs, 0, sizeof(dfs_lfs_t));
    _dfs_lfs_load_config(dfs_lfs, mtd_nor);

    /* format flash device */
    result = lfs_format(&dfs_lfs->lfs, &dfs_lfs->cfg);

    if (result != LFS_ERR_OK)
    {
        return lfs_result_to_dfs(result);
    }

    return 0;
}

static int _dfs_lfs_statfs_count(void *p, lfs_block_t b)
{
    *(lfs_size_t *)p += 1;

    return 0;
}

static int dfs_lfs_statfs(struct dfs_filesystem *dfs, struct statfs *buf)
{
    dfs_lfs_t *dfs_lfs;
    int result;
    lfs_size_t in_use = 0;

    RT_ASSERT(buf != RT_NULL);
    RT_ASSERT(dfs != RT_NULL);
    RT_ASSERT(dfs->data != RT_NULL);

    dfs_lfs = (dfs_lfs_t *)dfs->data;

    /* Get total sectors and free sectors */

    result = lfs_fs_traverse(&dfs_lfs->lfs, _dfs_lfs_statfs_count, &in_use);

    if (result != LFS_ERR_OK)
    {
        return lfs_result_to_dfs(result);
    }

    buf->f_bsize = dfs_lfs->cfg.block_size;
    buf->f_blocks = dfs_lfs->cfg.block_count;
    buf->f_bfree = dfs_lfs->cfg.block_count - in_use;

    return RT_EOK;
}

static int dfs_lfs_unlink(struct dfs_filesystem *dfs, const char *path)
{
    dfs_lfs_t *dfs_lfs;
    int result;

    RT_ASSERT(dfs != RT_NULL);
    RT_ASSERT(dfs->data != RT_NULL);
    dfs_lfs = (dfs_lfs_t *)dfs->data;

    result = lfs_remove(&dfs_lfs->lfs, path);

    return lfs_result_to_dfs(result);
}

static void _dfs_lfs_tostat(struct stat *st, struct lfs_info *info)
{
    rt_memset(st, 0, sizeof(struct stat));

    /* convert to dfs stat structure */

    st->st_dev = 0;
    st->st_size = info->size;
    st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
    switch (info->type)
    {
    case LFS_TYPE_DIR:
        st->st_mode |= S_IFDIR;
        break;
    case LFS_TYPE_REG:
        st->st_mode |= S_IFREG;
        break;
    }
}

static int dfs_lfs_stat(struct dfs_filesystem *dfs,
                 const char *path, struct stat *st)
{
    dfs_lfs_t *dfs_lfs;
    int result;
    struct lfs_info info;

    RT_ASSERT(dfs != RT_NULL);
    RT_ASSERT(dfs->data != RT_NULL);

    dfs_lfs = (dfs_lfs_t *)dfs->data;

    result = lfs_stat(&dfs_lfs->lfs, path, &info);
    if (result != LFS_ERR_OK)
    {
        return lfs_result_to_dfs(result);
    }

    _dfs_lfs_tostat(st, &info);

    return 0;
}

static int dfs_lfs_rename(struct dfs_filesystem *dfs,
                          const char *from, const char *to)
{
    dfs_lfs_t *dfs_lfs;
    int result;

    RT_ASSERT(dfs != RT_NULL);
    RT_ASSERT(dfs->data != RT_NULL);

    dfs_lfs = (dfs_lfs_t *)dfs->data;
    result = lfs_rename(&dfs_lfs->lfs, from, to);

    return lfs_result_to_dfs(result);
}

/*
 * file operations
 */

static int dfs_lfs_open(struct dfs_fd *file)
{
    struct dfs_filesystem *fs;
    dfs_lfs_t *dfs_lfs;
    int result;
    int flags = 0;

    fs = (struct dfs_filesystem *)file->data;
    dfs_lfs = (dfs_lfs_t *)fs->data;

    if (file->flags & O_DIRECTORY)
    {
        dfs_lfs_fd_t *dfs_lfs_fd = rt_malloc(sizeof(dfs_lfs_fd_t));

        if (dfs_lfs_fd == RT_NULL)
        {
            LOG_E("no memory!\n");

            result = -ENOMEM;
            goto _error_dir;
        }

        rt_memset(dfs_lfs_fd, 0, sizeof(dfs_lfs_fd_t));
        dfs_lfs_fd->lfs = &dfs_lfs->lfs;

        if (file->flags & O_CREAT)
        {
            result = lfs_mkdir(dfs_lfs_fd->lfs, file->path);

            if (result != LFS_ERR_OK)
            {
                goto _error_dir;
            }
        }

        result = lfs_dir_open(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.dir, file->path);
        if (result != LFS_ERR_OK)
        {
            goto _error_dir;
        }
        else
        {
            file->data = (void *)dfs_lfs_fd;

            return RT_EOK;
        }

    _error_dir:
        if (dfs_lfs_fd != RT_NULL)
        {
            rt_free(dfs_lfs_fd);
        }
    }
    else
    {
        dfs_lfs_fd_t *dfs_lfs_fd = rt_malloc(sizeof(dfs_lfs_fd_t));

        if (dfs_lfs_fd == RT_NULL)
        {
            rt_kprintf("err: no memory!\n");

            result = -ENOMEM;
            goto _error_file;
        }

        rt_memset(dfs_lfs_fd, 0, sizeof(dfs_lfs_fd_t));
        dfs_lfs_fd->lfs = &dfs_lfs->lfs;

        if ((file->flags & 3) == O_RDONLY)
            flags |= LFS_O_RDONLY;
        if ((file->flags & 3) == O_WRONLY)
            flags |= LFS_O_WRONLY;
        if ((file->flags & 3) == O_RDWR)
            flags |= LFS_O_RDWR;
        if (file->flags & O_CREAT)
            flags |= LFS_O_CREAT;
        if (file->flags & O_EXCL)
            flags |= LFS_O_EXCL;
        if (file->flags & O_TRUNC)
            flags |= LFS_O_TRUNC;
        if (file->flags & O_APPEND)
            flags |= LFS_O_APPEND;

        result = lfs_file_open(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file, file->path, flags);
        if (result != LFS_ERR_OK)
        {
            goto _error_file;
        }
        else
        {
            file->data = (void *)dfs_lfs_fd;
            file->pos = dfs_lfs_fd->u.file.pos;
            file->size = lfs_file_size(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file);

            return 0;
        }

    _error_file:
        if (dfs_lfs_fd != RT_NULL)
        {
            rt_free(dfs_lfs_fd);
        }
    }

    return lfs_result_to_dfs(result);
}

static int dfs_lfs_close(struct dfs_fd *file)
{
    int result;
    dfs_lfs_fd_t *dfs_lfs_fd;

    RT_ASSERT(file != RT_NULL);
    RT_ASSERT(file->data != RT_NULL);

    dfs_lfs_fd = (dfs_lfs_fd_t *)file->data;

    if (file->type == FT_DIRECTORY)
    {
        result = lfs_dir_close(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.dir);
    }
    else
    {
        result = lfs_file_close(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file);
    }

    rt_free(dfs_lfs_fd);

    return lfs_result_to_dfs(result);
}

static int dfs_lfs_ioctl(struct dfs_fd *file, int cmd, void *args)
{
    return -ENOSYS;
}

static int dfs_lfs_read(struct dfs_fd *file, void *buf, size_t len)
{
    lfs_ssize_t ssize;
    dfs_lfs_fd_t *dfs_lfs_fd;

    RT_ASSERT(file != RT_NULL);
    RT_ASSERT(file->data != RT_NULL);

    dfs_lfs_fd = (dfs_lfs_fd_t *)file->data;

    if (file->type == FT_DIRECTORY)
    {
        return -EISDIR;
    }

    if (lfs_file_tell(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file) != file->pos)
    {
        lfs_soff_t soff = lfs_file_seek(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file, file->pos, LFS_SEEK_SET);

        if (soff < 0)
        {
            return lfs_result_to_dfs(soff);
        }
    }

    ssize = lfs_file_read(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file, buf, len);

    if (ssize < 0)
    {
        return lfs_result_to_dfs(ssize);
    }

    /* update position */

    file->pos = dfs_lfs_fd->u.file.pos;

    return ssize;
}

static int dfs_lfs_write(struct dfs_fd *file, const void *buf, size_t len)
{
    lfs_ssize_t ssize;
    dfs_lfs_fd_t *dfs_lfs_fd;

    RT_ASSERT(file != RT_NULL);
    RT_ASSERT(file->data != RT_NULL);

    if (file->type == FT_DIRECTORY)
    {
        return -EISDIR;
    }

    dfs_lfs_fd = (dfs_lfs_fd_t *)file->data;

    if (lfs_file_tell(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file) != file->pos)
    {
        lfs_soff_t soff = lfs_file_seek(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file, file->pos, LFS_SEEK_SET);

        if (soff < 0)
        {
            return lfs_result_to_dfs(soff);
        }
    }

    ssize = lfs_file_write(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file, buf, len);

    if (ssize < 0)
    {
        return lfs_result_to_dfs(ssize);
    }

    /* update position and file size */

    file->pos = dfs_lfs_fd->u.file.pos;

    file->size = lfs_file_size(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file);

    return ssize;
}

static int dfs_lfs_flush(struct dfs_fd *file)
{
    int result;
    dfs_lfs_fd_t *dfs_lfs_fd;

    RT_ASSERT(file != RT_NULL);
    RT_ASSERT(file->data != RT_NULL);

    dfs_lfs_fd = (dfs_lfs_fd_t *)file->data;

    result = lfs_file_sync(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file);

    return lfs_result_to_dfs(result);
}

static int dfs_lfs_lseek(struct dfs_fd *file, off_t offset, int whence)
{
    dfs_lfs_fd_t *dfs_lfs_fd;

    RT_ASSERT(file != RT_NULL);
    RT_ASSERT(file->data != RT_NULL);

    dfs_lfs_fd = (dfs_lfs_fd_t *)file->data;

    if (file->type == FT_REGULAR)
    {
        lfs_soff_t soff = lfs_file_seek(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.file, offset, LFS_SEEK_SET);

        if (soff < 0)
        {
            return lfs_result_to_dfs(soff);
        }

        file->pos = dfs_lfs_fd->u.file.pos;
    }
    else if (file->type == FT_DIRECTORY)
    {
        lfs_soff_t soff = lfs_dir_seek(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.dir, offset);

        if (soff < 0)
        {
            return lfs_result_to_dfs(soff);
        }

        file->pos = dfs_lfs_fd->u.dir.pos;
    }

    return (file->pos);
}

static int dfs_lfs_getdents(struct dfs_fd *file, struct dirent *dirp, uint32_t count)
{
    dfs_lfs_fd_t *dfs_lfs_fd;
    int result;
    int index;
    struct dirent *d;
    struct lfs_info info;

    RT_ASSERT(file->data != RT_NULL);

    dfs_lfs_fd = (dfs_lfs_fd_t *)(file->data);

    /* make integer count */

    count = (count / sizeof(struct dirent)) * sizeof(struct dirent);

    if (count == 0)
    {
        return -EINVAL;
    }

    index = 0;

    while (1)
    {
        d = dirp + index;

        result = lfs_dir_read(dfs_lfs_fd->lfs, &dfs_lfs_fd->u.dir, &info);

        if ((result != 1) || (info.name[0] == 0))
        {
            return result;
        }

        d->d_type = DT_UNKNOWN;

        switch (info.type)
        {
        case LFS_TYPE_DIR:
            d->d_type |= DT_DIR;
            break;

        case LFS_TYPE_REG:
            d->d_type |= DT_REG;
            break;
        }

        d->d_namlen = (rt_uint8_t)rt_strlen(info.name);
        d->d_reclen = (rt_uint16_t)sizeof(struct dirent);
        rt_strncpy(d->d_name, info.name, rt_strlen(info.name) + 1);
        index++;
        if (index * sizeof(struct dirent) >= count)
        {
            break;
        }
    }

    if (index == 0)
    {
        return lfs_result_to_dfs(result);
    }

    file->pos += index * sizeof(struct dirent);

    return index * sizeof(struct dirent);
}

static const struct dfs_file_ops _dfs_lfs_fops =
{
    dfs_lfs_open,
    dfs_lfs_close,
    dfs_lfs_ioctl,
    dfs_lfs_read,
    dfs_lfs_write,
    dfs_lfs_flush,
    dfs_lfs_lseek,
    dfs_lfs_getdents,
    RT_NULL, /* poll interface */
};

static const struct dfs_filesystem_ops _dfs_lfs_ops =
{
    "lfs",
    DFS_FS_FLAG_DEFAULT,
    &_dfs_lfs_fops,
    dfs_lfs_mount,
    dfs_lfs_unmount,
    dfs_lfs_mkfs,
    dfs_lfs_statfs,
    dfs_lfs_unlink,
    dfs_lfs_stat,
    dfs_lfs_rename,
};

int dfs_lfs_init(void)
{
    /* register ram file system */

    dfs_register(&_dfs_lfs_ops);

    return 0;
}
INIT_COMPONENT_EXPORT(dfs_lfs_init);
