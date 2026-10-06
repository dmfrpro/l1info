# L1 data cache characteristics

Validation compares the measured L1d capacity, line size and
associativity against the `lstopo` topology. A CPU core passes when at
least `--success-ratio` percent of its `--runs` measurements match
exactly

## Example usage with Nix devShell

```sh
nix develop
validate                                    # all cores
validate 3                                  # only on CPU 3
validate 3 --runs 20 --success-ratio 90

l1info                                      # random CPU core manual run
l1info 3                                    # only on CPU 3
```

## Example usage with Docker

```sh
docker run --rm -it dmfrpro/l1info          # the same devShell in a container
```
