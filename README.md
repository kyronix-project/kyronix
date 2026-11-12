# Kyronix

<img align="left" src="meta/logo.png" width="96" alt="Kyronix logo">

Minimalist x86-64 operating system that sucks less.

![Commits per month](https://img.shields.io/github/commit-activity/m/kyronix-project/kyronix)
[![test](https://github.com/kyronix-project/kyronix/actions/workflows/test.yml/badge.svg)](https://github.com/kyronix-project/kyronix/actions/workflows/test.yml)
[![ISC](https://img.shields.io/badge/license-ISC-blue)](LICENSE)
[![x86-64](https://img.shields.io/badge/arch-x86__64-lightgrey)](#)

<br clear="left"/>

| <img src="meta/screenshots/preview.png" alt="Kyronix screenshot" width="760"> |
| :---: |

## Building & Running

Build using a container runtime (`podman` or `docker`):

```sh
make CRUNTIME=podman -j $(nproc)
```

and run: 
```sh
make run
```

## License
ISC License. See [LICENSE](./LICENSE) for details.