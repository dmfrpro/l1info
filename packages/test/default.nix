{ pkgs, perSystem, ... }:

let
  python = pkgs.python3.withPackages (
    ps: with ps; [
      pytest
      pytest-sugar
      rich
    ]
  );
in

pkgs.writeShellApplication {
  name = "test";
  runtimeInputs = with pkgs; [
    hwloc
    util-linux
  ];
  text = ''
    if [ $# -gt 0 ] && [ "$1" -eq "$1" ] 2>/dev/null; then
      cpu=$1
      shift
      set -- --cpu "$cpu" "$@"
    fi

    cd ${./.}
    exec ${python}/bin/pytest -p no:cacheprovider \
      --benchmark ${pkgs.lib.getExe perSystem.self.l1info} . "$@"
  '';
  meta = {
    description = "Validate l1info against lstopo ([CPU] [--runs N] [--success-ratio PCT])";
    mainProgram = "test";
    platforms = [ "x86_64-linux" ];
  };
}
