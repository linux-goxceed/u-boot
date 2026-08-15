SDCC ?= sdcc
MAKEBIN ?= makebin

TARGET := gx6702-lpc
CFLAGS := -mmcs51 --model-small --iram-size 0x100 --xram-loc 0x0100 \
	--xram-size 0x0200 --code-size 0x2000 --out-fmt-ihx

.PHONY: all clean

all: $(TARGET).bin

$(TARGET).ihx: firmware.c mailbox.h
	$(SDCC) $(CFLAGS) -o $@ firmware.c

$(TARGET).bin: $(TARGET).ihx
	$(MAKEBIN) -p -s 0x2000 $< $@

clean:
	rm -f $(TARGET).asm $(TARGET).bin $(TARGET).ihx $(TARGET).lk \
		$(TARGET).lst $(TARGET).map $(TARGET).mem $(TARGET).rel \
		$(TARGET).rst $(TARGET).sym
