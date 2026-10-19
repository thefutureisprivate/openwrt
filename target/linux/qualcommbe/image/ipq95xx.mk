DTS_DIR := $(DTS_DIR)/qcom

define Build/avm-arm64-shim
	$(STAGING_DIR_HOST)/bin/python3 $(TOPDIR)/scripts/avm-arm64-shim.py \
		$@ $@.new --load $(KERNEL_LOADADDR) --limit $(DEVICE_DTS_LOADADDR)
	mv $@.new $@
endef

define Build/fit-avm-arm32
	$(call Build/fit-its,$(1))
	$(SED) 's/arch = "arm64";/arch = "arm";/g' \
		-e '/algo = "crc32";/a\value = <0>;' \
		-e '/hash-2/,+2d' \
		-e 's/compression = "none";/compression = "lzma";/g' $@.its
	$(STAGING_DIR_HOST)/bin/lzma e $(basename $(word 2,$(1))) \
		-lc3 -lp0 -pb2 $(word 2,$(1))
	$(call Build/fit-image,$(1))
	$(TOPDIR)/scripts/fit-add-avm-header.sh $@ > $@.new
	mv $@.new $@
endef

define Device/8devices_kiwi-dvk
	$(call Device/FitImage)
	$(call Device/EmmcImage)
	DEVICE_VENDOR := 8devices
	DEVICE_MODEL := Kiwi-DVK
	DEVICE_DTS_CONFIG := config@8dev-kiwi
	SOC := ipq9570
	DEVICE_PACKAGES := kmod-ath12k ath12k-firmware-qcn9274 \
		ipq-wifi-8devices_kiwi f2fsck mkf2fs kmod-sfp \
		kmod-phy-maxlinear kmod-phy-realtek rtl826x-firmware
	IMAGE/factory.bin := qsdk-ipq-factory-nor
endef
TARGET_DEVICES += 8devices_kiwi-dvk

define Device/askey_sbe1v1k
	$(call Device/FitImage)
	$(call Device/EmmcImage)
	DEVICE_VENDOR := Askey
	DEVICE_MODEL := SBE1V1K
	DEVICE_ALT0_VENDOR := Askey
	DEVICE_ALT0_MODEL := RTQ7300T
	DEVICE_ALT1_VENDOR := Spectrum
	DEVICE_ALT1_MODEL := SBE1V1K
	DEVICE_DTS_CONFIG := config@rtq7300t-rev0
	KERNEL_LOADADDR := 0x42200000
	SOC := ipq9570
	DEVICE_PACKAGES := ath12k-firmware-qcn9274 f2fsck ipq-wifi-askey_sbe1v1k kmod-ath12k \
		kmod-hwmon-pwmfan kmod-phy-realtek mkf2fs rtl826x-firmware
endef
TARGET_DEVICES += askey_sbe1v1k

define Device/avm_fritzbox-5690-pro-common
	$(call Device/FitImage)
	DEVICE_VENDOR := AVM
	DEVICE_MODEL := FRITZ!Box 5690 Pro
	SOC := ipq9574
	KERNEL_LOADADDR := 0x42008000
	DEVICE_DTS_LOADADDR := 0x48000000
	DEVICE_DTC_FLAGS := -@
	DEVICE_DTS_CONFIG := alder_HW279_config_2
	KERNEL = kernel-bin | avm-arm64-shim | lzma | \
		fit-avm-arm32 lzma $$(KDIR)/image-$$(DEVICE_DTS).dtb.lzma
	DEVICE_PACKAGES := vrx619-dsl fritzbox-5690-pro-firmware kmod-leds-pwm \
		kmod-ath11k-ahb kmod-ath12k \
		fritzbox-5690-pro-tools
	# RAM boot first; persistent slot selection still requires board validation.
	IMAGES :=
	BROKEN := y
endef

define Device/avm_fritzbox-5690-pro
	$(call Device/avm_fritzbox-5690-pro-common)
	DEVICE_DTS := ipq9574-avm-fritzbox-5690-pro
endef
TARGET_DEVICES += avm_fritzbox-5690-pro

define Device/avm_fritzbox-5690-pro-hw0
	$(call Device/avm_fritzbox-5690-pro-common)
	DEVICE_VARIANT := HWSubRevision 0
	DEVICE_DTS_CONFIG := alder_HW279_config_0
	DEVICE_DTS := ipq9574-avm-fritzbox-5690-pro-hw0
endef
TARGET_DEVICES += avm_fritzbox-5690-pro-hw0

define Device/linksys_ln6001
	$(call Device/FitImage)
	DEVICE_VENDOR := Linksys
	DEVICE_MODEL := LN6001
	DEVICE_DTS := ipq9554-linksys-ln6001
	SOC := ipq9554
	DEVICE_PACKAGES += bluetooth-uart kmod-leds-pwm kmod-fs-f2fs mkf2fs f2fsck
	IMAGE/sysupgrade.bin/squashfs := append-rootfs | pad-to 64k | \
		check-size 128m | sysupgrade-tar rootfs=$$$$@ | append-metadata
endef
TARGET_DEVICES += linksys_ln6001

define Device/qcom_rdp433
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Qualcomm Technologies, Inc.
	DEVICE_MODEL := RDP433
	DEVICE_VARIANT := AP-AL02-C4
	BOARD_NAME := ap-al02.1-c4
	DEVICE_DTS_CONFIG := config@rdp433
	DEVICE_DTS_DIR := $(DTS_DIR)
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := 6096k
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to 64k | append-rootfs | pad-rootfs | check-size | append-metadata
endef
TARGET_DEVICES += qcom_rdp433
