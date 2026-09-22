# ota/

ROM-packaged Native Bridge for a **signed GrapheneOS/LINEOS OTA**, not Magisk
Direct Install. On this Pixel the bootloader stays locked; changes go in
`system/lib64` + `build.prop` inside an avbroot-patched OTA signed with
`~/grapheneos/keys`.

## What gets injected

- `/system/lib64/libmango_translator.so`
- `ro.dalvik.vm.native.bridge=libmango_translator.so`
- `ro.product.cpu.abilist` / `abilist32` (system and product images) gain
  `armeabi-v7a,armeabi` so the package installer will accept 32-bit APKs.
  Zygote stays 64-bit (no Pixel-7-style 32-bit zygote).

The signed GrapheneOS pack also installs Custota and removes the stock
GrapheneOS System Updater (`/system/priv-app/Updater`).

## Build

```
cmake -S native -B native/build
cmake --build native/build --target mango_translator
```

On Termux, CMake already targets Android. Then patch the GrapheneOS OTA
with `ota/mango.py` loaded by my-avbroot-setup (`--module-mango-so`).
