## Building for board

Toolchain(a symlink) is expected at `./toolchain`

```bash
export TOOLCHAIN_DIR=$(readlink -f toolchain) && export PATH="$TOOLCHAIN_DIR:$PATH" && \
make clean 2>&1 > /dev/null && \
make CC="arm-linux-gcc -flto=auto" CXX="arm-linux-g++ -flto=auto" \
  AR=arm-linux-gcc-ar RANLIB=arm-linux-gcc-ranlib \
  OPT_CFLAGS="-Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -Wno-deprecated-declarations" \
  OPT_CXXFLAGS="-Os -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables -std=gnu++14 -Wno-deprecated-declarations" \
  LFLAGS="-static -Wl,--gc-sections -Wl,--strip-all -Wl,--dynamic-linker=/lib/ld-uClibc.so.1 -flto=auto -Wl,--no-eh-frame-hdr" 2>&1
```

## Building for host

```bash
sudo dnf install -y valgrind.i686 libstdc++-devel.i686 libstdc++-static.i686 glibc-devel.i686 glibc-static.i686 libatomic.i686 libatomic-static.i686 heaptrack
```

```bash
make clean 2>&1 && \
make -j4 CC="gcc -m32 -flto=auto" CXX="g++ -m32 -flto=auto" \
  AR="gcc-ar" RANLIB="gcc-ranlib" \
  OPT_CFLAGS="-Os -g -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -Wno-deprecated-declarations" \
  OPT_CXXFLAGS="-Os -g -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables -std=gnu++14 -Wno-deprecated-declarations" \
  LFLAGS="-m32 -Wl,--gc-sections -Wl,-z,stack-size=16384 -flto=auto" \
  ADB_NOMMU=1 2>&1
```

## Profiling on host

### Phase tracking via /proc

```bash
pkill -9 adbd

heaptrack ./adb/adbd

adb connect 127.0.0.1:5555

adb -s 127.0.0.1:5555 shell id

echo test > /tmp/push_test
adb -s 127.0.0.1:5555 push /tmp/push_test /tmp/push_des

kill -15 $(pidof adbd)
```
