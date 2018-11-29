/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

/*
 * OTA API.
 *
 * See https://mongoose-os.com/docs/mos/userguide/ota.md for more details about
 * Mongoose OS OTA mechanism.
 */

#ifndef CS_FW_SRC_MGOS_UPDATER_H_
#define CS_FW_SRC_MGOS_UPDATER_H_

#include <stdbool.h>

#include "frozen.h"

#include "mgos_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MGOS_EVENT_OTA_BASE MGOS_EVENT_BASE('O', 'T', 'A')
enum mgos_event_ota {
  MGOS_EVENT_OTA_BEGIN =
      MGOS_EVENT_OTA_BASE, /* ev_data: struct mgos_upd_info */
  MGOS_EVENT_OTA_STATUS,   /* ev_data: struct mgos_ota_status */
};

struct mgos_upd_file_info {
  char name[50];
  uint32_t size;
  uint32_t processed;
};

struct mgos_upd_info {
  /* Data from the manifest, available from BEGIN until END */
  struct json_token name;
  struct json_token platform;
  struct json_token version;
  struct json_token build_id;
  struct json_token parts;
  bool abort; /* If MGOS_EVENT_OTA_BEGIN handler sets this to true, abort OTA */

  /* Current file, available in PROGRESS. */
  struct mgos_upd_file_info current_file;
};

enum mgos_ota_state {
  MGOS_OTA_STATE_IDLE = 0, /* idle */
  MGOS_OTA_STATE_PROGRESS, /* "progress" */
  MGOS_OTA_STATE_ERROR,    /* "error" */
  MGOS_OTA_STATE_SUCCESS,  /* "success" */
};

struct mgos_ota_status {
  bool is_committed;
  int commit_timeout;
  int partition;
  enum mgos_ota_state state;
  const char *msg;      /* stringified state */
  int progress_percent; /* valid only for "progress" state */
};

const char *mgos_ota_state_str(enum mgos_ota_state state);

void mgos_upd_boot_finish(bool is_successful, bool is_first);
bool mgos_upd_commit();
bool mgos_upd_is_committed();
bool mgos_upd_is_first_boot(void);
bool mgos_upd_revert(bool reboot);
bool mgos_upd_get_status(struct mgos_ota_status *);
/* Apply update on first boot, usually involves merging filesystem. */
int mgos_upd_apply_update(void);

int mgos_upd_get_commit_timeout();
bool mgos_upd_set_commit_timeout(int commit_timeout);

#ifdef __cplusplus
}
#endif

#endif /* CS_FW_SRC_MGOS_UPDATER_H_ */
