# Command-line build of the clock firmware with avr-gcc (no Arduino core
# needed: the sketch only uses avr-libc). The Arduino IDE + MiniCore build
# described in docs/firmware.md produces a functionally identical program.
#
#   make                          build build/binary_clock.hex
#   make VERBOSE_TICK=1 ...       override a build option of binary_clock.ino
#   make tools                    build the avr-libc bring-up sketches of tools/
#   make test                     run the decoder regression suite (PC)
#   make fuses PORT=/dev/ttyACM0  write the fuses through an Arduino as ISP
#   make flash PORT=/dev/ttyACM0  program the firmware through an Arduino as ISP

MCU      := atmega328p
F_CPU    := 8000000UL
BUILD    := build
SKETCH   := binary_clock/binary_clock.ino
TOOLS    := blink_test crystal_test
OPTIONS  := DCF77_ENABLED DCF77_OUT_ACTIVE_LOW DCF77_PD3_PULLUP DCF77_DEBUG_PULSES \
            DEBUG_UART_ENABLED VERBOSE_TICK BRIGHT_CURVE BRIGHT_MIN LIGHT_FILTER_SHIFT DBG_BIT_US
DEFINES  := $(foreach o,$(OPTIONS),$(if $($(o)),-D$(o)=$($(o))))

CXX      := avr-g++
OBJCOPY  := avr-objcopy
SIZE     := avr-size
CXXFLAGS := -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -std=gnu++11 -flto \
            -ffunction-sections -fdata-sections -Wall -Wextra $(DEFINES) $(EXTRA_CFLAGS)
LDFLAGS  := -Wl,--gc-sections

PORT     ?= /dev/ttyACM0
AVRDUDE  := avrdude -c stk500v1 -b 19200 -P $(PORT) -p m328p

all: $(BUILD)/binary_clock.hex

# Entry point normally provided by the Arduino core: setup() once, loop() forever.
$(BUILD)/main.cpp:
	@mkdir -p $(BUILD)
	@printf 'void setup(void);\nvoid loop(void);\nint main(void) { setup(); for (;;) loop(); }\n' > $@

$(BUILD)/binary_clock.elf: $(SKETCH) binary_clock/dcf77_decoder.h $(BUILD)/main.cpp FORCE
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -x c++ $(SKETCH) -x c++ $(BUILD)/main.cpp -o $@
	$(SIZE) $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

tools: $(TOOLS:%=$(BUILD)/%.hex)

$(BUILD)/blink_test.elf: tools/blink_test/blink_test.ino $(BUILD)/main.cpp
$(BUILD)/crystal_test.elf: tools/crystal_test/crystal_test.ino $(BUILD)/main.cpp
$(TOOLS:%=$(BUILD)/%.elf):
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -x c++ $< -x c++ $(BUILD)/main.cpp -o $@

test:
	$(MAKE) -C test test

fuses:
	$(AVRDUDE) -U lfuse:w:0xE2:m -U hfuse:w:0xD7:m -U efuse:w:0xFD:m

flash: $(BUILD)/binary_clock.hex
	$(AVRDUDE) -U flash:w:$<:i

clean:
	rm -rf $(BUILD)
	$(MAKE) -C test clean

FORCE:

.PHONY: all tools test fuses flash clean FORCE
.SECONDARY:
