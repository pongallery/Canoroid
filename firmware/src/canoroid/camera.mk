# SPDX-License-Identifier: GPL-2.0-only

$(BUILD_DIR)/canoroid-camera.o $(BUILD_DIR)/gui.o $(BUILD_DIR)/gui-common.o $(BUILD_DIR)/zebra.o $(BUILD_DIR)/histogram.o $(ML_LD_SCRIPT): CFLAGS += -DCONFIG_CANOROID=1

$(CR_JPEG_DIR)/jconfig.h: $(TOP_DIR)/tools/canoroid/fetch-jpeg.sh $(TOP_DIR)/tools/canoroid/small-idct.patch | $(BUILD_DIR)
	sh $< $(BUILD_DIR)

$(BUILD_DIR)/canoroid-print.o: $(SRC_DIR)/canoroid/print.c $(SRC_DIR)/canoroid/print.h $(CR_JPEG_DIR)/jconfig.h
	$(CC) $(CFLAGS) $(INCLUDE_ARGS) -I$(CR_JPEG_DIR) -c $< -o $@

$(BUILD_DIR)/canoroid-camera.o: $(SRC_DIR)/canoroid/camera.c $(SRC_DIR)/canoroid/print.h $(SRC_DIR)/canoroid/ui.h $(SRC_DIR)/canoroid/dinoframe.h $(TOP_DIR)/VERSION | $(BUILD_DIR)
	$(CC) $(CFLAGS) -mthumb $(INCLUDE_ARGS) -DCANOROID_VERSION='"$(strip $(shell cat $(TOP_DIR)/VERSION))"' -c $< -o $@

# Compile the dependency with its own flags to keep ML-specific preprocessor
# settings and warnings out of the IJG codec.
# Use Thumb code for the marker parser to fit the fixed boot reservation.
$(BUILD_DIR)/cr-jpeg-jdmarker.o $(BUILD_DIR)/cr-jpeg-jcmarker.o $(BUILD_DIR)/cr-jpeg-jdmaster.o $(BUILD_DIR)/cr-jpeg-jcparam.o $(BUILD_DIR)/cr-jpeg-jdinput.o $(BUILD_DIR)/cr-jpeg-jcapimin.o $(BUILD_DIR)/cr-jpeg-jdapimin.o $(BUILD_DIR)/cr-jpeg-jcmaster.o $(BUILD_DIR)/cr-jpeg-jcomapi.o $(BUILD_DIR)/cr-jpeg-jmemmgr.o: CR_JPEG_CFLAGS = -mthumb
$(BUILD_DIR)/cr-jpeg-jdmarker.o $(BUILD_DIR)/cr-jpeg-jcmarker.o $(BUILD_DIR)/cr-jpeg-jdmaster.o $(BUILD_DIR)/cr-jpeg-jcparam.o $(BUILD_DIR)/cr-jpeg-jdinput.o $(BUILD_DIR)/cr-jpeg-jcapimin.o $(BUILD_DIR)/cr-jpeg-jdapimin.o $(BUILD_DIR)/cr-jpeg-jcmaster.o $(BUILD_DIR)/cr-jpeg-jcomapi.o $(BUILD_DIR)/cr-jpeg-jmemmgr.o: $(TOP_DIR)/src/canoroid/camera.mk
$(BUILD_DIR)/cr-jpeg-jmemnobs.o: CR_JPEG_CFLAGS = -Dmalloc=cr_jpeg_alloc -Dfree=cr_jpeg_free
$(BUILD_DIR)/cr-jpeg-%.o: $(CR_JPEG_DIR)/jconfig.h
	$(CC) -Os -march=$(PLATFORM_ARCH) -ffreestanding -DNO_GETENV \
		-I$(TOP_DIR)/include -I$(CR_JPEG_DIR) $(CR_JPEG_CFLAGS) -c $(CR_JPEG_DIR)/$*.c -o $@

zip_contents:: $(CR_JPEG_DIR)/jconfig.h $(ZIP_DIR)
	$(CP) $(CR_JPEG_DIR)/README $(ZIP_DIR)/ML/CANOROID-IJG.txt

.PHONY: canoroid-packages
canoroid-packages: $(BUILD_DIR)/autoexec.bin
	$(PYTHON3) $(TOP_DIR)/tools/canoroid/pack.py $(TOP_DIR) $(BUILD_DIR)
