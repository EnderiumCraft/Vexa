This is Vexa's root file system. It lives in memory (tmpfs): the build packs
the rootfs/ folder and the programs from userland/ into initramfs.tar, and the
kernel unpacks it at boot. Changes you make here are lost when you restart.

  /bin   programs (try: run hello-world)
  /dev   devices
  /etc   settings
  /mnt   disks the kernel found and mounted
  /tmp   scratch space
