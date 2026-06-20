top_builddir ?= $(top_srcdir)
-include $(top_builddir)/config.mk

prefix ?= /usr
exec_prefix ?= $(prefix)
bindir ?= $(prefix)/bin
sbindir ?= $(exec_prefix)/sbin
datadir ?= $(prefix)/share
mandir ?= $(datadir)/man
sysconfdir ?= $(prefix)/etc
OPT_CFLAGS ?= -O2 -g
OPT_CXXFLAGS ?= -O2 -g
ADB_NOMMU ?= 0
CC ?= gcc
CXX ?= g++

# Internal helper: non-empty when building for noMMU Linux.
ifneq ($(ADB_NOMMU),0)
ADB_NOMMU_DEFINE = -DADB_NOMMU=1
endif
