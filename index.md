![img](https://github.com/zfsonfreebsd/ZoF/raw/gh-pages/zof-logo.png)

OpenZFS is an advanced file system and volume manager which was originally
developed for Solaris and is now maintained by the OpenZFS community.

# Official Resources

  * [ZoF GitHub Site](https://zfsonfreebsd.github.io/ZoF/)
  * [OpenZFS site](http://open-zfs.org/)
  * [OpenZFS repo](https://github.com/openzfs/zfs)

# Installation on FreeBSD

OpenZFS is available in the FreeBSD ports tree as sysutils/openzfs and
sysutils/openzfs-kmod. It can be installed on FreeBSD stable/12 or later.

# Development on FreeBSD

The following dependencies are required to build OpenZFS on FreeBSD:
  * FreeBSD sources in /usr/src or elsewhere specified by SYSDIR in env
  * Packages for build:
    ```
    autoconf
    automake
    autotools
    bash
    git
    gmake
    ```
  * Optional packages for build:
    ```
    python3 # or your preferred Python version
    ```
  * Optional packages for test:
    ```
    base64
    fio
    hs-ShellCheck
    ksh93
    py36-flake8 # or your preferred Python version
    sudo
    ```
    The user for running tests must have NOPASSWD sudo permission.

To build and install:
```
# as user
git clone https://github.com/openzfs/zfs
cd zfs
./autogen.sh
./configure
gmake -j$(sysctl -n hw.ncpu)
# as root
gmake install
```
The ZFS utilities will be installed in /usr/local/sbin/, so make sure your PATH
gets adjusted accordingly. Though not required, `WITHOUT_ZFS` is a useful build
option in FreeBSD to avoid building and installing the legacy zfs tools and
kmod - see `src.conf(5)`.

Beware that the FreeBSD boot loader does not allow booting from root pools with
encryption active (even if it is not in use), so do not try encryption on a
pool you boot from.

# Contributing

Submit changes to the [openzfs/zfs](https://github.com/openzfs/zfs) repo.

# Issues

Issues can be reported via GitHub's [Issue Tracker](https://github.com/openzfs/zfs).

