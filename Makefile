#!/usr/bin/make -f
OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer\
								 -O3 -fno-finite-math-only
PREFIX ?= /usr/local
CFLAGS ?= $(OPTIMIZATIONS) -Wall
LV2DIR ?= $(PREFIX)/lib/lv2

STRIP?=strip
STRIPFLAGS?=-s

harmonizer_VERSION?=$(shell git describe --tags HEAD 2>/dev/null\
									 	| sed 's/-g.*$$//;s/^v//' || echo "LV2")
###############################################################################
LIB_EXT=.so
BUILDDIR=build/

LOADLIBES=-lm
LV2NAME=harmonizer
BUNDLE=harmonizer.lv2
targets=
SRCS =

.SUFFIXES:

.SUFFIXES: .cpp

UNAME=$(shell uname)
ifeq ($(UNAME),Darwin)
  LV2LDFLAGS=-dynamiclib
  LIB_EXT=.dylib
  EXTENDED_RE=-E
  STRIPFLAGS=-u -r -arch all -s lv2syms
  targets+=lv2syms
else
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic
  LIB_EXT=.so
  EXTENDED_RE=-r
endif

ifneq ($(XWIN),)
  CC=$(XWIN)-gcc
  STRIP=$(XWIN)-strip
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic -Wl,--as-needed
  LIB_EXT=.dll
  override LDFLAGS += -static-libgcc -static-libstdc++
endif

targets+=$(BUILDDIR)$(LV2NAME)$(LIB_EXT)

ifneq ($(MOD),)
  targets+=$(BUILDDIR)modgui
  MODLABEL=mod:label \"Harmonizer\";
  MODBRAND=mod:brand \"Daniel Sheeler\";
  MODGUILABEL=modgui:label \"Harmonizer\";
  MODGUIBRAND=modgui:brand \"Daniel Sheeler\";
else
  MODLABEL=
  MODBRAND=
endif

###############################################################################
# extract versions
LV2VERSION=$(harmonizer_VERSION)
include git2lv2.mk

# check for build-dependencies
ifeq ($(shell pkg-config --exists lv2 || echo no), no)
  $(error "LV2 SDK was not found")
endif

override CFLAGS += -fPIC
override CFLAGS += `pkg-config --cflags lv2`
#override CFLAGS += `pkg-config --cflags aubio`

#override LDFLAGS += `pkg-config --libs aubio`

# build target definitions
default: all

all: aubio $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl $(targets)

lv2syms:
	echo "_lv2_descriptor" > lv2syms

$(BUILDDIR)manifest.ttl: lv2ttl/manifest.ttl.in Makefile
	@mkdir -p $(BUILDDIR)
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LIB_EXT@/$(LIB_EXT)/" \
	  lv2ttl/manifest.ttl.in > $(BUILDDIR)manifest.ttl
ifneq ($(MOD),)
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@URISUFFIX@/$(URISUFFIX)/;s/@MODBRAND@/$(MODGUIBRAND)/;s/@MODLABEL@/$(MODGUILABEL)/" \
		lv2ttl/manifest.modgui.in >> $(BUILDDIR)manifest.ttl
endif

$(BUILDDIR)$(LV2NAME).ttl: lv2ttl/$(LV2NAME).ttl.in Makefile
	@mkdir -p $(BUILDDIR)
	sed "s/@VERSION@/lv2:microVersion $(LV2MIC) ;lv2:minorVersion $(LV2MIN) ;/g" \
		lv2ttl/$(LV2NAME).ttl.in > $(BUILDDIR)$(LV2NAME).ttl

AUBIO_SRCS = $(BUILDDIR)mathutils.c $(BUILDDIR)fvec.c $(BUILDDIR)onset.c $(BUILDDIR)peakpicker.c $(BUILDDIR)biquad.c $(BUILDDIR)filter.c $(BUILDDIR)lvec.c \
						 $(BUILDDIR)specdesc.c $(BUILDDIR)statistics.c $(BUILDDIR)hist.c $(BUILDDIR)scale.c $(BUILDDIR)cvec.c $(BUILDDIR)pitch.c \
						 $(BUILDDIR)pitchyinfft.c $(BUILDDIR)pitchyin.c $(BUILDDIR)pitchspecacf.c $(BUILDDIR)pitchfcomb.c \
						 $(BUILDDIR)pitchmcomb.c $(BUILDDIR)pitchschmitt.c $(BUILDDIR)fft.c $(BUILDDIR)ooura_fft8g.c $(BUILDDIR)c_weighting.c \
						 $(BUILDDIR)phasevoc.c
AUBIO_OBJS= $(AUBIO_SRCS:.c=.o)
.SUFFIXES:

.SUFFIXES: .c

aubio: aubio_init

aubio_init:
	@mkdir -p $(BUILDDIR)
	cp -r aubio/* $(BUILDDIR)

%.o : %.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -I $(BUILDDIR) -c \
	$< -o $@

$(BUILDDIR)$(LV2NAME)$(LIB_EXT): $(LV2NAME).cpp $(AUBIO_OBJS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
	  -o $(BUILDDIR)$(LV2NAME)$(LIB_EXT) $(LV2NAME).cpp \
		-shared $(LV2LDFLAGS) $(LDFLAGS) $(LOADLIBES) \
		$(AUBIO_OBJS)
	$(STRIP) $(STRIPFLAGS) $(BUILDDIR)$(LV2NAME)$(LIB_EXT)

$(BUILDDIR)modgui: $(BUILDDIR)$(LV2NAME).ttl
	@mkdir -p $(BUILDDIR)/modgui
	cp -r modgui/* $(BUILDDIR)modgui/

# install/uninstall/clean target definitions

install: all
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m755 $(BUILDDIR)$(LV2NAME)$(LIB_EXT) $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m644 $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl $(DESTDIR)$(LV2DIR)/$(BUNDLE)
ifneq ($(MOD),)
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)/modgui
	install -t $(DESTDIR)$(LV2DIR)/$(BUNDLE)/modgui $(BUILDDIR)modgui/*
endif

uninstall:
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/manifest.ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME).ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME)$(LIB_EXT)
	rm -rf $(DESTDIR)$(LV2DIR)/$(BUNDLE)/modgui
	-rmdir $(DESTDIR)$(LV2DIR)/$(BUNDLE)

clean:
	rm -f $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl \
		$(BUILDDIR)/*.o  $(BUILDDIR)$(LV2NAME)$(LIB_EXT) lv2syms
	rm -rf $(BUILDDIR)modgui
	-test -d $(BUILDDIR) && rmdir $(BUILDDIR) || true

.PHONY: clean all install uninstall
