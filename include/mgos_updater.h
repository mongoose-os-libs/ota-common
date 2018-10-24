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

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

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

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CS_FW_SRC_MGOS_UPDATER_H_ */
