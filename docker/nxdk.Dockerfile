# nxdk build image for xiso-cartographer.
#
# Mirrors third_party/nxdk/Dockerfile, but pins the base images by digest and
# builds nxdk from our pinned submodule, so the toolchain always matches the
# commit recorded in this repo.
#
# Build context must be the nxdk submodule, plus our lwIP options as the named context
# "xcat-lwip" (platform/nxdk/lwip, see its lwipopts.h):
#   docker build -f docker/nxdk.Dockerfile -t xcat-nxdk \
#       --build-context xcat-lwip=platform/nxdk/lwip third_party/nxdk
# Used by `make docker-xbe` and CI. See docs/build-xbox.md.

# ghcr.io/xboxdev/nxdk-buildbase:git-3a71ed5f
FROM ghcr.io/xboxdev/nxdk-buildbase@sha256:b14cd17f59449c0c3bdc86dc04dde411cdd0eaaeb3539fbe85a598dec3846f52 AS builder

COPY ./ /usr/src/nxdk/
ENV NXDK_DIR=/usr/src/nxdk
RUN cd /usr/src/nxdk && make tools -j"$(nproc)"
# Prebuild the nxdk libraries (libc, lwIP, SDL, ...) so app builds only compile our code.
# lwIP gets our options (TCP receive tuning): our lwipopts.h comes first in the include path
# and includes nxdk's. NXDK_CFLAGS from the environment is kept; nxdk's Makefiles append to it.
COPY --from=xcat-lwip lwipopts.h /usr/src/xcat-lwip/lwipopts.h
RUN eval "$(/usr/src/nxdk/bin/activate -s)" && cd /usr/src/nxdk && \
    NXDK_CFLAGS="-I/usr/src/xcat-lwip" make NXDK_ONLY=y -j"$(nproc)"
# nxdk compiles without optimisation. SDL's software renderer and blitters, SDL_ttf and
# FreeType touch every pixel of every frame of the UI, so they're built again at -O2 (the UI
# ran at 9 frames/s on the console without; docs/ui-style.md#cost). The rest of nxdk (libc,
# lwIP, ...) is left as it was tested.
RUN eval "$(/usr/src/nxdk/bin/activate -s)" && cd /usr/src/nxdk && \
    find lib/sdl -name '*.obj' -delete && \
    rm -f lib/libSDL2.lib lib/SDL2_test.lib lib/libSDL2_image.lib lib/libSDL_ttf.lib \
          lib/libfreetype.lib && \
    NXDK_CFLAGS="-I/usr/src/xcat-lwip" CFLAGS="-O2" make NXDK_ONLY=y -j"$(nproc)"

# ghcr.io/xboxdev/nxdk-runbase:git-3a71ed5f
FROM ghcr.io/xboxdev/nxdk-runbase@sha256:cfe819e9eddb8372c896977782a6b776a435af8597693fccf601ad9649b2ecbb

COPY --from=builder /usr/src/nxdk/ /usr/src/nxdk/
# The prebuilt objects' dependency files name it; keep it so app builds don't rebuild lwIP.
COPY --from=builder /usr/src/xcat-lwip/ /usr/src/xcat-lwip/
ENV NXDK_DIR=/usr/src/nxdk

WORKDIR /usr/src/app

ENTRYPOINT ["/usr/src/nxdk/docker_entry.sh"]
