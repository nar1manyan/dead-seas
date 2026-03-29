# Dead Seas

### Deps

```bash
  sudo apt install build-essential cmake libsfml-dev
```

### CMake

```bash
  cmake -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j$(nproc)
```

## Run Application

**Server Terminal:**

```bash
  ./build/server
```

**Client Terminal:**

```bash
  ./build/client
```