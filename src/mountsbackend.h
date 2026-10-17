#pragma once
#include "backend.h"

///
/// Backend for network shares and user space filesystems, e.g. sshfs, NFS or SMB.
///
/// Mounted ones are read from the mount table, the ones that can be mounted from fstab. Uses the
/// mount tools, with pkexec if the user is not allowed to mount or unmount on their own.
///
class MountsBackend : public Backend
{
public:

    std::vector<Volume> volumes() override;
    QCoro::Task<Result> mount(Volume volume) override;
    QCoro::Task<Result> unmount(Volume volume, bool force) override;
    QCoro::Task<Result> unlockAndMount(Volume volume, QString passphrase) override;
    QCoro::Task<Result> lock(Volume volume) override;
    QCoro::Task<Result> safelyRemove(Volume volume) override;

};
