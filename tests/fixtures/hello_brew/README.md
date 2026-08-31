# hello_brew fixture

`hello_brew.c` is our own minimal BREW-shaped test app (not Qualcomm code
-- see the file-level comment) used by `tests/brew_lifecycle_test.cpp` to
integration-test the loader/HLE pipeline against real compiled ARM code.

`hello_brew.bin` is the compiled, flat-binary output, committed directly
since it's small (~4.3KB) and it's our own original code (no copyright
concern, unlike real game dumps). Regenerate it if `hello_brew.c` changes:

```sh
arm-none-eabi-gcc -march=armv5te -marm -mthumb-interwork -ffreestanding \
  -fno-builtin -nostdlib -O0 -Wl,-Ttext=0x00100000 -Wl,-e,AEEMod_Load \
  -o hello_brew.elf hello_brew.c
arm-none-eabi-objcopy -O binary hello_brew.elf hello_brew.bin
```

After regenerating, **re-verify `kAeeModLoadOffset` in
`tests/brew_lifecycle_test.cpp`** by checking where the linker actually
placed `AEEMod_Load` (it is not necessarily the first function in the
file -- the compiler is free to order functions however it likes):

```sh
arm-none-eabi-nm hello_brew.elf | grep AEEMod_Load
# subtract the link base (0x00100000) from the reported address
```

`hello_brew.elf` itself is not committed (only needed to regenerate the
`.bin`, and it's easy to reproduce from the command above).
