################################################################################
#
# usign
#
################################################################################

USIGN_VERSION = c4c72b1b07945ee192361dc751291a7c98d6adcd
USIGN_SITE = https://git.openwrt.org/project/usign.git
USIGN_SITE_METHOD = git
USIGN_LICENSE = ISC
USIGN_DEPENDENCIES = libubox
USIGN_CONF_OPTS = -DUSE_LIBUBOX=ON

$(eval $(cmake-package))
