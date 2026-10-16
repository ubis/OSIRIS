################################################################################
#
# umdns
#
################################################################################

UMDNS_VERSION = defe7ccfd20281b9ef4dfa45437813386a64f8ba
UMDNS_SITE = https://git.openwrt.org/project/mdnsd.git
UMDNS_SITE_METHOD = git
UMDNS_LICENSE = LGPL-2.1
UMDNS_DEPENDENCIES = libubox ubus json-c udebug
UMDNS_CONF_OPTS = -DUNIT_TESTING=OFF

$(eval $(cmake-package))
