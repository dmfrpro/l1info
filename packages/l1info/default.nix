{ pkgs, ... }:

pkgs.clangStdenv.mkDerivation {
  pname = "l1info";
  version = "1.0.0";
  src = ./.;
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    clang++ -O3 -std=c++17 -Wall -Wextra -o l1info l1info.cpp
    runHook postBuild
  '';

  doCheck = true;
  nativeCheckInputs = [ pkgs.clang-tools ];
  checkPhase = ''
    runHook preCheck
    # Reuse the stdenv include paths so clang-tidy resolves the C++ stdlib
    # without a compilation database
    clang-tidy l1info.cpp -- $NIX_CFLAGS_COMPILE -std=c++17
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 l1info $out/bin/l1info
    runHook postInstall
  '';

  meta = {
    description = "L1d benchmark via pointer chase and a linked eviction cycle";
    mainProgram = "l1info";
    platforms = [ "x86_64-linux" ];
  };
}
