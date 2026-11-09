DEVKITPRO ?= C:/devkitPro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
CC := $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(DEVKITPRO)/tools/bin/elf2dol
LIBOGC := $(DEVKITPRO)/libogc
MACHDEP := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float
BUILD := build

.PHONY: all clean

all: $(BUILD)/wii_gx_smoke_gx_probe.dol

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/exi_init_override.o: exi_init_override.c | $(BUILD)
	$(CC) $(MACHDEP) -O2 -Wall -c $< -o $@

$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(MACHDEP) -O2 -Wall -I$(LIBOGC)/include -c $< -o $@

$(BUILD)/wii_gx_smoke.elf: $(BUILD)/exi_init_override.o $(BUILD)/main.o
	$(CC) $(MACHDEP) -Wl,--wrap=__exi_init -Wl,--wrap=EXI_Select \
		$^ -L$(LIBOGC)/lib/wii -logc -lm -o $@

$(BUILD)/wii_gx_smoke.dol: $(BUILD)/wii_gx_smoke.elf
	$(ELF2DOL) $< $@

$(BUILD)/wii_gx_smoke_gx_probe.dol: $(BUILD)/wii_gx_smoke.dol patch_dol.py
	python patch_dol.py $< $@

clean:
	rm -rf $(BUILD)
