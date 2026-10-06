{ pkgs, flake, ... }:

let
  system = pkgs.stdenv.hostPlatform.system;
in

pkgs.dockerTools.buildNixShellImage {
  name = "dmfrpro/l1info";
  tag = "latest";
  drv = flake.devShells.${system}.default // {
    outputs = [ "out" ];
  };
}
