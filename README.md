irc-bot
-------

An IRC bot written in C++ using boost asio.

### build

First time, build dependencies and run `meson`:
```
./build_dependencies.sh
meson build
```

Then to build:
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
