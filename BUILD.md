## Building pi-gen

```bash
gotty1337@apollo13-rpi4:~/source/repos/pi-gen $ docker build -t pi-gen:latest .
docker run --rm -it  --privileged -v /home/gotty1337/source/repos/pi-gen:/pi-gen -v /home/gotty1337/pi-gen-work:/pi-gen/work -v /home/gotty1337/pi-gen-deploy:/pi-gen/deploy pi-gen:latest ./build.sh
```

## Building pi-gen with WIN32 support

```powershell
docker build -t pi-gen:latest .
docker volume create pi-gen-work
docker run --rm -it --privileged -v "C:/Users/thomas.oresnik/source/repos/pi-gen:/pi-gen" -v "pi-gen-work:/pi-gen/work" -v "C:/Users/thomas.oresnik/pi-gen-deploy:/pi-gen/deploy" pi-gen:latest bash -c "mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc 2>/dev/null || true; find /pi-gen -maxdepth 4 -type f -not -path '*/.git/*' | xargs dos2unix -q 2>/dev/null; ./build.sh"
```

### QEMU

Only if needed for cross-compilation, e.g. when building on a non-ARM64 host.

```powershell
docker run --privileged --rm tonistiigi/binfmt --install arm,arm64
 ```