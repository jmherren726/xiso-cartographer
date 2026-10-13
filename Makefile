# Xbox (XBE) build entry point. The host build uses CMake; see CONTRIBUTING.md.
#
#   make docker-xbe    Build the nxdk Docker image (cached) and the XBE inside it.
#   make xbe           Build the XBE directly; needs an activated nxdk environment.
#   make clean-xbe     Remove XBE build output.
#
# Output: app/bin/default.xbe and app/xiso-cartographer.iso. See docs/build-xbox.md.

NXDK_IMAGE ?= xcat-nxdk
DOCKER ?= docker

# On Linux, build as the calling user so the output isn't owned by root.
ifeq ($(shell uname -s),Linux)
DOCKER_USER := --user $(shell id -u):$(shell id -g)
endif

.PHONY: xbe docker-image docker-xbe clean-xbe

xbe:
	$(MAKE) -C app

docker-image:
	$(DOCKER) build -f docker/nxdk.Dockerfile -t $(NXDK_IMAGE) third_party/nxdk

docker-xbe: docker-image
	$(DOCKER) run --rm $(DOCKER_USER) -v "$(CURDIR):/usr/src/app" $(NXDK_IMAGE) make -C app

clean-xbe:
	$(MAKE) -C app clean
