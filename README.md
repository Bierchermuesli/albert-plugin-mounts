# Drives

Albert plugin to mount, unmount and safely remove drives, e.g. USB sticks, external disks or optical media.

- Lists the drives that are not part of the system, with their state (mounted, not mounted, locked).
- Mount and open, unmount, copy the mount path. Shows the free space of mounted drives.
- Force unmount (lazy) for busy drives. If unmounting fails because the drive is busy, the error names the programs using it.
- Encrypted (LUKS) volumes: unlock with a passphrase and mount, lock.
- Safely remove: unmounts and locks all volumes of the drive, then powers it off or ejects the medium.
- Network shares and user space filesystems, e.g. sshfs, NFS or SMB: mounted ones are listed from the mount table, the ones that can be mounted from fstab. Unmount, force unmount (lazy) and mount, with an authentication dialog if the user is not allowed to on their own.

Search by name, label, drive model or device, or use the trigger `mount `.

## Platforms

Linux using UDisks2, independent of the desktop environment. Permissions are handled by polkit, in an active session usually without a password.

The platform specific part is a small backend interface (`src/backend.h`), so other platforms, e.g. macOS using DiskArbitration, can be added.

## Build

The plugin is built as part of the Albert source tree. Clone it into `plugins/drives` and build Albert as usual. Requires Qt DBus and Widgets and QCoro (`qcoro-qt6-dev` on Debian).
