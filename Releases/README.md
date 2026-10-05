Packed StOMP binaries belong on GitHub Releases, not in this directory.

Build one from this tree:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --target stomp-pack
```

The file is `build/stomp`. `install.sh` uses that path when you run it from a source checkout.
