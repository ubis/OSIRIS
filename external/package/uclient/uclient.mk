################################################################################
#
# uclient
#
################################################################################

UCLIENT_VERSION = daad21fa2c17cae54d37149bf27bb3dd78e50a3f
UCLIENT_SITE = https://git.openwrt.org/project/uclient.git
UCLIENT_SITE_METHOD = git
UCLIENT_LICENSE = ISC
UCLIENT_DEPENDENCIES = libubox ustream-ssl
UCLIENT_CONF_OPTS = -DBUILD_UCODE=OFF

$(eval $(cmake-package))
