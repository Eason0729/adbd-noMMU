/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define TRACE_TAG AUTH

#include "sysdeps.h"
#include "adb_auth.h"

#include <resolv.h>
#include <stdio.h>
#include <string.h>

#if !defined(ADB_NOMMU)
#include <openssl/obj_mac.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <crypto_utils/android_pubkey.h>
#endif

#if defined(ADB_NOMMU) && !defined(ADB_NOMMU_NO_CRYPTO)
/* Simple base64 decode replacement for __b64_pton (BSD libc) on uClibc. */
static const int8_t adb_b64_tbl[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};
static int adb_b64_pton(const char *in, uint8_t *out, int outlen) {
    int val = 0, bits = 0, idx = 0;
    for (; *in; in++) {
        int8_t d = adb_b64_tbl[(unsigned char)*in];
        if (d == -1) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (idx >= outlen) return -1;
            out[idx++] = (val >> bits) & 0xFF;
        }
    }
    return idx;
}
#else
#define adb_b64_pton(in, out, len) __b64_pton(in, out, len)
#endif

#include "cutils/list.h"
#include "cutils/sockets.h"

#include "adb.h"
#include "fdevent.h"
#include "transport.h"

static fdevent listener_fde;
static fdevent framework_fde;
static int framework_fd = -1;

static void usb_disconnected(void* unused, atransport* t);
static const struct adisconnect usb_disconnect = { usb_disconnected, nullptr};
static atransport* usb_transport;
static bool needs_retry = false;

#if !defined(ADB_NOMMU)

struct adb_public_key {
    struct listnode node;
    RSA* key;
};

static const char *key_paths[] = {
    "/adb_keys",
    "/data/misc/adb/adb_keys",
    NULL
};

static void read_keys(const char *file, struct listnode *list)
{
    FILE *f;
    char buf[MAX_PAYLOAD_V1];
    char *sep;
    int ret;

    f = fopen(file, "re");
    if (!f) {
        D("Can't open '%s'", file);
        return;
    }

    while (fgets(buf, sizeof(buf), f)) {
        auto key = reinterpret_cast<adb_public_key*>(
            calloc(1, sizeof(adb_public_key)));
        if (key == nullptr) {
            D("Can't malloc key");
            break;
        }

        sep = strpbrk(buf, " \t");
        if (sep)
            *sep = '\0';

        uint8_t keybuf[ANDROID_PUBKEY_ENCODED_SIZE + 1];
        ret = adb_b64_pton(buf, keybuf, sizeof(keybuf));
        if (ret != ANDROID_PUBKEY_ENCODED_SIZE) {
            D("%s: Invalid base64 data ret=%d", file, ret);
            free(key);
            continue;
        }

        if (!android_pubkey_decode(keybuf, ret, &key->key)) {
            D("%s: Failed to parse key", file);
            free(key);
            continue;
        }

        list_add_tail(list, &key->node);
    }

    fclose(f);
}

static void free_keys(struct listnode *list)
{
    struct listnode *item;

    while (!list_empty(list)) {
        item = list_head(list);
        list_remove(item);
        adb_public_key* key = node_to_item(item, struct adb_public_key, node);
        RSA_free(key->key);
        free(key);
    }
}

static void load_keys(struct listnode *list)
{
    const char* path;
    const char** paths = key_paths;
    struct stat buf;

    list_init(list);

    while ((path = *paths++)) {
        if (!stat(path, &buf)) {
            D("Loading keys from '%s'", path);
            read_keys(path, list);
        }
    }
}

int adb_auth_verify(uint8_t* token, size_t token_size, uint8_t* sig, int siglen)
{
    struct listnode *item;
    struct listnode key_list;
    int ret = 0;

    load_keys(&key_list);

    list_for_each(item, &key_list) {
        adb_public_key* key = node_to_item(item, struct adb_public_key, node);
        ret = RSA_verify(NID_sha1, token, token_size, sig, siglen, key->key);
        if (ret)
            break;
    }

    free_keys(&key_list);

    return ret;
}

#else // ADB_NOMMU — no crypto, deny all verification

int adb_auth_verify(uint8_t* token, size_t token_size, uint8_t* sig, int siglen)
{
    (void)token; (void)token_size; (void)sig; (void)siglen;
    return 0;
}

#endif

int adb_auth_generate_token(void *token, size_t token_size)
{
    FILE *f;
    int ret;

    f = fopen("/dev/urandom", "re");
    if (!f)
        return 0;

    ret = fread(token, token_size, 1, f);

    fclose(f);
    return ret * token_size;
}

static void usb_disconnected(void* unused, atransport* t) {
    D("USB disconnect");
    usb_transport = NULL;
    needs_retry = false;
}

static void framework_disconnected() {
    D("Framework disconnect");
    fdevent_remove(&framework_fde);
    framework_fd = -1;
}

static void adb_auth_event(int fd, unsigned events, void*) {
    char response[2];
    int ret;

    if (events & FDE_READ) {
        ret = unix_read(fd, response, sizeof(response));
        if (ret <= 0) {
            framework_disconnected();
        } else if (ret == 2 && response[0] == 'O' && response[1] == 'K') {
            if (usb_transport) {
                adb_auth_verified(usb_transport);
            }
        }
    }
}

void adb_auth_confirm_key(unsigned char *key, size_t len, atransport *t)
{
    ScratchBuf _msg_buf(MAX_PAYLOAD_V1);
    if (!_msg_buf.valid()) {
        D("Scratch allocation failed");
        return;
    }
    char* msg = _msg_buf.get();
    int ret;

    if (!usb_transport) {
        usb_transport = t;
        t->AddDisconnect(const_cast<adisconnect*>(&usb_disconnect));
    }

    if (framework_fd < 0) {
        D("Client not connected");
        needs_retry = true;
        return;
    }

    if (key[len - 1] != '\0') {
        D("Key must be a null-terminated string");
        return;
    }

    ret = snprintf(msg, MAX_PAYLOAD_V1, "PK%s", key);
    if (ret >= MAX_PAYLOAD_V1) {
        D("Key too long. ret=%d", ret);
        return;
    }
    D("Sending '%s'", msg);

    ret = unix_write(framework_fd, msg, ret);
    if (ret < 0) {
        D("Failed to write PK, errno=%d", errno);
        return;
    }
}

static void adb_auth_listener(int fd, unsigned events, void* data) {
    sockaddr_storage addr;
    socklen_t alen;
    int s;

    alen = sizeof(addr);

    s = adb_socket_accept(fd, reinterpret_cast<sockaddr*>(&addr), &alen);
    if (s < 0) {
        D("Failed to accept: errno=%d", errno);
        return;
    }

    if (framework_fd >= 0) {
        LOG(WARNING) << "adb received framework auth socket connection again";
        framework_disconnected();
    }

    framework_fd = s;
    fdevent_install(&framework_fde, framework_fd, adb_auth_event, nullptr);
    fdevent_add(&framework_fde, FDE_READ);

    if (needs_retry) {
        needs_retry = false;
        send_auth_request(usb_transport);
    }
}

void adbd_cloexec_auth_socket() {
    int fd = android_get_control_socket("adbd");
    if (fd == -1) {
        D("Failed to get adbd socket");
        return;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

void adbd_auth_init(void) {
    int fd = android_get_control_socket("adbd");
    if (fd == -1) {
        D("Failed to get adbd socket");
        return;
    }

    if (listen(fd, 4) == -1) {
        D("Failed to listen on '%d'", fd);
        return;
    }

    fdevent_install(&listener_fde, fd, adb_auth_listener, NULL);
    fdevent_add(&listener_fde, FDE_READ);
}
