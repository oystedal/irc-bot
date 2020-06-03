irc-bot
-------

An IRC bot written in C++ using boost asio.

### build

First time setup: Fetch external dependencies, build dependencies, and run `meson`:
```
vcs-import --input external_repositories.yaml third_party
./build_dependencies.sh
meson build
```

Execute ninja to build
```
ninja -C build
```

### config

```
cp config.json.example config.json
```

Edit as needed

### run

```
./build/main
```
