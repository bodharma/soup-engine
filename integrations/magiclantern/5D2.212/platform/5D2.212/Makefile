MODEL = 5D2
FW_VERSION = 212

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120
FIRMWARE_ID = 0x80000218
MAIN_FIRMWARE_ADDR = 0xFF810000
PLATFORM_ARCH = armv5te
ML_BOOT_OBJ = boot-d45.o
RESTARTSTART = 0x4E000
ML_PICSTYLE_AUTO = n

SOUP_ENGINE := $(TOP_DIR)/../../engine
ML_SRC_EXTRA_OBJS = $(BUILD_DIR)/afma.o $(BUILD_DIR)/asm.o \
    $(BUILD_DIR)/film_menu.o $(BUILD_DIR)/film_develop.o \
    $(BUILD_DIR)/soup_se.o $(BUILD_DIR)/soup_cr2.o \
    $(BUILD_DIR)/soup_jpeg_dec.o $(BUILD_DIR)/soup_jpeg_enc.o \
    $(BUILD_DIR)/soup_look.o $(BUILD_DIR)/soup_stock.o \
    $(BUILD_DIR)/soup_wb.o $(BUILD_DIR)/soup_color.o \
    $(BUILD_DIR)/soup_grade.o $(BUILD_DIR)/soup_lut.o

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete

$(BUILD_DIR)/soup_%.o: $(SOUP_ENGINE)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CDEPS_FLAGS) -I$(SOUP_ENGINE) -c -o $@ $<

$(BUILD_DIR)/film_develop.o: $(SRC_DIR)/film_develop.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CDEPS_FLAGS) -I$(SOUP_ENGINE) -c -o $@ $< $(INCLUDE_ARGS) $(LFLAGS)
