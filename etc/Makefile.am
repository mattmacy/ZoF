SUBDIRS = zfs sudoers.d
if BUILD_LINUX
SUBDIRS += $(ZFS_INIT_SYSTEMD) $(ZFS_INIT_SYSV) $(ZFS_MODULE_LOAD)
endif
DIST_SUBDIRS = init.d zfs systemd modules-load.d sudoers.d
