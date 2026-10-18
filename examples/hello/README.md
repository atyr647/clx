# Hello clx

The simplest possible clx program.

## Source

```lua
print("Hello clx!")
```

## Build

```bash
clx hello.lua
```

> Use the `--minimal` flag to compile with only the base and package modules, reducing executable size

## Run

```bash
./hello
```

Output:

```text
Hello clx!
```

## Generate C++

To inspect the generated C++ source:

```bash
clx --cpp hello.lua
```

## Purpose

This example demonstrates:

* Basic clx usage
* Native executable generation
* Lua-to-C++ compilation
* Minimal deployment workflow

It is the recommended starting point for new users.

## Next steps

After building this example, explore:

* `examples/mandelbrot/`
* `examples/pong/`
* `examples/sokol/`

for more advanced use cases.
