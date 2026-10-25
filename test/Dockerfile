# syntax=docker/dockerfile:1
# Keep this image limited to tools; sources are bind-mounted at run time.
ARG COMPILER_IMAGE=gcc:16.2.0-trixie
FROM ${COMPILER_IMAGE}
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates cmake git just ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /simplx
CMD ["just", "--list"]
