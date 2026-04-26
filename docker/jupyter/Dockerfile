FROM jupyter/scipy-notebook:latest

USER root
RUN apt-get update && \
    apt-get install -y --no-install-recommends graphviz && \
    rm -rf /var/lib/apt/lists/*

USER ${NB_UID}
RUN mamba install -y -c conda-forge xeus-eta && \
    mamba clean -a -y

RUN eta_jupyter --install --sys-prefix
