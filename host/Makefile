# wolfctl (Linux) and the protocol tests
#
#   make            wolfctl
#   make test       the protocol against a simulated device, with faults

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu99
CPPFLAGS += -I. -I../common -I../firmware

all : wolfctl

SRC = wolfctl.c wkclient.c wkhid.c swiocmd.c dbgcmd.c chip.c flash.c

wolfctl : $(SRC) wkclient.h wkhid.h swiocmd.h dbgcmd.h chip.h flash.h ../common/wkproto.h ../firmware/swio_bits.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC)

test/proto_test : test/proto_test.c wkclient.c ../firmware/wkdev.c ../firmware/wkdev.h ../common/wkproto.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -DWK_HOST_TEST -pthread -o $@ \
		test/proto_test.c wkclient.c ../firmware/wkdev.c

test/swio_test : test/swio_test.c ../firmware/swio_bits.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test/swio_test.c

test/dbg_test : test/dbg_test.c test/chipsim.h ../firmware/ch32dbg.c ../firmware/ch32dbg.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itest -o $@ test/dbg_test.c ../firmware/ch32dbg.c

FLASH_TEST_SRC = test/flash_test.c flash.c chip.c wkclient.c ../firmware/wkdev.c \
	../firmware/dbgcmds.c ../firmware/ch32dbg.c

test/flash_test : $(FLASH_TEST_SRC) test/chipsim.h flash.h chip.h ../firmware/ch32dbg.h \
		../firmware/dbgcmds.h ../common/wkproto.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itest -DWK_HOST_TEST -o $@ $(FLASH_TEST_SRC)

test : test/proto_test test/swio_test test/dbg_test test/flash_test
	./test/swio_test
	./test/dbg_test
	./test/flash_test
	./test/proto_test 5000 0
	./test/proto_test 20000 5
	./test/proto_test 5000 30

clean :
	rm -f wolfctl test/proto_test test/swio_test test/dbg_test test/flash_test test/dbg_test

.PHONY : all test clean
