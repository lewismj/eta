# Binder configuration

This folder builds the [mybinder.org](https://mybinder.org) image for the Eta
notebooks.

## Launch URLs

| Notebook | Badge link |
|---|---|
| Language Basics | `https://mybinder.org/v2/gh/lewismj/eta/HEAD?labpath=examples%2Fnotebooks%2FLanguageBasics.ipynb` |
| AAD             | `https://mybinder.org/v2/gh/lewismj/eta/HEAD?labpath=examples%2Fnotebooks%2FAAD.ipynb` |
| Portfolio       | `https://mybinder.org/v2/gh/lewismj/eta/HEAD?labpath=examples%2Fnotebooks%2FPortfolio.ipynb` |

## How it works

`Dockerfile` builds eta + the `eta_jupyter` kernel from source on top of
`condaforge/mambaforge`:

1. apt installs runtime deps (`libssl-dev`, `graphviz`, …).
2. conda-forge supplies Python 3.11, JupyterLab 4, GCC 13, CMake ≥ 3.28,
   Ninja.
3. Boost 1.88 is built from source (only `Boost.Test`, ~3 min).
4. CMake fetches xeus, xeus-zmq, libzmq, nng, Eigen, csv-parser,
   nlohmann_json (in-tree).
5. `FetchLibtorch.cmake` downloads the CPU-only libtorch 2.5.1 (~200 MB).
6. The runtime targets `eta_jupyter etac etai eta_repl` are built.
7. `eta_jupyter --install --sys-prefix` registers the `Eta` kernel into the
   active conda env so JupyterLab finds it.

## First-launch cost

A cold build on mybinder.org takes **~20 minutes**. After that the image is
cached and launches in seconds until a new commit invalidates the cache.

## xeus-eta on conda-forge

Once `xeus-eta` lands on conda-forge, this Dockerfile can be replaced by a
simple `environment.yml` + `postBuild` (see `docs/eta_plan.md` §2.3) and
launches will drop to under a minute.

## Local reproduction

```sh
docker build -f binder/Dockerfile -t eta-binder .
docker run --rm -p 8888:8888 eta-binder \
  jupyter lab --ip=0.0.0.0 --port=8888 --no-browser \
              --ServerApp.token='' --ServerApp.password=''
```

Then open <http://localhost:8888/lab>.

