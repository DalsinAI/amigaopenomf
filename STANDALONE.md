# Standalone port repository

`amigaopenomf` is the canonical home of the AmigaOS 3.x / AmiCradle work for
`OpenOMF`. Its history was split from `DalsinAI/amigachrome-gameports` without
flattening the commits.

Repository-local build support under `scripts/`, `toolchain/` and `gameports/`
is included where the port needs it. Source revisions and downloadable runtime
data are pinned; proprietary game data remains external and must be supplied
lawfully by the user.

Prepare every source and build dependency at its pinned revision:

```sh
python3 scripts/game_port_prepare.py check --game all
python3 scripts/game_port_prepare.py prepare --game all --allow-network
python3 scripts/game_port_prepare.py verify --game all
```

This prepares OpenOMF, the C-Dogs SDL ENet source used by the port, and the
AmiCradle guest AGA profile entirely inside `build/game-ports/`. The build can
then run without the former GamePorts checkout:

```sh
sh build-amigaos3.sh
```

The cross compiler/SDK is still supplied by `STOVE` (default:
`$HOME/AmiCradle/stoves/os32-gcc16`). Original One Must Fall 2097 game data is
not redistributed.

GamePorts no longer owns a second implementation of this port.
