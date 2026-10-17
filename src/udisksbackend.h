#pragma once
#include "backend.h"

///
/// Backend using UDisks2, available on all Linux desktops.
///
class UDisksBackend : public Backend
{
public:

    std::vector<Volume> volumes() override;
    QCoro::Task<Result> mount(Volume volume) override;
    QCoro::Task<Result> unmount(Volume volume, bool force) override;
    QCoro::Task<Result> unlockAndMount(Volume volume, QString passphrase) override;
    QCoro::Task<Result> lock(Volume volume) override;
    QCoro::Task<Result> safelyRemove(Volume volume) override;

};
