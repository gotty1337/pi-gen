SRC    := files/usb_gadget.c
CFLAGS := -O2 -Wall -Wextra

# Output directory for compiled binaries
OUTDIR := build

# Cross-compiler prefixes
CC_ARM64 := aarch64-linux-gnu-gcc
CC_ARMHF := arm-linux-gnueabihf-gcc
CC_NATIVE := gcc

.PHONY: all arm64 armhf native clean

## all: build for all targets (arm64, armhf, native)
all: arm64 armhf native

## arm64: cross-compile for aarch64 (Pi Zero 2W, 64-bit Raspberry Pi OS)
arm64: $(OUTDIR)/usb_gadget.arm64

## armhf: cross-compile for armhf (32-bit Raspberry Pi OS)
armhf: $(OUTDIR)/usb_gadget.armhf

## native: compile for the build host (quick sanity / syntax check)
native: $(OUTDIR)/usb_gadget.native

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(OUTDIR)/usb_gadget.arm64: $(SRC) | $(OUTDIR)
	$(CC_ARM64) $(CFLAGS) -o $@ $<
	file $@

$(OUTDIR)/usb_gadget.armhf: $(SRC) | $(OUTDIR)
	$(CC_ARMHF) $(CFLAGS) -o $@ $<
	file $@

$(OUTDIR)/usb_gadget.native: $(SRC) | $(OUTDIR)
	$(CC_NATIVE) $(CFLAGS) -o $@ $<
	file $@

## clean: remove build directory
clean:
	rm -rf $(OUTDIR)

help:
	@grep -E '^##' $(MAKEFILE_LIST) | sed 's/## /  /'
