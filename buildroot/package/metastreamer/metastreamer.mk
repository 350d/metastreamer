################################################################################
#
# metastreamer
#
################################################################################

METASTREAMER_VERSION = local
METASTREAMER_SITE = $(BR2_EXTERNAL_METASTREAMER_PATH)/..
METASTREAMER_SITE_METHOD = local
METASTREAMER_LICENSE = GPL-3.0-or-later
METASTREAMER_LICENSE_FILES = LICENSE

define METASTREAMER_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/src CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define METASTREAMER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/src/metastreamer $(TARGET_DIR)/usr/bin/metastreamer
	for f in gadget reconnect wifi ntp-hook; do \
		$(INSTALL) -D -m 0755 $(@D)/scripts/metastreamer-$$f $(TARGET_DIR)/usr/bin/metastreamer-$$f || exit 1; \
	done
endef

$(eval $(generic-package))
