#pragma once
#include "backend.h"

///
/// Backend using UDisks2, available on all Linux desktops.
///
class UDisksBackend : public Backend
{
public:

    std::vector<Volume> volumes() override;
    void mount(const Volume &volume, Callback done) override;
    void unmount(const Volume &volume, bool force, Callback done) override;
    void unlockAndMount(const Volume &volume, const QString &passphrase, Callback done) override;
    void lock(const Volume &volume, Callback done) override;
    void safelyRemove(const Volume &volume, Callback done) override;

};
