# Redix

Redix is a C++23 Redis-style key-value store assignment scaffold.

## Layout

```text
.
├── Makefile
├── include/
├── src/
├── test/
├── bin/
└── build/
```

## Build

```bash
make
make debug
make test
make integration
```

The build uses `clang++`, `-std=c++23`, and `-stdlib=libc++`.
