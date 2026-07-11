{
  rev,
  lib,
  clangStdenv,
  pkg-config,
  meson,
  ninja,
  cmake,
  curl,
  mbedtls,
  libffi,
  libarchive,
  zlib,
  asmjit,
  replxx,
  mimalloc,
}:
clangStdenv.mkDerivation (finalAttrs: {
  pname = "nari";
  version = rev;

  __structuredAttrs = true;

  src = ../.;

  nativeBuildInputs = [
    pkg-config
    meson
    ninja
    cmake
  ];

  buildInputs = [
    curl.dev
    mbedtls
    libffi.dev
    libarchive
    zlib
    asmjit
    replxx
    mimalloc
  ];

  strictDeps = true;

  mesonFlags = [
    "-Dsystem_deps=true"
    "--buildtype=release"
  ];

  meta = {
    description = "dynamically typed multi-paradigm language featuring a full JIT compiler, and an entire ecosystem";
    homepage = "https://github.com/nari-lang/nari";
    license = lib.licenses.gpl3Only;
    maintainers = [];
  };
})
