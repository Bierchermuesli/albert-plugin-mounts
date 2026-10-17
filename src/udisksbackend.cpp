#include "procfs.h"
#include "udisksbackend.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QMap>
#include <albert/logging.h>
#include <qcoro/qcorodbuspendingcall.h>
#include <qcoro/qcorotimer.h>
using namespace Qt::StringLiterals;
using namespace std;
using namespace std::chrono_literals;

namespace {

const auto service = u"org.freedesktop.UDisks2"_s;
const auto block_iface = u"org.freedesktop.UDisks2.Block"_s;
const auto filesystem_iface = u"org.freedesktop.UDisks2.Filesystem"_s;
const auto encrypted_iface = u"org.freedesktop.UDisks2.Encrypted"_s;
const auto drive_iface = u"org.freedesktop.UDisks2.Drive"_s;

using Interfaces = QMap<QString, QVariantMap>;
using Objects = QMap<QString, Interfaces>;  // object path -> interfaces

Objects managedObjects()
{
    auto msg = QDBusMessage::createMethodCall(service, u"/org/freedesktop/UDisks2"_s,
                                              u"org.freedesktop.DBus.ObjectManager"_s,
                                              u"GetManagedObjects"_s);
    const auto reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
    {
        WARN << "Failed to get the UDisks2 objects:" << reply.errorMessage();
        return {};
    }

    // a{oa{sa{sv}}}
    Objects objects;
    const auto arg = reply.arguments().constFirst().value<QDBusArgument>();
    arg.beginMap();
    while (!arg.atEnd())
    {
        QDBusObjectPath path;
        Interfaces interfaces;
        arg.beginMapEntry();
        arg >> path;
        arg.beginMap();
        while (!arg.atEnd())
        {
            QString interface;
            QVariantMap properties;
            arg.beginMapEntry();
            arg >> interface >> properties;
            arg.endMapEntry();
            interfaces.insert(interface, properties);
        }
        arg.endMap();
        arg.endMapEntry();
        objects.insert(path.path(), interfaces);
    }
    arg.endMap();
    return objects;
}

QString objectPath(const QVariant &v) { return v.value<QDBusObjectPath>().path(); }

// Byte strings are null terminated
QString byteString(const QVariant &v)
{
    auto bytes = v.toByteArray();
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    return QString::fromLocal8Bit(bytes);
}

// aay
QStringList mountPoints(const QVariant &v)
{
    QStringList list;
    if (!v.canConvert<QDBusArgument>())
        return list;
    const auto arg = v.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd())
    {
        QByteArray bytes;
        arg >> bytes;
        list << byteString(bytes);
    }
    arg.endArray();
    return list;
}

/// Returns the object path of the filesystem of _volume_, i.e. the cleartext device if encrypted.
QString filesystemPath(const Objects &objects, const QString &volume)
{
    const auto &interfaces = objects.value(volume);
    if (interfaces.contains(encrypted_iface))
        return objectPath(interfaces.value(encrypted_iface).value(u"CleartextDevice"_s));
    return volume;
}

QCoro::Task<QDBusMessage> call(QString path, QString interface, QString method, QVariantList args)
{
    auto msg = QDBusMessage::createMethodCall(service, path, interface, method);
    msg.setArguments(args);
    // Generous timeout, e.g. polkit authentication or slow drives
    co_return co_await QDBusConnection::systemBus().asyncCall(msg, 120000);
}

/// Returns the error of a reply, empty on success.
QString error(const QDBusMessage &reply)
{
    if (reply.type() == QDBusMessage::ReplyMessage)
        return {};
    return reply.errorMessage().isEmpty() ? u"Unknown error"_s : reply.errorMessage();
}

QCoro::Task<Result> mountWithRetry(QString path, int attempts)
{
    for (int attempt = 1;; ++attempt)
    {
        const auto reply = co_await call(path, filesystem_iface, u"Mount"_s, {QVariantMap{}});
        if (const auto err = error(reply); err.isEmpty())
            co_return Result{{}, reply.arguments().value(0).toString()};
        else if (attempt == attempts)
            co_return Result{err, {}};

        // The filesystem interface of a freshly unlocked device may appear a bit later
        co_await QCoro::sleepFor(500ms);
    }
}

// Unmounts the filesystem at _path_. If it is busy, the error names the blocking processes.
QCoro::Task<QString> unmountFilesystem(QString path, QString mount_point, bool force)
{
    QVariantMap options;
    if (force)
        options.insert(u"force"_s, true);

    const auto err = error(co_await call(path, filesystem_iface, u"Unmount"_s, {options}));
    if (err.isEmpty() || !err.contains(u"busy"_s))
        co_return err;

    const auto processes = blockingProcesses(mount_point);
    co_return processes.isEmpty() ? err
                                  : u"The drive is used by: %1"_s.arg(processes.join(u", "_s));
}

}  // namespace

vector<Volume> UDisksBackend::volumes()
{
    const auto objects = managedObjects();

    vector<Volume> volumes;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it)
    {
        const auto &interfaces = it.value();
        if (!interfaces.contains(block_iface))
            continue;

        const auto block = interfaces.value(block_iface);
        if (block.value(u"HintIgnore"_s).toBool() || block.value(u"HintSystem"_s).toBool())
            continue;

        const auto drive_path = objectPath(block.value(u"Drive"_s));
        if (drive_path == u"/"_s)  // e.g. loop devices
            continue;

        // Cleartext devices are represented by their encrypted container
        if (objectPath(block.value(u"CryptoBackingDevice"_s)) != u"/"_s)
            continue;

        const bool encrypted = interfaces.contains(encrypted_iface);
        if (!encrypted && !interfaces.contains(filesystem_iface))
            continue;

        const auto drive = objects.value(drive_path).value(drive_iface);

        Volume v;
        v.id = it.key();
        v.label = block.value(u"IdLabel"_s).toString();
        v.device = byteString(block.value(u"Device"_s));
        v.size = block.value(u"Size"_s).toULongLong();
        v.drive = u"%1 %2"_s.arg(drive.value(u"Vendor"_s).toString(),
                                 drive.value(u"Model"_s).toString()).trimmed();

        const auto media = drive.value(u"MediaCompatibility"_s).toStringList();
        if (any_of(media.begin(), media.end(), [](const QString &m){ return m.startsWith(u"optical"_s); }))
            v.kind = Volume::Kind::Optical;
        else if (drive.value(u"ConnectionBus"_s).toString() == u"usb"_s)
            v.kind = Volume::Kind::Usb;
        else if (drive.value(u"Removable"_s).toBool() || drive.value(u"MediaRemovable"_s).toBool())
            v.kind = Volume::Kind::Removable;

        v.can_safely_remove = drive.value(u"CanPowerOff"_s).toBool()
                              || drive.value(u"Ejectable"_s).toBool();

        if (encrypted)
        {
            v.encrypted = true;
            const auto cleartext = filesystemPath(objects, v.id);
            v.unlocked = cleartext != u"/"_s;
            if (v.unlocked)
            {
                const auto &ct = objects.value(cleartext);
                v.mount_points = mountPoints(ct.value(filesystem_iface).value(u"MountPoints"_s));
                if (v.label.isEmpty())
                    v.label = ct.value(block_iface).value(u"IdLabel"_s).toString();
            }
        }
        else
            v.mount_points = mountPoints(interfaces.value(filesystem_iface).value(u"MountPoints"_s));

        volumes.emplace_back(::move(v));
    }
    return volumes;
}

QCoro::Task<Result> UDisksBackend::mount(Volume volume)
{
    const auto path = filesystemPath(managedObjects(), volume.id);
    if (path == u"/"_s)
        co_return Result{u"The volume is locked."_s, {}};
    co_return co_await mountWithRetry(path, 1);
}

QCoro::Task<Result> UDisksBackend::unmount(Volume volume, bool force)
{
    const auto path = filesystemPath(managedObjects(), volume.id);
    co_return Result{co_await unmountFilesystem(path, volume.mount_points.value(0), force), {}};
}

QCoro::Task<Result> UDisksBackend::unlockAndMount(Volume volume, QString passphrase)
{
    const auto reply = co_await call(volume.id, encrypted_iface, u"Unlock"_s,
                                     {passphrase, QVariantMap{}});
    if (const auto err = error(reply); !err.isEmpty())
        co_return Result{err, {}};
    co_return co_await mountWithRetry(objectPath(reply.arguments().value(0)), 6);
}

QCoro::Task<Result> UDisksBackend::lock(Volume volume)
{
    if (!volume.mount_points.isEmpty())
        if (const auto result = co_await unmount(volume, false); !result.error.isEmpty())
            co_return result;

    const auto reply = co_await call(volume.id, encrypted_iface, u"Lock"_s, {QVariantMap{}});
    co_return Result{error(reply), {}};
}

QCoro::Task<Result> UDisksBackend::safelyRemove(Volume volume)
{
    const auto objects = managedObjects();
    const auto drive_path = objectPath(objects.value(volume.id).value(block_iface).value(u"Drive"_s));
    const auto drive = objects.value(drive_path).value(drive_iface);

    // Unmount and lock everything on this drive
    for (auto it = objects.cbegin(); it != objects.cend(); ++it)
    {
        const auto block = it.value().value(block_iface);
        if (objectPath(block.value(u"Drive"_s)) != drive_path)
            continue;

        if (objectPath(block.value(u"CryptoBackingDevice"_s)) != u"/"_s)
            continue;  // handled with the container

        const auto path = it.key();
        const auto fs = filesystemPath(objects, path);
        if (fs == u"/"_s)
            continue;  // locked container

        const auto points = mountPoints(objects.value(fs).value(filesystem_iface)
                                            .value(u"MountPoints"_s));
        if (!points.isEmpty())
            if (const auto err = co_await unmountFilesystem(fs, points.constFirst(), false);
                !err.isEmpty())
                co_return Result{err, {}};

        if (it.value().contains(encrypted_iface))
        {
            const auto reply = co_await call(path, encrypted_iface, u"Lock"_s, {QVariantMap{}});
            if (const auto err = error(reply); !err.isEmpty())
                co_return Result{err, {}};
        }
    }

    if (drive.value(u"CanPowerOff"_s).toBool())
    {
        const auto reply = co_await call(drive_path, drive_iface, u"PowerOff"_s, {QVariantMap{}});
        co_return Result{error(reply), {}};
    }

    if (drive.value(u"Ejectable"_s).toBool())
    {
        const auto reply = co_await call(drive_path, drive_iface, u"Eject"_s, {QVariantMap{}});
        co_return Result{error(reply), {}};
    }

    co_return Result{};
}
