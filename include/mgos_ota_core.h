/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 *
 * Common bits of code handling update process.
 * Driven externaly by data source - mg_rpc or POST file upload.
 */

#pragma once

#include <stdbool.h>

#include "mgos_ota.h"
#include "mgos_ota_backend.h"
#include "mgos_ota_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create a snapshot of currently running firmware (including FS) in
 * a currently inactive slot. There must be no uncommitted update
 * in progress.
 * Returns slot id used for snapshot or < 0 in case of error.
 */
int mgos_ota_create_snapshot(void);

struct mgos_ota_boot_state {
  /* Slot that will be used to load firmware during next boot. */
  int active_slot;
  /* Whether the boot configuration is committed or not.
   * Reboot with uncommitted configration reverts to revert_slot. */
  bool is_committed;
  /* Slot that will be used in case of revert, explicit or implicit. */
  int revert_slot;
};
bool mgos_ota_boot_get_state(struct mgos_ota_boot_state *bs);
bool mgos_ota_boot_set_state(const struct mgos_ota_boot_state *bs);
/* Shortcuts for get and set */
void mgos_ota_boot_commit(void);
void mgos_ota_boot_revert(void);

#ifdef __cplusplus
}
#endif
