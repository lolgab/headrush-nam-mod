/* ext4_image.c -- see ext4_image.h. */
#include "ext4_image.h"

#include <ext2fs/ext2fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Ext4Image
{
  ext2_filsys fs;
  bool read_write;
};

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

static void set_err_code(char* err, size_t err_size, errcode_t code, const char* what)
{
  if (!err || err_size == 0)
    return;
  const char* msg = error_message(code);
  snprintf(err, err_size, "%s: %s", what, msg ? msg : "unknown error");
}

bool nam_ext4_open(const char* path, bool read_write, Ext4Image** out, char* err, size_t err_size)
{
  *out = NULL;
  Ext4Image* img = (Ext4Image*)calloc(1, sizeof(Ext4Image));
  if (!img)
  {
    set_err(err, err_size, "out of memory");
    return false;
  }
  img->read_write = read_write;

  /* default_io_manager resolves to unix_io_manager or windows_io_manager
   * (ext2_io.h, #ifdef _WIN32) -- e2fsprogs's own portable pick, not
   * something we need to branch on ourselves. */
  errcode_t retval = ext2fs_open(path, read_write ? EXT2_FLAG_RW : 0, 0, 0, default_io_manager, &img->fs);
  if (retval)
  {
    set_err_code(err, err_size, retval, "opening ext4 image");
    free(img);
    return false;
  }

  retval = ext2fs_read_bitmaps(img->fs);
  if (retval)
  {
    set_err_code(err, err_size, retval, "reading block/inode bitmaps");
    ext2fs_close(img->fs);
    free(img);
    return false;
  }

  *out = img;
  return true;
}

void nam_ext4_close(Ext4Image* img)
{
  if (!img)
    return;
  if (img->read_write)
    ext2fs_flush(img->fs);
  ext2fs_close(img->fs);
  free(img);
}

/* Splits "/a/b/c" into parent="/a/b" and base="c". Root-level paths like
 * "/c" split into parent="/" and base="c". */
static bool split_path(const char* path, char* parent, size_t parent_sz, char* base, size_t base_sz)
{
  size_t len = strlen(path);
  if (len == 0 || path[0] != '/')
    return false;
  const char* slash = strrchr(path, '/');
  size_t base_len = len - (size_t)(slash - path) - 1;
  if (base_len == 0 || base_len >= base_sz)
    return false;
  memcpy(base, slash + 1, base_len);
  base[base_len] = '\0';

  size_t parent_len = (size_t)(slash - path);
  if (parent_len == 0)
  {
    if (parent_sz < 2)
      return false;
    parent[0] = '/';
    parent[1] = '\0';
  }
  else
  {
    if (parent_len >= parent_sz)
      return false;
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';
  }
  return true;
}

static int free_block_cb(ext2_filsys fs, blk64_t* blocknr, e2_blkcnt_t blockcnt, blk64_t ref_blk, int ref_offset,
                          void* priv)
{
  (void)blockcnt;
  (void)ref_blk;
  (void)ref_offset;
  (void)priv;
  ext2fs_block_alloc_stats2(fs, *blocknr, -1);
  return 0;
}

/* Removes `name` from directory `parent` if it exists (mirrors debugfs's
 * "rm", which the old Python pipeline's inject() ran with check=False -- a
 * missing target is not an error). Frees the old inode's blocks and inode
 * slot once its link count reaches zero. */
static errcode_t remove_if_exists(ext2_filsys fs, ext2_ino_t parent, const char* name)
{
  ext2_ino_t ino;
  errcode_t retval = ext2fs_namei(fs, parent, parent, name, &ino);
  if (retval)
    return 0; /* not found -- fine */

  retval = ext2fs_unlink(fs, parent, name, ino, 0);
  if (retval)
    return retval;

  struct ext2_inode inode;
  retval = ext2fs_read_inode(fs, ino, &inode);
  if (retval)
    return retval;

  if (inode.i_links_count > 0)
    inode.i_links_count--;

  if (inode.i_links_count == 0)
  {
    retval = ext2fs_block_iterate3(fs, ino, BLOCK_FLAG_READ_ONLY, NULL, free_block_cb, NULL);
    if (retval)
      return retval;
    ext2fs_inode_alloc_stats2(fs, ino, -1, LINUX_S_ISDIR(inode.i_mode));
    inode.i_dtime = (__u32)time(NULL);
  }
  return ext2fs_write_inode(fs, ino, &inode);
}

bool nam_ext4_dump(Ext4Image* img, const char* inner_path, uint8_t** out_data, size_t* out_len, char* err,
                    size_t err_size)
{
  *out_data = NULL;
  *out_len = 0;

  ext2_ino_t ino;
  errcode_t retval = ext2fs_namei(img->fs, EXT2_ROOT_INO, EXT2_ROOT_INO, inner_path, &ino);
  if (retval)
  {
    set_err_code(err, err_size, retval, inner_path);
    return false;
  }

  ext2_file_t file;
  retval = ext2fs_file_open(img->fs, ino, 0, &file);
  if (retval)
  {
    set_err_code(err, err_size, retval, "opening file for read");
    return false;
  }

  ext2_off64_t size = 0;
  retval = ext2fs_file_get_lsize(file, (__u64*)&size);
  if (retval)
  {
    set_err_code(err, err_size, retval, "getting file size");
    ext2fs_file_close(file);
    return false;
  }

  uint8_t* buf = (uint8_t*)malloc((size_t)size ? (size_t)size : 1);
  if (!buf)
  {
    set_err(err, err_size, "out of memory (%lld bytes)", (long long)size);
    ext2fs_file_close(file);
    return false;
  }

  size_t total = 0;
  while (total < (size_t)size)
  {
    unsigned int got = 0;
    retval = ext2fs_file_read(file, buf + total, (unsigned int)((size_t)size - total), &got);
    if (retval || got == 0)
    {
      if (retval)
        set_err_code(err, err_size, retval, "reading file content");
      else
        set_err(err, err_size, "short read: got %zu of %lld bytes", total, (long long)size);
      free(buf);
      ext2fs_file_close(file);
      return false;
    }
    total += got;
  }

  ext2fs_file_close(file);
  *out_data = buf;
  *out_len = total;
  return true;
}

bool nam_ext4_inject(Ext4Image* img, const char* inner_path, const uint8_t* data, size_t len, uint32_t unix_mode,
                      char* err, size_t err_size)
{
  char parent_path[512], base[256];
  if (!split_path(inner_path, parent_path, sizeof(parent_path), base, sizeof(base)))
  {
    set_err(err, err_size, "invalid inner path %s", inner_path);
    return false;
  }

  ext2_ino_t parent_ino;
  errcode_t retval = ext2fs_namei(img->fs, EXT2_ROOT_INO, EXT2_ROOT_INO, parent_path, &parent_ino);
  if (retval)
  {
    set_err_code(err, err_size, retval, parent_path);
    return false;
  }

  retval = remove_if_exists(img->fs, parent_ino, base);
  if (retval)
  {
    set_err_code(err, err_size, retval, "removing pre-existing file");
    return false;
  }

  ext2_ino_t newino;
  retval = ext2fs_new_inode(img->fs, parent_ino, (int)(LINUX_S_IFREG | (unix_mode & 07777)), 0, &newino);
  if (retval)
  {
    set_err_code(err, err_size, retval, "allocating new inode");
    return false;
  }
  ext2fs_inode_alloc_stats2(img->fs, newino, +1, 0);

  struct ext2_inode inode;
  memset(&inode, 0, sizeof(inode));
  inode.i_mode = (__u16)(LINUX_S_IFREG | (unix_mode & 07777));
  inode.i_links_count = 1;
  inode.i_atime = inode.i_ctime = inode.i_mtime = (__u32)time(NULL);
  retval = ext2fs_write_new_inode(img->fs, newino, &inode);
  if (retval)
  {
    set_err_code(err, err_size, retval, "writing new inode");
    return false;
  }

  retval = ext2fs_link(img->fs, parent_ino, base, newino, EXT2_FT_REG_FILE);
  if (retval == EXT2_ET_DIR_NO_SPACE)
  {
    retval = ext2fs_expand_dir(img->fs, parent_ino);
    if (retval)
    {
      set_err_code(err, err_size, retval, "expanding parent directory");
      return false;
    }
    retval = ext2fs_link(img->fs, parent_ino, base, newino, EXT2_FT_REG_FILE);
  }
  if (retval)
  {
    set_err_code(err, err_size, retval, "linking new file into parent directory");
    return false;
  }

  ext2_file_t file;
  retval = ext2fs_file_open(img->fs, newino, EXT2_FILE_WRITE, &file);
  if (retval)
  {
    set_err_code(err, err_size, retval, "opening new file for write");
    return false;
  }

  size_t total = 0;
  while (total < len)
  {
    unsigned int written = 0;
    retval = ext2fs_file_write(file, data + total, (unsigned int)(len - total), &written);
    if (retval || (written == 0 && total < len))
    {
      if (retval)
        set_err_code(err, err_size, retval, "writing file content");
      else
        set_err(err, err_size, "short write at %zu of %zu bytes", total, len);
      ext2fs_file_close(file);
      return false;
    }
    total += written;
  }

  retval = ext2fs_file_flush(file);
  if (retval)
  {
    set_err_code(err, err_size, retval, "flushing new file");
    ext2fs_file_close(file);
    return false;
  }
  retval = ext2fs_file_close(file);
  if (retval)
  {
    set_err_code(err, err_size, retval, "closing new file");
    return false;
  }
  return true;
}

typedef struct
{
  ext2_filsys fs;
  ext2_ino_t ino;
  bool ok;
  char* err;
  size_t err_size;
} VerifyCtx;

static int verify_block_cb(ext2_filsys fs, blk64_t* blocknr, e2_blkcnt_t blockcnt, blk64_t ref_blk, int ref_offset,
                            void* priv)
{
  (void)blockcnt;
  (void)ref_blk;
  (void)ref_offset;
  VerifyCtx* ctx = (VerifyCtx*)priv;
  if (!ext2fs_test_block_bitmap2(fs->block_map, *blocknr))
  {
    ctx->ok = false;
    set_err(ctx->err, ctx->err_size,
            "inode %u references block %llu that is NOT marked used in the block bitmap -- likely corrupted by a "
            "prior write/remove",
            ctx->ino, (unsigned long long)*blocknr);
    return BLOCK_ABORT;
  }
  return 0;
}

bool nam_ext4_verify_basic(Ext4Image* img, char* err, size_t err_size)
{
  ext2_filsys fs = img->fs;
  ext2_ino_t ino;
  struct ext2_inode inode;

  ext2_inode_scan scan;
  errcode_t retval = ext2fs_open_inode_scan(fs, 0, &scan);
  if (retval)
  {
    set_err_code(err, err_size, retval, "opening inode scan");
    return false;
  }

  bool ok = true;
  while (ok)
  {
    retval = ext2fs_get_next_inode(scan, &ino, &inode);
    if (retval)
    {
      set_err_code(err, err_size, retval, "scanning inodes");
      ok = false;
      break;
    }
    if (ino == 0)
      break; /* end of scan */
    if (inode.i_links_count == 0)
      continue; /* deleted/unused slot */
    if (!LINUX_S_ISREG(inode.i_mode) && !LINUX_S_ISDIR(inode.i_mode))
      continue; /* symlinks/devices/etc carry no ordinary data blocks to check here */

    VerifyCtx ctx = {fs, ino, true, err, err_size};
    /* A BLOCK_ABORT return from verify_block_cb stops iteration early with
     * retval==0 (not an error code) -- ctx.ok is what actually recorded the
     * problem, not retval. */
    retval = ext2fs_block_iterate3(fs, ino, BLOCK_FLAG_READ_ONLY, NULL, verify_block_cb, &ctx);
    if (retval)
    {
      set_err_code(err, err_size, retval, "iterating inode blocks");
      ok = false;
      break;
    }
    if (!ctx.ok)
    {
      ok = false;
      break;
    }
  }

  ext2fs_close_inode_scan(scan);
  return ok;
}
