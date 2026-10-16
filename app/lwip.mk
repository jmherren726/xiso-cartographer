# lwIP options for every XBE (platform/nxdk/lwip/lwipopts.h): our folder goes ahead of nxdk's
# in the include path. Include it in each app Makefile before nxdk's Makefile.
#
# nxdk's network library itself is prebuilt in the Docker image with the same folder first
# (docker/nxdk.Dockerfile, build context "xcat-lwip"), so the library and our code agree on
# the options and on lwIP's struct layouts. After changing lwipopts.h, rebuild the image:
# `make docker-xbe` does. Outside Docker, export NXDK_CFLAGS=-I<repo>/platform/nxdk/lwip
# while building nxdk's libraries.
NXDK_CFLAGS += -I$(ROOT_DIR)/platform/nxdk/lwip
