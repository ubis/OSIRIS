################################################################################
#
# shairport-sync
#
################################################################################

SHAIRPORT_SYNC_VERSION = 3.3.9
SHAIRPORT_SYNC_SITE = $(call github,mikebrady,shairport-sync,$(SHAIRPORT_SYNC_VERSION))
SHAIRPORT_SYNC_LICENSE = MIT, BSD-3c
SHAIRPORT_SYNC_LICENSE_FILES = LICENSES
SHAIRPORT_SYNC_DEPENDENCIES = openssl popt libconfig host-pkgconf
SHAIRPORT_SYNC_AUTORECONF = YES

# classic AirPlay only, into a pipe; tinysvcmdns instead of avahi
SHAIRPORT_SYNC_CONF_OPT = \
	--with-pipe \
	--with-tinysvcmdns \
	--with-ssl=openssl \
	--without-alsa

# it is all C, but automake links with the C++ driver, which this toolchain lacks
SHAIRPORT_SYNC_MAKE_OPT = CXXLD="$(TARGET_CC)" CXX="$(TARGET_CC)"

$(eval $(autotools-package))
