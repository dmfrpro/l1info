{
  pkgs,
  inputs,
  flake,
  perSystem,
  ...
}:

let
  system = pkgs.stdenv.hostPlatform.system;
  pre-commit = inputs.git-hooks.lib.${system}.run {
    src = ./.;
    hooks = {
      treefmt = {
        enable = true;
        package = flake.formatter.${system};
      };
      statix.enable = true;
      deadnix.enable = true;
      end-of-file-fixer.enable = true;
      check-shebang-scripts-are-executable.enable = true;
      nixf-diagnose.enable = true;
    };
  };
in

(import inputs.devshell { nixpkgs = pkgs; }).mkShell {
  packages =
    (with pkgs; [
      clang
      clang-tools
      perf
      util-linux
      coreutils

      nix
      git

      statix
      deadnix
      nixf-diagnose

      perSystem.self.test
    ])
    ++ pre-commit.enabledPackages;

  commands = [
    {
      name = "l1info";
      help = "measure L1d: l1info [CPU] (pinned to CPU if given)";
      command = ''
        if [ $# -gt 0 ]; then
          exec taskset -c "$1" ${perSystem.self.l1info}/bin/l1info
        fi
        exec ${perSystem.self.l1info}/bin/l1info
      '';
    }
    {
      name = "validate";
      help = "validate against lstopo: validate [CPU] [--runs N] [--success-ratio PCT]";
      command = ''exec ${perSystem.self.test}/bin/test "$@"'';
    }
  ];

  devshell.startup.pre-commit.text = pre-commit.shellHook;

  env = [
    {
      name = "TMPDIR";
      eval = ''"$([ -w /tmp ] && echo /tmp || echo /build)"'';
    }
  ];
}
