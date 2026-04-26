# syntax=docker/dockerfile:1.6
# ─────────────────────────────────────────────────────────────────────────
# Eta — Binder image
#
# Builds eta + the eta_jupyter kernel from source on top of conda-forge
# (mambaforge), then registers the kernelspec into the active sys-prefix
# so JupyterLab finds it.
#
# First cold-build on mybinder.org takes ~20 minutes (boost source build
# ~3 min, libtorch download ~2 min, eta + xeus + nng + eigen build ~12 min).
# Subsequent launches reuse the cached image.
#
# Local test:
#   docker build -f binder/Dockerfile -t eta-binder .
#   docker run --rm -p 8888:8888 eta-binder \
#     jupyter lab --ip=0.0.0.0 --port=8888 --no-browser \
#                 --ServerApp.token='' --ServerApp.password=''
# ─────────────────────────────────────────────────────────────────────────

FROM condaforge/mambaforge:latest

# ── Binder boilerplate ────────────────────────────────────────────────
ARG NB_USER=jovyan
ARG NB_UID=1000
ENV USER=${NB_USER} \
    NB_UID=${NB_UID} \
    HOME=/home/${NB_USER}

# ── System packages ───────────────────────────────────────────────────
# Most build deps come from conda-forge; apt covers things conda-forge
# does not surface cleanly in this base image.
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
        git curl ca-certificates patch make perl \
        graphviz libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# ── conda-forge build & runtime stack ─────────────────────────────────
# - python + jupyterlab: notebook server
# - gcc 13 / gxx 13:     C++23 toolchain
# - cmake ≥ 3.28, ninja: required by eta
RUN mamba install -y -n base -c conda-forge \
        python=3.11 \
        jupyterlab=4 notebook \
        cmake ninja \
        gcc=13 gxx=13 binutils \
    && mamba clean -afy

ENV CC=gcc CXX=g++ AR=ar RANLIB=ranlib

# ── Boost 1.88 (eta requires ≥ 1.88) ──────────────────────────────────
# Distro packages and current conda-forge top out below 1.88, so build
# only Boost.Test (unit_test_framework) + headers from source.
ENV BOOST_VER=1.88.0 \
    BOOST_ROOT=/opt/boost
RUN set -eux; \
    BVU=$(echo "$BOOST_VER" | tr . _); \
    curl -fsSL -o /tmp/boost.tar.bz2 \
        "https://archives.boost.io/release/${BOOST_VER}/source/boost_${BVU}.tar.bz2"; \
    mkdir -p /tmp/boost_src "${BOOST_ROOT}"; \
    tar -xjf /tmp/boost.tar.bz2 -C /tmp/boost_src --strip-components=1; \
    cd /tmp/boost_src; \
    ./bootstrap.sh --prefix="${BOOST_ROOT}" --with-libraries=test; \
    ./b2 -j"$(nproc)" install link=shared variant=release; \
    rm -rf /tmp/boost.tar.bz2 /tmp/boost_src
ENV LD_LIBRARY_PATH=${BOOST_ROOT}/lib

# ── Binder user ───────────────────────────────────────────────────────
RUN useradd -m -s /bin/bash -u ${NB_UID} ${NB_USER}

# ── Copy source and build eta ─────────────────────────────────────────
# Build context is the repo root; .dockerignore prunes out/ dist/ .git etc.
COPY --chown=${NB_UID}:${NB_UID} . ${HOME}/eta
WORKDIR ${HOME}/eta

# Build the runtime targets needed for the Jupyter kernel.
# libtorch (CPU) is auto-downloaded by cmake/FetchLibtorch.cmake;
# xeus / xeus-zmq / nng / eigen / csv-parser are FetchContent-fetched.
RUN cmake -S . -B out/release -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DETA_TORCH_BACKEND=cpu \
        -DBOOST_ROOT=${BOOST_ROOT} \
    && cmake --build out/release \
        --target eta_jupyter etac etai eta_repl \
        -j"$(nproc)" \
    && chown -R ${NB_UID}:${NB_UID} ${HOME}/eta

# ── Runtime PATHs and kernelspec install ──────────────────────────────
ENV PATH=${HOME}/eta/out/release/bin:${PATH} \
    ETA_MODULE_PATH=${HOME}/eta/stdlib

# Register the kernel into the active conda env. The path written into
# kernel.json is the absolute one above, which survives container restarts.
RUN eta_jupyter --install --sys-prefix \
    && jupyter kernelspec list

# ── Hand off to non-root user ─────────────────────────────────────────
USER ${NB_USER}
WORKDIR ${HOME}/eta/examples/notebooks

