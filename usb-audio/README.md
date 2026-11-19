# Smart Pro UAC1 modules

These GPL-2.0 kernel modules enable the experimental, input-only UAC1 gadget
test on the Trimui Smart Pro running Linux 4.9.191. They are not for Brick,
Brick Pro, Smart Pro S, other kernels, or USB audio OUT.

The source files and target kernel configuration are included in `source/`.
They come from [`pocketforge-os/kernel-tsp`](https://github.com/pocketforge-os/kernel-tsp)
at commit `3f6d59b76852ea2edf9738649f470b5cf42fa436`; the full kernel tree and
build scripts are available at that revision. `source/COPYING` is the kernel's
GPLv2 license.

To reproduce the modules, check out that revision, copy
`source/tg5040-kernel.config` to `.config`, and run:

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  M=drivers/usb/gadget/function \
  CONFIG_USB_U_AUDIO=m CONFIG_USB_F_UAC1=m modules
```

The build requires the matching kernel source and an AArch64 cross-compiler.
The target config has `CONFIG_MODVERSIONS` disabled. The bundled modules have
`vermagic 4.9.191 SMP preempt mod_unload aarch64`.

SHA-256:

```text
u_audio.ko     5d5d42f1ebfb027078a6ec4fc58418ead4305fe221fa97d2b85f22efd773edbe
usb_f_uac1.ko  0be97a179c24173de135b5cc08978840670c21e2647152c350964fedc3f0bfd2
```
