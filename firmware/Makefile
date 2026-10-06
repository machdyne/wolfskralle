# Wolfskralle firmware (CH32V003)
#
#   make deps         clone ch32fun and rv003usb at the pinned versions
#   make              wolfskralle.bin (BOARD=v0 by default; BOARD=v1)
#   make flash        program it with a WCH-LinkE (minichlink) through J2
#   make DEBUG=1      printf over the Wolfskralle's own SWIO (J2 pin 5)

all : wolfskralle.bin

TARGET:=wolfskralle
TARGET_MCU:=CH32V003

CH32FUN_COMMIT:=6b247c7c10f1a96c1f0117edc47a695e9b0ab538
RV003USB_COMMIT:=5524a833c656a6007fca9f6c947fd49ddcc16eb6

CH32FUN:=ch32fun/ch32fun
RV003USB:=rv003usb

BOARD?=v0
ifeq ($(BOARD),v1)
EXTRA_CFLAGS+=-DBOARD_V1
endif
ifeq ($(DEBUG),1)
EXTRA_CFLAGS+=-DWK_DEBUG
endif

EXTRA_CFLAGS+=-I. -I../common -I$(RV003USB)/rv003usb -I$(RV003USB)/lib
ADDITIONAL_C_FILES+=wkdev.c swio.c ch32dbg.c dbgcmds.c $(RV003USB)/rv003usb/rv003usb.S $(RV003USB)/rv003usb/rv003usb.c

ifneq ($(wildcard $(CH32FUN)/ch32fun.mk),)
include $(CH32FUN)/ch32fun.mk
else
$(TARGET).bin $(TARGET).elf build flash :
	@echo "ch32fun is missing: run 'make deps' first"; false
endif

deps :
	@if [ ! -d ch32fun/.git ]; then \
		rm -rf ch32fun; git init -q ch32fun; \
		git -C ch32fun fetch -q --depth 1 https://github.com/cnlohr/ch32fun $(CH32FUN_COMMIT); \
		git -C ch32fun checkout -q FETCH_HEAD; fi
	@if [ ! -d rv003usb/.git ]; then \
		rm -rf rv003usb; git init -q rv003usb; \
		git -C rv003usb fetch -q --depth 1 https://github.com/cnlohr/rv003usb $(RV003USB_COMMIT); \
		git -C rv003usb checkout -q FETCH_HEAD; fi
	@echo "ch32fun  $$(git -C ch32fun rev-parse --short HEAD)"
	@echo "rv003usb $$(git -C rv003usb rev-parse --short HEAD)"

flash : cv_flash
clean : cv_clean

.PHONY : all deps flash clean
