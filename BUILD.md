Toolchain(built by buildroot) is expected at `./toolchain`

**Some setting:**
```bash
export TOOLCHAIN_DIR=$(readlink -f toolchain)
export PATH="$TOOLCHAIN_DIR:$PATH"
```

```sh
CC="arm-linux-gcc -flto=auto"
CXX="arm-linux-g++ -flto=auto"
AR=arm-linux-gcc-ar
RANLIB=arm-linux-gcc-ranlib
OPT_CFLAGS="-Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -Wno-deprecated-declarations"
OPT_CXXFLAGS="-Os -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables -std=gnu++14 -Wno-deprecated-declarations"
LFLAGS="-static -Wl,--gc-sections -Wl,--strip-all -Wl,--dynamic-linker=/lib/ld-uClibc.so.1 -flto=auto -Wl,--no-eh-frame-hdr"
```

**Full comamnd:**
```bash
export TOOLCHAIN_DIR=$(readlink -f toolchain) && export PATH="$TOOLCHAIN_DIR:$PATH" && \
make clean 2>&1 > /dev/null && \
make CC="arm-linux-gcc -flto=auto" CXX="arm-linux-g++ -flto=auto" \
  AR=arm-linux-gcc-ar RANLIB=arm-linux-gcc-ranlib \
  OPT_CFLAGS="-Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -Wno-deprecated-declarations" \
  OPT_CXXFLAGS="-Os -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables -std=gnu++14 -Wno-deprecated-declarations" \
  LFLAGS="-static -Wl,--gc-sections -Wl,--strip-all -Wl,--dynamic-linker=/lib/ld-uClibc.so.1 -flto=auto -Wl,--no-eh-frame-hdr" 2>&1
```
