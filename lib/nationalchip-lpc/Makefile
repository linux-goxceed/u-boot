SDCC ?= sdcc
MAKEBIN ?= makebin

CFLAGS := -mmcs51 --model-small --iram-size 0x100 --xram-loc 0x0100 \
	--xram-size 0x0200 --code-size 0x2000 --out-fmt-ihx

.PHONY: all clean gx6702-lpc.bin gx6706-lpc.bin

all: gx6702-lpc.bin gx6706-lpc.bin

build-gx6702/gx6702-lpc.ihx: firmware.c mailbox.h
	mkdir -p build-gx6702
	$(SDCC) $(CFLAGS) -o $@ firmware.c

build-gx6706/gx6706-lpc.ihx: firmware.c mailbox.h
	mkdir -p build-gx6706
	$(SDCC) $(CFLAGS) -DGX6706_LPC -o $@ firmware.c

gx6702-lpc.bin: build-gx6702/gx6702-lpc.ihx
	$(MAKEBIN) -p -s 0x2000 $< $@

gx6706-lpc.bin: build-gx6706/gx6706-lpc.ihx
	$(MAKEBIN) -p -s 0x2000 $< $@

clean:
	rm -rf build-gx6702 build-gx6706
	rm -f gx6702-lpc.bin gx6706-lpc.bin
