# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=vrx619_ep
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/vrx619_ep
  SUBMENU:=Network Devices
  TITLE:=VRX619 PCIe endpoint support
  DEPENDS:=@TARGET_qualcommbe @PCI_SUPPORT @BROKEN
  FILES:=$(PKG_BUILD_DIR)/vrx619_ep.ko
  AUTOLOAD:=$(call AutoProbe,vrx619_ep)
endef

define KernelPackage/vrx619_pmi
  SUBMENU:=Network Devices
  TITLE:=VRX619 firmware and mailbox support
  DEPENDS:=+kmod-vrx619_ep @BROKEN
  KCONFIG:=CONFIG_FW_LOADER=y CONFIG_CRC32=y
  FILES:=$(PKG_BUILD_DIR)/vrx619_pmi.ko
  AUTOLOAD:=$(call AutoProbe,vrx619_pmi)
endef

define KernelPackage/vrx619_pmi/description
  Firmware loading, runtime paging and a userspace control interface for
  the VRX619 DSL endpoint. Requires vrx619/xcpe_fw.bin and a line controller.
  Hardware validation is pending.
endef

define KernelPackage/vrx619_dp
  SUBMENU:=Network Devices
  TITLE:=VRX619 PTM and ATM packet transport
  DEPENDS:=+kmod-vrx619_pmi +kmod-atm @BROKEN
  FILES:=$(PKG_BUILD_DIR)/vrx619_dp.ko
  AUTOLOAD:=$(call AutoProbe,vrx619_dp)
endef

define KernelPackage/vrx619_dp/description
  ACA firmware loading with PTM Ethernet and ATM AAL5 packet interfaces
  for the VRX619 DSL endpoint. Requires vrx619/aca_fw.bin and a separately
  configured DSL line. Hardware validation is pending.
endef

define KernelPackage/vrx619_ep/description
  PCIe resource management for the VRX619 DSL endpoint. Modem control
  and packet transport require separate drivers. Hardware validation
  is pending.
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/linux
	$(CP) $(PKG_BUILD_DIR)/include/uapi/linux/vrx619.h $(1)/usr/include/linux/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,vrx619_ep))
$(eval $(call KernelPackage,vrx619_pmi))
$(eval $(call KernelPackage,vrx619_dp))
