# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=fritz-tools
PKG_RELEASE:=9
PKG_LICENSE:=GPL-2.0-only GPL-2.0-or-later
PKG_CONFIG_DEPENDS:=CONFIG_PACKAGE_fritz-fit
CMAKE_INSTALL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/fritz-tools/Default
  SECTION:=utils
  CATEGORY:=Utilities
endef

define Package/fritz-tffs
  $(call Package/fritz-tools/Default)
  TITLE:=Utility to partially read the TFFS filesystems
endef

define Package/fritz-tffs/description
 Utility to partially read the TFFS filesystems.
endef

define Package/fritz-tffs-nand
  $(call Package/fritz-tools/Default)
  TITLE:=Utility to partially read the TFFS filesystems on NAND flash
endef

define Package/fritz-tffs-nand/description
 Utility to partially read the TFFS filesystems on NAND flash.
endef

define Package/fritz-tffs3
  $(call Package/fritz-tools/Default)
  TITLE:=Read-only utility for TFFS 3.1 block storage
endef

define Package/fritz-fit
  $(call Package/fritz-tools/Default)
  TITLE:=AVM FIT inspection and read-only rootfs mapping
  DEPENDS:=+libfdt +zlib +kmod-loop
endef

define Package/fritzbox-5690-pro-tools
  $(call Package/fritz-tools/Default)
  TITLE:=FRITZ!Box 5690 Pro bring-up diagnostics
  DEPENDS:=+fritz-tffs3 +fritz-fit +ethtool
endef

define Package/fritzbox-5690-pro-firmware
  $(call Package/fritz-tools/Default)
  TITLE:=FRITZ!Box 5690 Pro firmware importer
  DEPENDS:=@TARGET_qualcommbe @BROKEN +fritz-fit +fritz-caldata +kmod-fs-squashfs +losetup
endef

define Package/fritzbox-5690-pro-firmware/description
  Extract modem and Wi-Fi firmware from a user-supplied FRITZ!OS 8.25 image.
  No vendor firmware is downloaded or included in this package.
endef

define Package/fritz-caldata
  $(call Package/fritz-tools/Default)
  DEPENDS:=+zlib
  TITLE:=Utility to extract WLAN calibration data
endef

define Package/fritz-caldata/description
 Utility to extract the zlib compress calibration data from flash.
endef

define Package/fritz-tffs/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fritz_tffs_read $(1)/usr/bin/fritz_tffs
endef

define Package/fritz-tffs-nand/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fritz_tffs_nand_read $(1)/usr/bin/fritz_tffs_nand
endef

define Package/fritz-caldata/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fritz_cal_extract $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fritz_caldata $(1)/usr/bin/
endef

define Package/fritz-tffs3/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fritz_tffs3 $(1)/usr/bin/
endef

define Package/fritz-fit/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fritz_fit $(1)/usr/bin/
endef

define Package/fritzbox-5690-pro-tools/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/fritzbox-5690-pro-diag $(1)/usr/sbin/
endef

define Package/fritzbox-5690-pro-firmware/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/fritzbox-5690-pro-firmware $(1)/usr/sbin/
endef

CMAKE_OPTIONS += \
	-DBUILD_FRITZ_FIT=$(if $(CONFIG_PACKAGE_fritz-fit),ON,OFF)

$(eval $(call BuildPackage,fritz-tffs))
$(eval $(call BuildPackage,fritz-tffs-nand))
$(eval $(call BuildPackage,fritz-tffs3))
$(eval $(call BuildPackage,fritz-fit))
$(eval $(call BuildPackage,fritzbox-5690-pro-tools))
$(eval $(call BuildPackage,fritzbox-5690-pro-firmware))
$(eval $(call BuildPackage,fritz-caldata))
