/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define TRACE_TAG ADB

#ifndef ADB_NOMMU

#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#include "adb.h"
#include "cutils/properties.h"
#include "sysdeps.h"

#if !ADB_NON_ANDROID
#include "ext4_sb.h"
#include "fec/io.h"
#include "fs_mgr.h"
#include "remount_service.h"

#define FSTAB_PREFIX "/fstab."
struct fstab* fstab;

#ifdef ALLOW_ADBD_DISABLE_VERITY
static const bool kAllowDisableVerity = true;
#else
static const bool kAllowDisableVerity = false;
#endif

/* Turn verity on/off */
static int set_verity_enabled_state(int wfd, const char* block_device, const char* mount_point,
                                    bool enable) {
    if (!make_block_device_writable(block_device)) {
        WriteFdFmt(wfd, "Could not make block device %s writable (%s).\n", block_device,
                   strerror(errno));
        return -1;
    }

    fec::io fh(block_device, O_RDWR);

    if (!fh) {
        WriteFdFmt(wfd, "Could not open block device %s (%s).\n", block_device, strerror(errno));
        WriteFdFmt(wfd, "Maybe run adb root?\n");
        return -1;
    }

    fec_verity_metadata metadata;

    if (!fh.get_verity_metadata(metadata)) {
        WriteFdFmt(wfd, "Couldn't find verity metadata!\n");
        return -1;
    }

    if (!enable && metadata.disabled) {
        WriteFdFmt(wfd, "Verity already disabled on %s\n", mount_point);
        return -1;
    }

    if (enable && !metadata.disabled) {
        WriteFdFmt(wfd, "Verity already enabled on %s\n", mount_point);
        return -1;
    }

    if (!fh.set_verity_status(enable)) {
        WriteFdFmt(wfd, "Could not set verity %s flag on device %s with error %s\n",
                   enable ? "enabled" : "disabled", block_device, strerror(errno));
        return -1;
    }

    WriteFdFmt(wfd, "Verity %s on %s\n", enable ? "enabled" : "disabled", mount_point);
    return 0;
}

void set_verity_enabled_state_service(adb_channel ch, void* cookie) {
    int wfd = ch.write_fd >= 0 ? ch.write_fd : ch.read_fd;
    bool enable = (cookie != NULL);
    if (kAllowDisableVerity) {
        char fstab_filename[PROPERTY_VALUE_MAX + sizeof(FSTAB_PREFIX)];
        char propbuf[PROPERTY_VALUE_MAX];
        int i;
        bool any_changed = false;

        property_get("ro.secure", propbuf, "0");
        if (strcmp(propbuf, "1")) {
            WriteFdFmt(wfd, "verity not enabled - ENG build\n");
            goto errout;
        }

        property_get("ro.debuggable", propbuf, "0");
        if (strcmp(propbuf, "1")) {
            WriteFdFmt(wfd, "verity cannot be disabled/enabled - USER build\n");
            goto errout;
        }

        property_get("ro.hardware", propbuf, "");
        snprintf(fstab_filename, sizeof(fstab_filename), FSTAB_PREFIX "%s", propbuf);

        fstab = fs_mgr_read_fstab(fstab_filename);
        if (!fstab) {
            WriteFdFmt(wfd, "Failed to open %s\nMaybe run adb root?\n", fstab_filename);
            goto errout;
        }

        /* Loop through entries looking for ones that vold manages */
        for (i = 0; i < fstab->num_entries; i++) {
            if (fs_mgr_is_verified(&fstab->recs[i])) {
                if (!set_verity_enabled_state(wfd, fstab->recs[i].blk_device,
                                              fstab->recs[i].mount_point, enable)) {
                    any_changed = true;
                }
            }
        }

        if (any_changed) {
            WriteFdFmt(wfd, "Now reboot your device for settings to take effect\n");
        }
    } else {
        WriteFdFmt(wfd, "%s-verity only works for userdebug builds\n",
                   enable ? "enable" : "disable");
    }

errout:
    adb_channel_close(&ch);
}
#else
void set_verity_enabled_state_service(adb_channel ch, void* cookie) {
    adb_channel_close(&ch);
}
#endif /* !ADB_NON_ANDROID */

#endif /* !ADB_NOMMU */
