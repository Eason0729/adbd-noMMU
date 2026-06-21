/*
 * Copyright 2011, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h> /* for BLKGETSIZE */
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cutils/properties.h>

static int only_one_char(char *buf, int len, char c)
{
    int i, ret;

    ret = 1;
    for (i=0; i<len; i++) {
        if (buf[i] != c) {
            ret = 0;
            break;
        }
    }
    return ret;
}

int partition_wiped(char *source)
{
    int fd, ret;
#if defined(ADB_NOMMU)
    char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) return 0;
#else
    char buf[4096];
#endif

    if ((fd = open(source, O_RDONLY)) < 0) {
#if defined(ADB_NOMMU)
        munmap(buf, 4096);
#endif
        return 0;
    }

    ret = read(fd, buf, 4096);
    close(fd);

    if (ret != 4096) {
#if defined(ADB_NOMMU)
        munmap(buf, 4096);
#endif
        return 0;
    }

    /* Check for all zeros */
    if (only_one_char(buf, 4096, 0)) {
#if defined(ADB_NOMMU)
        munmap(buf, 4096);
#endif
       return 1;
    }

    /* Check for all ones */
    if (only_one_char(buf, 4096, 0xff)) {
#if defined(ADB_NOMMU)
        munmap(buf, 4096);
#endif
       return 1;
    }
#if defined(ADB_NOMMU)
    munmap(buf, 4096);
#endif

    return 0;
}

