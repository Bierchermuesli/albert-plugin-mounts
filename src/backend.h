#pragma once
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

///
/// A mountable volume, e.g. a partition of a USB stick or an encrypted container.
///
struct Volume
{
    enum class Kind { Usb, Optical, Removable, Disk };

    QString id;           ///< Backend specific identifier
    QString label;        ///< Filesystem label, may be empty
    QString drive;        ///< Human readable drive name, e.g. "Seagate BUP BK"
    QString device;       ///< Device file, e.g. /dev/sdb1
    quint64 size = 0;     ///< Bytes
    Kind kind = Kind::Disk;
    bool encrypted = false;
    bool unlocked = false;        ///< Only meaningful if encrypted
    QStringList mount_points;     ///< Empty if not mounted
    bool can_safely_remove = false;
};

///
/// Platform specific access to the volumes.
///
/// All operations are asynchronous. The callback receives an error message, which is empty on
/// success. Mount and unlock pass the mount point on success.
///
class Backend
{
public:

    using Callback = std::function<void(const QString &error, const QString &result)>;

    virtual ~Backend() = default;

    /// Returns the volumes that are of interest for the user, i.e. no system volumes.
    /// Thread-safe, called from worker threads.
    virtual std::vector<Volume> volumes() = 0;

    virtual void mount(const Volume &volume, Callback done) = 0;
    /// Unmounts the volume. _force_ detaches it even if it is busy (lazy unmount).
    virtual void unmount(const Volume &volume, bool force, Callback done) = 0;

    /// Unlocks an encrypted volume and mounts it.
    virtual void unlockAndMount(const Volume &volume, const QString &passphrase,
                                Callback done) = 0;

    virtual void lock(const Volume &volume, Callback done) = 0;

    /// Unmounts and locks all volumes of the drive, then powers it off or ejects the medium.
    virtual void safelyRemove(const Volume &volume, Callback done) = 0;
};
