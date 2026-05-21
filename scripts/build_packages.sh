#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/.." && pwd)"

build_root="${BUILD_ROOT:-${project_root}/out/package-build}"
config="${CONFIG:-Release}"
eta_executable="${ETA_EXECUTABLE:-}"
fetch_upstream="${FETCH_UPSTREAM:-ON}"
skip_native="${SKIP_NATIVE:-OFF}"
cmake_prefix_path="${CMAKE_PREFIX_PATH:-}"
boost_dir="${BOOST_DIR:-}"
boost_include_dir="${BOOST_INCLUDE_DIR:-}"
eta_core_library="${ETA_CORE_LIBRARY:-}"

while [ $# -gt 0 ]; do
  case "$1" in
    --build-root)
      build_root="$2"
      shift 2
      ;;
    --config)
      config="$2"
      shift 2
      ;;
    --eta-exe)
      eta_executable="$2"
      shift 2
      ;;
    --fetch-upstream)
      fetch_upstream="ON"
      shift
      ;;
    --no-fetch-upstream)
      fetch_upstream="OFF"
      shift
      ;;
    --skip-native)
      skip_native="ON"
      shift
      ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "Usage: $0 [--build-root DIR] [--config Release|Debug] [--eta-exe PATH] [--fetch-upstream] [--skip-native]" >&2
      exit 1
      ;;
  esac
done

if [ -z "${eta_executable}" ]; then
  eta_executable="${project_root}/out/release/eta/cli/eta"
fi
if [ ! -x "${eta_executable}" ]; then
  echo "eta executable not found or not executable: ${eta_executable}" >&2
  exit 1
fi

host_triple() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"
  case "${os}" in
    Linux)
      case "${arch}" in
        x86_64) echo "x86_64-unknown-linux-gnu" ;;
        aarch64|arm64) echo "aarch64-unknown-linux-gnu" ;;
        *) echo "unsupported-linux-arch:${arch}" ;;
      esac
      ;;
    Darwin)
      case "${arch}" in
        x86_64) echo "x86_64-apple-darwin" ;;
        arm64|aarch64) echo "aarch64-apple-darwin" ;;
        *) echo "unsupported-macos-arch:${arch}" ;;
      esac
      ;;
    *)
      echo "unsupported-os:${os}"
      ;;
  esac
}

vcpkg_host_triplet() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"
  case "${os}" in
    Linux)
      case "${arch}" in
        x86_64) echo "x64-linux" ;;
        aarch64|arm64) echo "arm64-linux" ;;
        *) echo "unsupported-linux-arch:${arch}" ;;
      esac
      ;;
    Darwin)
      case "${arch}" in
        x86_64) echo "x64-osx" ;;
        arm64|aarch64) echo "arm64-osx" ;;
        *) echo "unsupported-macos-arch:${arch}" ;;
      esac
      ;;
    *)
      echo "unsupported-os:${os}"
      ;;
  esac
}

vcpkg_root=""
if [ -n "${VCPKG_ROOT:-}" ] && [ -d "${VCPKG_ROOT}" ]; then
  vcpkg_root="${VCPKG_ROOT}"
elif [ -n "${VCPKG_DIR:-}" ] && [ -d "${VCPKG_DIR}" ]; then
  vcpkg_root="${VCPKG_DIR}"
fi

if [ -n "${vcpkg_root}" ]; then
  vcpkg_triplet="$(vcpkg_host_triplet)"
  case "${vcpkg_triplet}" in
    unsupported-*)
      ;;
    *)
      vcpkg_installed="${vcpkg_root}/installed/${vcpkg_triplet}"
      if [ -d "${vcpkg_installed}" ]; then
        if [ -z "${cmake_prefix_path}" ]; then
          cmake_prefix_path="${vcpkg_installed}"
        fi
        if [ -z "${boost_dir}" ]; then
          if [ -d "${vcpkg_installed}/share/boost" ]; then
            boost_dir="${vcpkg_installed}/share/boost"
          elif [ -d "${vcpkg_installed}/share/boost-headers" ]; then
            boost_dir="${vcpkg_installed}/share/boost-headers"
          fi
        fi
        if [ -z "${boost_include_dir}" ] && [ -d "${vcpkg_installed}/include" ]; then
          boost_include_dir="${vcpkg_installed}/include"
        fi
      fi
      ;;
  esac
fi

resolve_sidecar_binary() {
  local build_dir="$1"
  local base_name="$2"
  local package_root="${3:-}"
  local file_name=""
  case "$(uname -s)" in
    Linux) file_name="lib${base_name}.so" ;;
    Darwin) file_name="lib${base_name}.dylib" ;;
    *) echo "Unsupported host OS for sidecar resolution: $(uname -s)" >&2; exit 1 ;;
  esac

  if [ -f "${build_dir}/${config}/${file_name}" ]; then
    echo "${build_dir}/${config}/${file_name}"
    return
  fi
  if [ -f "${build_dir}/${file_name}" ]; then
    echo "${build_dir}/${file_name}"
    return
  fi

  local found
  found="$(find "${build_dir}" -type f -name "${file_name}" | head -n 1 || true)"
  if [ -n "${found}" ]; then
    echo "${found}"
    return
  fi
  if [ -n "${package_root}" ] && [ -d "${package_root}/libs" ]; then
    found="$(find "${package_root}/libs" -type f -name "${file_name}" | head -n 1 || true)"
    if [ -n "${found}" ]; then
      echo "${found}"
      return
    fi
  fi
  echo "Could not locate sidecar binary ${file_name} under ${build_dir}" >&2
  exit 1
}

resolve_eta_core_library() {
  local eta_exe="$1"
  local config_name="$2"
  local explicit="${3:-}"
  local core_name=""
  case "$(uname -s)" in
    Linux|Darwin) core_name="libeta_core.a" ;;
    *) echo "Unsupported host OS for eta_core resolution: $(uname -s)" >&2; exit 1 ;;
  esac

  if [ -n "${explicit}" ]; then
    if [ -f "${explicit}" ]; then
      echo "${explicit}"
      return
    fi
    echo "ETA_CORE_LIBRARY does not exist: ${explicit}" >&2
    exit 1
  fi

  local eta_dir
  eta_dir="$(cd "$(dirname "${eta_exe}")" && pwd)"
  local candidates=(
    "${eta_dir}/../core/${core_name}"
    "${eta_dir}/../../core/${core_name}"
    "${eta_dir}/../core/${config_name}/${core_name}"
    "${eta_dir}/../../core/${config_name}/${core_name}"
    "${project_root}/build/eta/core/${core_name}"
    "${project_root}/build/eta/core/${config_name}/${core_name}"
  )

  local candidate=""
  for candidate in "${candidates[@]}"; do
    if [ -f "${candidate}" ]; then
      echo "${candidate}"
      return
    fi
  done

  echo "Could not resolve eta_core library from eta executable path. Pass ETA_CORE_LIBRARY explicitly." >&2
  exit 1
}

invoke_native_build() {
  local name="$1"
  local package_root="$2"
  local native_build_dir="$3"
  local target_name="$4"
  local base_name="$5"
  local stage_script="$6"
  local fetch_arg_name="${7:-}"
  local tests_arg_name="${8:-}"
  local cmake_prefix_arg="${9:-}"
  local boost_dir_arg="${10:-}"
  local boost_include_arg="${11:-}"
  local eta_core_library_arg="${12:-}"

  echo "> Building native package: ${name}"

  local configure_args=(
    -S "${package_root}"
    -B "${native_build_dir}"
  )
  if [ -n "${fetch_arg_name}" ]; then
    configure_args+=("-D${fetch_arg_name}=${fetch_upstream}")
  fi
  if [ -n "${tests_arg_name}" ]; then
    configure_args+=("-D${tests_arg_name}=OFF")
  fi
  if [ -n "${cmake_prefix_arg}" ]; then
    configure_args+=("-DCMAKE_PREFIX_PATH=${cmake_prefix_arg}")
  fi
  if [ -n "${boost_dir_arg}" ]; then
    configure_args+=("-DBoost_DIR=${boost_dir_arg}")
  fi
  if [ -n "${boost_include_arg}" ]; then
    configure_args+=("-DBoost_INCLUDE_DIR=${boost_include_arg}")
  fi
  if [ -n "${eta_core_library_arg}" ]; then
    configure_args+=("-DETA_CORE_LIBRARY=${eta_core_library_arg}")
  fi

  cmake "${configure_args[@]}"

  cmake --build "${native_build_dir}" --config "${config}" --target "${target_name}"

  local sidecar_binary
  sidecar_binary="$(resolve_sidecar_binary "${native_build_dir}" "${base_name}" "${package_root}")"

  local triple
  triple="$(host_triple)"
  case "${triple}" in
    unsupported-*)
      echo "Unsupported host target: ${triple}" >&2
      exit 1
      ;;
  esac

  cmake \
    -DPACKAGE_ROOT="${package_root}" \
    -DSIDECAR_BINARY="${sidecar_binary}" \
    -DHOST_TARGET_TRIPLE="${triple}" \
    -P "${stage_script}"
}

if [ "${skip_native}" != "ON" ]; then
  eta_core_library="$(resolve_eta_core_library "${eta_executable}" "${config}" "${eta_core_library}")"
  echo "  using ETA_CORE_LIBRARY=${eta_core_library}"

  invoke_native_build \
    "http" \
    "${project_root}/packages/net/native/http" \
    "${build_root}/http" \
    "eta_http" \
    "eta_http" \
    "${project_root}/packages/net/native/http/cmake/StageHttpSidecar.cmake" \
    "ETA_HTTP_FETCH_UPSTREAM" \
    "ETA_HTTP_ENABLE_TESTS" \
    "${cmake_prefix_path}" \
    "${boost_dir}" \
    "${boost_include_dir}" \
    "${eta_core_library}"

  invoke_native_build \
    "duckdb" \
    "${project_root}/packages/db/native/duckdb" \
    "${build_root}/duckdb" \
    "eta_duckdb" \
    "eta_duckdb" \
    "${project_root}/packages/db/native/duckdb/cmake/StageDuckDBSidecar.cmake" \
    "ETA_DUCKDB_FETCH_UPSTREAM" \
    "ETA_DUCKDB_ENABLE_TESTS" \
    "${cmake_prefix_path}" \
    "${boost_dir}" \
    "${boost_include_dir}" \
    "${eta_core_library}"

  invoke_native_build \
    "lightgbm" \
    "${project_root}/packages/ml/native/lightgbm" \
    "${build_root}/lightgbm" \
    "eta_lightgbm" \
    "eta_lightgbm" \
    "${project_root}/packages/ml/native/lightgbm/cmake/StageLightGBMSidecar.cmake" \
    "ETA_LIGHTGBM_FETCH_UPSTREAM" \
    "ETA_LIGHTGBM_ENABLE_TESTS" \
    "${cmake_prefix_path}" \
    "${boost_dir}" \
    "${boost_include_dir}" \
    "${eta_core_library}"
fi

echo "> Building Eta package artifacts (.etac) for non-stdlib packages"
stdlib_module_path="${project_root}/stdlib"
effective_module_path=""
if [ -d "${stdlib_module_path}" ]; then
  effective_module_path="${stdlib_module_path}"
elif [ -n "${ETA_MODULE_PATH:-}" ]; then
  effective_module_path="${ETA_MODULE_PATH}"
fi

if [ -n "${effective_module_path}" ]; then
  echo "  using ETA_MODULE_PATH=${effective_module_path}"
fi

while IFS= read -r manifest; do
  echo "  - ${manifest}"
  if [ -n "${effective_module_path}" ]; then
    ETA_MODULE_PATH="${effective_module_path}" "${eta_executable}" build --manifest-path "${manifest}"
  else
    "${eta_executable}" build --manifest-path "${manifest}"
  fi
done < <(find "${project_root}/packages" -type f -name "eta.toml" \
          ! -path "*/packages/stdlib/*" | sort)

echo
echo "[OK] Package rebuild complete."
