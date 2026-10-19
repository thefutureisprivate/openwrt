# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=vrx619_ep
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/vrx619_ep
  SUBMENU:=Network Devices
  TITLE:=VRX619 PCIe endpoint support
  DEPENDS:=@TARGET_qualcommbe @PCI_SUPPORT @BROKEN
  FILES:=$(PKG_BUILD_DIR)/vrx619_ep.ko
  AUTOLOAD:=$(call AutoProbe,vrx619_ep)
endef

define KernelPackage/vrx619_ep/description
  PCIe resource management for the VRX619 DSL endpoint. Modem control
  and packet transport require separate drivers. Hardware validation
  is pending.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,vrx619_ep))
