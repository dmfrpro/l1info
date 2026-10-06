{
  pkgs,
  inputs,
  ...
}:

inputs.treefmt-nix.lib.mkWrapper pkgs {
  projectRootFile = "flake.nix";
  programs = {
    nixfmt.enable = true;
    shfmt.enable = true;
    shellcheck.enable = true;
  };
  settings.formatter = {
    clang-format = {
      command = pkgs.lib.getExe' pkgs.clang-tools "clang-format";
      options = [
        "-i"
        "-style=Microsoft"
      ];
      includes = [
        "*.c"
        "*.cpp"
        "*.h"
      ];
    };
    black = {
      command = pkgs.lib.getExe pkgs.black;
      options = [
        "--line-length"
        "80"
      ];
      includes = [ "*.py" ];
    };
    markdownlint = {
      command = pkgs.lib.getExe pkgs.markdownlint-cli;
      options = [
        "--fix"
        "--disable"
        "MD013"
        "MD024"
      ];
      includes = [ "*.md" ];
    };
  };
}
