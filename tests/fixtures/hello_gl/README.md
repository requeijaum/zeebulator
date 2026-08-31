# hello_gl fixture

`hello_gl.c` is our own minimal BREW-shaped test app (not Qualcomm code --
see the file-level comment) used by `tests/gl_lifecycle_test.cpp` to
integration-test the `IGL`/`IEGL` HLE dispatch against real compiled ARM
code, the same way `hello_brew.c` does for `IShell`/`IDisplay`.

`hello_gl.bin` is the compiled, flat-binary output, committed directly
since it's small and it's our own original code (no copyright concern,
unlike real game dumps). Regenerate it if `hello_gl.c` changes:

```sh
arm-none-eabi-gcc -march=armv5te -marm -mthumb-interwork -ffreestanding \
  -fno-builtin -nostdlib -O0 -Wl,-Ttext=0x00100000 -Wl,-e,AEEMod_Load \
  -o hello_gl.elf hello_gl.c
arm-none-eabi-objcopy -O binary hello_gl.elf hello_gl.bin
```

After regenerating, **re-verify `kAeeModLoadOffset` in
`tests/gl_lifecycle_test.cpp`** by checking where the linker actually
placed `AEEMod_Load` (it is not necessarily the first function in the
file -- the compiler is free to order functions however it likes):

```sh
arm-none-eabi-nm hello_gl.elf | grep AEEMod_Load
# subtract the link base (0x00100000) from the reported address
```

`hello_gl.elf` itself is not committed (only needed to regenerate the
`.bin`, and it's easy to reproduce from the command above).
