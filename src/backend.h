#pragma once
#include <QString>
#include <QStringList>
#include <qcoro/qcorotask.h>
#include <vector>

///
/// A mountable volume, e.g. a partition of a USB stick or an encrypted container.
///
struct Volume
{
    enum class Kind { Usb, Optical, Removable, Disk, Network };

    QString id;           ///< Backend specific identifier
    QString label;        ///< Filesystem label, may be empty
    QString drive;        ///< Human readable drive name, e.g. "Seagate BUP BK"
    QString device;       ///< Device file, e.g. /dev/sdb1
    QString source;       ///< Remote source of a network share, e.g. user@host:/path
    quint64 size = 0;     ///< Bytes
    Kind kind = Kind::Disk;
    bool encrypted = false;
    bool unlocked = false;        ///< Only meaningful if encrypted
    QStringList mount_points;     ///< Empty if not mounted
    bool can_safely_remove = false;
};

///
/// Result of an operation. The error is empty on success. Mounting also returns the mount point.
///
struct Result
{
    QString error;
    QString mount_point;
};

///
/// Platform specific access to the volumes.
///
/// The operations are coroutines running on the main thread. Volumes are passed by value, the
/// caller may be gone before the operation finished.
///
class Backend
{
public:

    virtual ~Backend() = default;

    /// Returns the volumes that are of interest for the user, i.e. no system volumes.
    /// Thread-safe, called from worker threads.
    virtual std::vector<Volume> volumes() = 0;

    virtual QCoro::Task<Result> mount(Volume volume) = 0;

    /// Unmounts the volume. _force_ detaches it even if it is busy (lazy unmount).
    virtual QCoro::Task<Result> unmount(Volume volume, bool force) = 0;

    /// Unlocks an encrypted volume and mounts it.
    virtual QCoro::Task<Result> unlockAndMount(Volume volume, QString passphrase) = 0;

    virtual QCoro::Task<Result> lock(Volume volume) = 0;

    /// Unmounts and locks all volumes of the drive, then powers it off or ejects the medium.
    virtual QCoro::Task<Result> safelyRemove(Volume volume) = 0;
};
