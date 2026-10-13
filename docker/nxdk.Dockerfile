# nxdk build image for xiso-cartographer.
#
# Mirrors third_party/nxdk/Dockerfile, but pins the base images by digest and
# builds nxdk from our pinned submodule, so the toolchain always matches the
# commit recorded in this repo.
#
# Build context must be the nxdk submodule:
#   docker build -f docker/nxdk.Dockerfile -t xcat-nxdk third_party/nxdk
# Used by `make docker-xbe` and CI. See docs/build-xbox.md.

# ghcr.io/xboxdev/nxdk-buildbase:git-3a71ed5f
FROM ghcr.io/xboxdev/nxdk-buildbase@sha256:b14cd17f59449c0c3bdc86dc04dde411cdd0eaaeb3539fbe85a598dec3846f52 AS builder

COPY ./ /usr/src/nxdk/
ENV NXDK_DIR=/usr/src/nxdk
RUN cd /usr/src/nxdk && make tools -j"$(nproc)"
# Prebuild the nxdk libraries (libc, lwIP, SDL, ...) so app builds only compile our code.
RUN eval "$(/usr/src/nxdk/bin/activate -s)" && cd /usr/src/nxdk && make NXDK_ONLY=y -j"$(nproc)"

# ghcr.io/xboxdev/nxdk-runbase:git-3a71ed5f
FROM ghcr.io/xboxdev/nxdk-runbase@sha256:cfe819e9eddb8372c896977782a6b776a435af8597693fccf601ad9649b2ecbb

COPY --from=builder /usr/src/nxdk/ /usr/src/nxdk/
ENV NXDK_DIR=/usr/src/nxdk

WORKDIR /usr/src/app

ENTRYPOINT ["/usr/src/nxdk/docker_entry.sh"]
