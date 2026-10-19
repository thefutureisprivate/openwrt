# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=vrx619-dsl
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DEPENDS:=vrx619_ep
PKG_BUILD_FLAGS:=gc-sections

include $(INCLUDE_DIR)/package.mk

define Package/vrx619-dsl
  SECTION:=net
  CATEGORY:=Network
  TITLE:=VRX619 DSL line controller
  DEPENDS:=+kmod-vrx619_dp +libubox +libubus @BROKEN
endef

define Package/vrx619-dsl/description
  Native VRX619 line configuration, activation, retraining and temperature
  recovery with ubus control. The host datapath supports VDSL PTM and ADSL ATM.
  Hardware validation is pending.
endef

define Package/vrx619-dsl/conffiles
/etc/config/vrx619-dsl
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/vrx619-dsl \
		$(PKG_BUILD_DIR)/main.c $(PKG_BUILD_DIR)/line.c \
		$(TARGET_LDFLAGS) -lubus -lubox
endef

define Package/vrx619-dsl/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/vrx619-dsl $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/vrx619-dsl.init $(1)/etc/init.d/vrx619-dsl
	$(INSTALL_CONF) ./files/vrx619-dsl.config $(1)/etc/config/vrx619-dsl
endef

$(eval $(call BuildPackage,vrx619-dsl))
