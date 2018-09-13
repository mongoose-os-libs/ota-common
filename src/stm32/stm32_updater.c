/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>

#include "common/cs_crc32.h"
#include "common/cs_dbg.h"
#include "common/str_util.h"

#include "frozen.h"

#include "mgos_boot_cfg.h"
#include "mgos_vfs.h"
#include "mgos_vfs_dev.h"
#include "mgos_updater_hal.h"
#include "mgos_updater_util.h"

#include "stm32_vfs_dev_flash.h"

struct mgos_upd_hal_ctx {
  const char *status_msg;
  struct mgos_vfs_dev *app_dev, *fs_dev, *bl_dev;
  struct mg_str app_file_name, app_cs_sha1;
  unsigned int app_bl_size, app_bl_cfg_size, app_fs_size, update_bl;
  struct mg_str fs_file_name, fs_cs_sha1;
  uintptr_t app_org;
  size_t file_offset, app_bl_cfg_offset, app_fs_offset, app_app_offset;
  uint32_t app_len, app_crc32;
  struct mgos_vfs_dev *cur_dev;
  size_t cur_dev_num_erased;
  int8_t dst_slot;
};

struct mgos_upd_hal_ctx *mgos_upd_hal_ctx_create(void) {
  struct mgos_upd_hal_ctx *ctx =
      (struct mgos_upd_hal_ctx *) calloc(1, sizeof(*ctx));
  return ctx;
}

const char *mgos_upd_get_status_msg(struct mgos_upd_hal_ctx *ctx) {
  return ctx->status_msg;
}

int mgos_upd_begin(struct mgos_upd_hal_ctx *ctx, struct json_token *parts) {
  const struct mgos_boot_cfg *bcfg = mgos_boot_cfg_get();
  if (bcfg == NULL) return -1;
  struct json_token app_file_name = JSON_INVALID_TOKEN,
                    app_cs_sha1 = JSON_INVALID_TOKEN;
  struct json_token fs_file_name = JSON_INVALID_TOKEN,
                    fs_cs_sha1 = JSON_INVALID_TOKEN;
  json_scanf(parts->ptr, parts->len,
             "{app: {src: %T, cs_sha1: %T, bl_size: %u, bl_cfg_size: %u, "
             "       fs_size: %u, update_bl: %B}, "
             "fs: {src: %T, cs_sha1: %T}}",
             &app_file_name, &app_cs_sha1, &ctx->app_bl_size,
             &ctx->app_bl_cfg_size, &ctx->app_fs_size, &ctx->update_bl,
             &fs_file_name, &fs_cs_sha1);
  if (app_file_name.len == 0 || app_cs_sha1.len == 0 ||
      (ctx->update_bl && ctx->app_bl_size == 0)) {
    ctx->status_msg = "Incomplete update package";
    return -2;
  }
  ctx->app_file_name = mg_mk_str_n(app_file_name.ptr, app_file_name.len);
  ctx->app_cs_sha1 = mg_mk_str_n(app_cs_sha1.ptr, app_cs_sha1.len);
  ctx->fs_file_name = mg_mk_str_n(fs_file_name.ptr, fs_file_name.len);
  ctx->fs_cs_sha1 = mg_mk_str_n(fs_cs_sha1.ptr, fs_cs_sha1.len);
  ctx->app_bl_cfg_offset = ctx->app_bl_size;
  ctx->app_fs_offset = ctx->app_bl_cfg_offset + ctx->app_bl_cfg_size;
  ctx->app_app_offset = ctx->app_fs_offset + ctx->app_fs_size;
  ctx->app_org = FLASH_BASE + ctx->app_app_offset;
  /* Try to put into a directly bootable slot, if possible. */
  ctx->dst_slot =
      mgos_boot_cfg_find_slot(bcfg, ctx->app_org, true /* want_fs */, -1, -1);
  if (ctx->dst_slot < 0) {
    /* Ok, try any available slot, boot loader will perform a swap. */
    ctx->dst_slot = mgos_boot_cfg_find_slot(bcfg, 0 /* map_addr */,
                                            true /* want_fs */, -1, -1);
  }
  if (ctx->dst_slot < 0) {
    ctx->status_msg = "No slots available for update";
    return -3;
  }
  const struct mgos_boot_slot *sl = &bcfg->slots[ctx->dst_slot];
  LOG(LL_INFO, ("Picked slot %d, app -> %s, FS -> %s", ctx->dst_slot,
                sl->cfg.app_dev, sl->cfg.fs_dev));
  ctx->app_dev = mgos_vfs_dev_open(sl->cfg.app_dev);
  if (ctx->app_dev == NULL) {
    ctx->status_msg = "Failed to open app_dev";
    return -4;
  }
  ctx->fs_dev = mgos_vfs_dev_open(sl->cfg.fs_dev);
  if (ctx->fs_dev == NULL) {
    ctx->status_msg = "Failed to open fs_dev";
    return -5;
  }
  if (ctx->update_bl) {
    ctx->bl_dev = mgos_vfs_dev_create(MGOS_VFS_DEV_TYPE_STM32_FLASH, NULL);
    if (ctx->bl_dev == NULL ||
        !stm32_flash_dev_init(ctx->bl_dev, 0, ctx->app_bl_size,
                              false /* ese */)) {
      ctx->status_msg = "Failed to open bl_dev";
      return -6;
    }
  }
  LOG(LL_INFO, ("BL size %u (update? %d) + %u cfg; FS size %u; app org 0x%lx",
                ctx->app_bl_size, ctx->update_bl, ctx->app_bl_cfg_size,
                ctx->app_fs_size, (unsigned long) ctx->app_org));
  /* To simplify logic while writing. */
  if (ctx->app_bl_size % MGOS_UPDATER_DATA_CHUNK_SIZE != 0 ||
      ctx->app_bl_cfg_size % MGOS_UPDATER_DATA_CHUNK_SIZE != 0 ||
      ctx->app_fs_size % MGOS_UPDATER_DATA_CHUNK_SIZE != 0) {
    ctx->status_msg = "Invalid size";
    return -7;
  }
  return 1;
}

enum mgos_upd_file_action mgos_upd_file_begin(
    struct mgos_upd_hal_ctx *ctx, const struct mgos_upd_file_info *fi) {
  enum mgos_upd_file_action res = MGOS_UPDATER_SKIP_FILE;
  ctx->file_offset = 0;
  if (mg_vcmp(&ctx->app_file_name, fi->name) == 0) {
    if (fi->size < ctx->app_app_offset) {
      ctx->status_msg = "App file too short";
      res = MGOS_UPDATER_ABORT;
      goto out;
    }
    ctx->app_len = fi->size - ctx->app_app_offset;
    res = MGOS_UPDATER_PROCESS_FILE;
  } else if (mg_vcmp(&ctx->fs_file_name, fi->name) == 0) {
    res = MGOS_UPDATER_PROCESS_FILE;
  }
out:
  return res;
}

int mgos_upd_file_data(struct mgos_upd_hal_ctx *ctx,
                       const struct mgos_upd_file_info *fi,
                       struct mg_str data) {
  int res = -1;
  size_t write_offset = 0;
  struct mgos_vfs_dev *dev = NULL;
  if (mg_vcmp(&ctx->app_file_name, fi->name) == 0) {
    if (ctx->file_offset < ctx->app_bl_size) {
      /* Boot loader. Write if instructed. */
      if (ctx->update_bl) {
        dev = ctx->bl_dev;
        write_offset = ctx->file_offset;
      }
    } else if (ctx->file_offset < ctx->app_fs_offset) {
      /* Boot loader config is never updated during OTA. */
    } else if (ctx->file_offset < ctx->app_app_offset) {
      /* FS image - write unless we have a separate file.
       * Generally speaking it should be one or the other, not both. */
      if (ctx->fs_file_name.len == 0) {
        dev = ctx->fs_dev;
        write_offset = ctx->file_offset - ctx->app_fs_offset;
      }
    } else {
      /* This is app. */
      dev = ctx->app_dev;
      write_offset = ctx->file_offset - ctx->app_app_offset;
      ctx->app_crc32 =
          cs_crc32(ctx->app_crc32, (const uint8_t *) data.p, data.len);
    }
  } else if (mg_vcmp(&ctx->fs_file_name, fi->name) == 0) {
    dev = ctx->fs_dev;
  }
  if (dev != ctx->cur_dev) {
    ctx->cur_dev = dev;
    ctx->cur_dev_num_erased = 0;
  }
  LOG(LL_DEBUG,
      ("fn %s ds %d fo %d | %d %d %d | dev %s wo %d", fi->name, (int) data.len,
       (int) ctx->file_offset, (int) ctx->app_bl_size, (int) ctx->app_fs_offset,
       (int) ctx->app_app_offset, (dev ? dev->name : "-"), (int) write_offset));
  if (dev != NULL) {
    /* See if we need to erase before writing. */
    size_t write_end = write_offset + data.len;
    if (write_end > ctx->cur_dev_num_erased) {
      size_t erase_len = 0;
      size_t erase_sizes[MGOS_VFS_DEV_NUM_ERASE_SIZES];
      size_t dev_size = mgos_vfs_dev_get_size(dev);
      size_t headroom = dev_size - ctx->cur_dev_num_erased;
      if (mgos_vfs_dev_get_erase_sizes(dev, erase_sizes) == 0) {
        erase_len = write_end - ctx->cur_dev_num_erased;
        /* Use the largest erase size smaller than the remaining space ahead. */
        for (int i = 0; i < (int) ARRAY_SIZE(erase_sizes); i++) {
          if (erase_sizes[i] > 0 && erase_sizes[i] < headroom) {
            erase_len = erase_sizes[i];
          } else {
            break;
          }
        }
      } else {
        /* Just nuke the whole thing */
        erase_len = headroom;
      }
      LOG(LL_DEBUG, ("Erase %s %d @ 0x%lx", dev->name, (int) erase_len,
                     (unsigned long) ctx->cur_dev_num_erased));
      enum mgos_vfs_dev_err eres =
          mgos_vfs_dev_erase(dev, ctx->cur_dev_num_erased, erase_len);
      if (eres != 0) {
        ctx->status_msg = "Erase failed";
        LOG(LL_INFO,
            ("%s: erase %d failed: %d", dev->name, (int) erase_len, eres));
        goto out;
      }
      ctx->cur_dev_num_erased += erase_len;
    }
    enum mgos_vfs_dev_err wres =
        mgos_vfs_dev_write(dev, write_offset, data.len, data.p);
    if (wres < 0) {
      LOG(LL_ERROR,
          ("%s @ %d => wr %s: %d @ %d = %d", fi->name, (int) ctx->file_offset,
           dev->name, (int) data.len, (int) write_offset, wres));
      res = wres;
    } else {
      res = (int) data.len;
    }
  } else {
    res = (int) data.len; /* Skip */
  }
out:
  ctx->file_offset += data.len;
  return res;
}

int mgos_upd_file_end(struct mgos_upd_hal_ctx *ctx,
                      const struct mgos_upd_file_info *fi, struct mg_str tail) {
  int res = -1;
  if (tail.len > 0) {
    int wres = mgos_upd_file_data(ctx, fi, tail);
    if (wres != (int) tail.len) {
      res = wres;
      goto out;
    }
  }
  /* TODO(rojer): Verify SHA1. */
  res = (int) tail.len;
out:
  return res;
}

int mgos_upd_finalize(struct mgos_upd_hal_ctx *ctx) {
  int res = -1;
  struct mgos_boot_cfg *bcfg = mgos_boot_cfg_get();
  struct mgos_boot_slot_state *ss;
  if (bcfg == NULL) goto out;
  bcfg->revert_slot = bcfg->active_slot;
  bcfg->active_slot = ctx->dst_slot;
  bcfg->flags &= ~(MGOS_BOOT_F_COMMITTED);
  bcfg->flags |= (MGOS_BOOT_F_FIRST_BOOT_A | MGOS_BOOT_F_FIRST_BOOT_B);
  bcfg->flags |= (MGOS_BOOT_F_MERGE_FS);
  ss = &bcfg->slots[ctx->dst_slot].state;
  ss->app_len = ctx->app_len;
  ss->app_org = ctx->app_org;
  ss->app_crc32 = ctx->app_crc32;
  ss->app_flags = 0;
  LOG(LL_INFO, ("Updating boot config"));
  res = (mgos_boot_cfg_write(bcfg, true /* dump */) ? 1 : -1);

out:
  return res;
}

void mgos_upd_hal_ctx_free(struct mgos_upd_hal_ctx *ctx) {
  if (ctx == NULL) return;
  mgos_vfs_dev_close(ctx->app_dev);
  mgos_vfs_dev_close(ctx->fs_dev);
  mgos_vfs_dev_close(ctx->bl_dev);
  free(ctx);
}

int mgos_upd_create_snapshot() {
  /* TODO */
  return -1;
}

bool mgos_upd_boot_get_state(struct mgos_upd_boot_state *bs) {
  const struct mgos_boot_cfg *bcfg = mgos_boot_cfg_get();
  if (bcfg == NULL) return false;
  memset(bs, 0, sizeof(*bs));
  bs->active_slot = bcfg->active_slot;
  bs->revert_slot = bcfg->revert_slot;
  bs->is_committed = !!(bcfg->flags & MGOS_BOOT_F_COMMITTED);
  return true;
}

bool mgos_upd_boot_set_state(const struct mgos_upd_boot_state *bs) {
  struct mgos_boot_cfg *bcfg = mgos_boot_cfg_get();
  if (bcfg == NULL) return false;
  bcfg->active_slot = bs->active_slot;
  bcfg->revert_slot = bs->revert_slot;
  if (bs->is_committed) {
    bcfg->flags |= MGOS_BOOT_F_COMMITTED;
  } else {
    bcfg->flags &= ~(MGOS_BOOT_F_COMMITTED);
  }
  return mgos_boot_cfg_write(bcfg, true /* dump */);
}

int mgos_upd_apply_update(void) {
  int res = -1;
  struct mgos_vfs_dev *old_fs_dev = NULL;
  const struct mgos_boot_cfg *bcfg = mgos_boot_cfg_get();
  if (bcfg == NULL) goto out;
  if (bcfg->revert_slot < 0) {
    LOG(LL_ERROR, ("Revert slot not set!"));
    goto out;
  }
  if (!mgos_vfs_mount_dev_name("/old",
                               bcfg->slots[bcfg->revert_slot].cfg.fs_dev,
                               CS_STRINGIFY_MACRO(MGOS_ROOT_FS_TYPE),
                               CS_STRINGIFY_MACRO(MGOS_ROOT_FS_OPTS))) {
    LOG(LL_ERROR, ("Failed to mount old file system"));
    goto out;
  }
  res = (mgos_upd_merge_fs("/old", "/") ? 0 : -2);
  mgos_vfs_umount("/old");
out:
  mgos_vfs_dev_close(old_fs_dev);
  return res;
}

static bool mgos_boot_commit_slot(struct mgos_boot_cfg *bcfg, int8_t slot) {
  bcfg->active_slot = slot;
  bcfg->revert_slot = -1;
  bcfg->flags |= MGOS_BOOT_F_COMMITTED;
  bcfg->flags &= ~(MGOS_BOOT_F_FIRST_BOOT_A | MGOS_BOOT_F_MERGE_FS);
  return (mgos_boot_cfg_write(bcfg, false /* dump */));
}

void mgos_upd_boot_commit(void) {
  struct mgos_boot_cfg *bcfg = mgos_boot_cfg_get();
  if (bcfg == NULL) return;
  int8_t active_slot = bcfg->active_slot;
  if (mgos_boot_commit_slot(bcfg, active_slot)) {
    LOG(LL_INFO, ("Committed slot %d", active_slot));
  }
}

void mgos_upd_boot_revert(void) {
  struct mgos_boot_cfg *bcfg = mgos_boot_cfg_get();
  if (bcfg == NULL) return;
  int8_t revert_slot = bcfg->revert_slot;
  if (mgos_boot_commit_slot(bcfg, revert_slot)) {
    LOG(LL_INFO, ("Reverted to slot %d", revert_slot));
  }
}

bool mgos_upd_is_first_boot(void) {
  struct mgos_boot_cfg *bcfg = mgos_boot_cfg_get();
  if (bcfg == NULL) return false;
  return (bcfg->flags & MGOS_BOOT_F_FIRST_BOOT_A) != 0;
}
