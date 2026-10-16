# Xbox (XBE) build and test-fixture entry point. The host build uses CMake;
# see CONTRIBUTING.md.
#
#   make docker-xbe        Build the nxdk Docker image (cached) and the XBEs inside it
#                          (the app, the FATX probe in app/fatx-probe and the
#                          network test in app/net-test).
#   make xbe               Build the XBE directly; needs an activated nxdk environment.
#   make clean-xbe         Remove XBE build output.
#   make fixtures          Generate the synthetic xiso fixtures (needs Python 3).
#   make fixtures-verify   Generate them, then cross-check with extract-xiso in Docker.
#
# XBE output: app/bin/default.xbe and app/xiso-cartographer.iso (docs/build-xbox.md).
# Fixture output: $(FIXTURES_DIR) (tools/fixtures/README.md). Add the >4 GiB
# image with FIXTURES_ARGS=--large.

NXDK_IMAGE ?= xcat-nxdk
DOCKER ?= docker
PYTHON ?= python3
FIXTURES_DIR ?= build/fixtures
FIXTURES_ARGS ?=

# On Linux, build as the calling user so the output isn't owned by root.
ifeq ($(shell uname -s),Linux)
DOCKER_USER := --user $(shell id -u):$(shell id -g)
endif

.PHONY: xbe docker-image docker-xbe clean-xbe fixtures fixtures-verify

xbe:
	$(MAKE) -C app
	$(MAKE) -C app/fatx-probe
	$(MAKE) -C app/net-test

docker-image:
	$(DOCKER) build -f docker/nxdk.Dockerfile -t $(NXDK_IMAGE) \
		--build-context xcat-lwip=platform/nxdk/lwip third_party/nxdk

docker-xbe: docker-image
	$(DOCKER) run --rm $(DOCKER_USER) -v "$(CURDIR):/usr/src/app" $(NXDK_IMAGE) \
		sh -c 'make -C app && make -C app/fatx-probe && make -C app/net-test'

clean-xbe:
	$(MAKE) -C app clean
	$(MAKE) -C app/fatx-probe clean
	$(MAKE) -C app/net-test clean

fixtures:
	$(PYTHON) tools/fixtures/fixtures.py --out $(FIXTURES_DIR) $(FIXTURES_ARGS)

fixtures-verify: docker-image fixtures
	$(DOCKER) run --rm $(DOCKER_USER) -v "$(CURDIR):/usr/src/app" $(NXDK_IMAGE) \
		sh tools/fixtures/verify.sh $(FIXTURES_DIR)
