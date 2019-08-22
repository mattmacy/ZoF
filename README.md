![img](https://github.com/zfsonfreebsd/ZoF/raw/master/zof-logo.png)

ZFS on Linux is an advanced file system and volume manager which was originally
developed for Solaris and is now maintained by the OpenZFS community. ZoF is
the work to bring FreeBSD support into the ZoL repo.

[![codecov](https://codecov.io/gh/zfsonlinux/zfs/branch/master/graph/badge.svg)](https://codecov.io/gh/zfsonlinux/zfs)
[![coverity](https://scan.coverity.com/projects/1973/badge.svg)](https://scan.coverity.com/projects/zfsonlinux-zfs)

# Official Resources

  * [ZoF GitHub Site](https://zfsonfreebsd.github.io/ZoF/)
  * [ZoL Site](http://zfsonlinux.org)
  * [ZoL Wiki](https://github.com/zfsonlinux/zfs/wiki)
  * [ZoL Mailing lists](https://github.com/zfsonlinux/zfs/wiki/Mailing-Lists)
  * [OpenZFS site](http://open-zfs.org/)

# Installation

ZoF is available in the FreeBSD ports tree as sysutils/openzfs and
sysutils/openzfs-kmod. It can be installed on FreeBSD stable/12 or later.

# Development

The following dependencies are required to build ZoF from source:
  * FreeBSD sources in /usr/src or elsewhere specified by passing
    `--with-freebsd=$path` to `./configure`
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
    shuf
    sudo
    ```
    The user for running tests must have NOPASSWD sudo permission.

To build and install:
```
# as user
git clone https://github.com/zfsonfreebsd/ZoF
cd ZoF
./autogen.sh
./configure
gmake
# as root
gmake install
```
The ZFS utilities will be installed in /usr/local/sbin/, so make sure your PATH
gets adjusted accordingly.

Beware that the FreeBSD boot loader does not allow booting from root pools with
encryption active (even if it is not in use), so do not try encryption on a
pool you boot from.

# Issues

Issues can be reported via GitHub's [Issue Tracker](https://github.com/zfsonfreebsd/ZoF).

