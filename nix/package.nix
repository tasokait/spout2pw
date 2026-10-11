{
  lib,
  stdenv,
  meson,
  ninja,
  cmake,
  pkgsCross,
  wineWow64Packages,
  mesa,
  pkg-config,
  libgbm,
  libdrm,
  vulkan-loader,
  vulkan-headers,
  pipewire,
  zenity,
  makeWrapper,
  withWine ? wineWow64Packages.staging,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "spout2pw";
  version = "0.3.0";

  src = ./..;

  strictDeps = true;
  __structuredAttrs = true;

  buildInputs = [
    vulkan-headers
    makeWrapper
    libgbm
    libdrm
    vulkan-loader
    pipewire
    pkg-config
  ];

  nativeBuildInputs = [
    meson
    ninja
    cmake
    pkgsCross.mingwW64.buildPackages.gcc
  ];

  depsBuildBuild = [
    pkg-config
    libgbm
    libdrm
    vulkan-loader
    pipewire
    withWine
    pkg-config
  ];

  patches = [
    ./0001-mesa.patch
    ./0002-spout2pw-path.patch
    ./0003-spout2pw-usage.patch
    ./0004-tools-package.patch
  ];

  postPatch = ''
    patchShebangs --build tools/get_wine_path.sh
    chmod +x tools/package.sh
    patchShebangs --build tools/package.sh
    patchShebangs --host misc/spout2pw.sh
  '';

  mesonFlags = [
    "--cross-file=${finalAttrs.src}/misc/x86_64-w64-mingw32.txt"
    (lib.mesonBool "libpipewire_static" false)
  ];

  runtimeInputs = [
    zenity
  ];

  postInstall = ''
    mkdir -p $out/bin
    makeWrapper $out/share/spout2pw/spout2pw.sh $out/bin/spout2pw \
      --argv0 spout2pw \
      --prefix PATH : ${lib.makeBinPath finalAttrs.runtimeInputs}
  '';

  postFixup = ''
    substituteInPlace $out/share/spout2pw/spout2pw.sh \
      --replace-fail '@MESA_PATH@' "${mesa}" \
      --replace-fail '@SPOUT2PW_PATH@' "$out"
  '';

  meta = {
    description = "Spout2 to PipeWire video bridge";
    homepage = "https://github.com/tasokait/spout2pw";
    mainProgram = "spout2pw";
    license = lib.licenses.lgpl21Plus;
  };
})
