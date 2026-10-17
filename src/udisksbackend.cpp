#include "udisksbackend.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QTimer>
#include <albert/logging.h>
#include <memory>
using namespace Qt::StringLiterals;
using namespace std;

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

using Done = function<void(const QString &error, const QDBusMessage &reply)>;

void call(const QString &path, const QString &interface, const QString &method,
          const QVariantList &args, Done done)
{
    auto msg = QDBusMessage::createMethodCall(service, path, interface, method);
    msg.setArguments(args);

    // Generous timeout, e.g. polkit authentication or slow drives
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, 120000));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [watcher, done]{
        watcher->deleteLater();
        if (watcher->isError())
            done(watcher->error().message(), {});
        else
            done({}, watcher->reply());
    });
}

// The filesystem interface of a freshly unlocked device may appear a bit later.
void mountWithRetry(const QString &path, int attempts, Backend::Callback done)
{
    call(path, filesystem_iface, u"Mount"_s, {QVariantMap{}},
         [=](const QString &error, const QDBusMessage &reply) {
        if (!error.isEmpty() && attempts > 1)
            QTimer::singleShot(500, [=]{ mountWithRetry(path, attempts - 1, done); });
        else if (!error.isEmpty())
            done(error, {});
        else
            done({}, reply.arguments().value(0).toString());
    });
}

// Returns the names of the processes using files below _mount_point_. Only processes of the
// current user are visible, but those are the ones the user can close anyway.
QStringList blockingProcesses(const QString &mount_point)
{
    const auto prefix = mount_point.endsWith(u'/') ? mount_point : mount_point + u'/';
    auto below = [&](const QString &path) { return path == mount_point || path.startsWith(prefix); };

    QStringList names;
    for (const auto &pid : QDir(u"/proc"_s).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        if (!pid.front().isDigit())
            continue;

        const QDir proc(u"/proc/"_s + pid);
        bool uses = below(QFileInfo(proc.filePath(u"cwd"_s)).symLinkTarget());
        if (!uses)
            for (const auto &fd : QDir(proc.filePath(u"fd"_s)).entryInfoList(QDir::System))
                if (below(fd.symLinkTarget()))
                {
                    uses = true;
                    break;
                }

        if (uses)
            if (QFile comm(proc.filePath(u"comm"_s)); comm.open(QIODevice::ReadOnly))
                if (const auto name = QString::fromUtf8(comm.readAll()).trimmed();
                    !names.contains(name))
                    names << name;
    }
    return names;
}

// Unmounts the filesystem at _path_. If it is busy, the error names the blocking processes.
void unmountFilesystem(const QString &path, const QString &mount_point, bool force,
                       function<void(const QString &error)> done)
{
    QVariantMap options;
    if (force)
        options.insert(u"force"_s, true);

    call(path, filesystem_iface, u"Unmount"_s, {options},
         [=](const QString &error, const QDBusMessage &) {
        if (error.isEmpty() || !error.contains(u"busy"_s))
            return done(error);
        const auto processes = blockingProcesses(mount_point);
        done(processes.isEmpty() ? error
                                 : u"The drive is used by: %1"_s.arg(processes.join(u", "_s)));
    });
}

using Step = function<void(function<void(const QString &error)> next)>;

void runSteps(shared_ptr<vector<Step>> steps, size_t index, Backend::Callback done)
{
    if (index == steps->size())
    {
        done({}, {});
        return;
    }
    (*steps)[index]([=](const QString &error) {
        if (!error.isEmpty())
            done(error, {});
        else
            runSteps(steps, index + 1, done);
    });
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

void UDisksBackend::mount(const Volume &volume, Callback done)
{
    const auto path = filesystemPath(managedObjects(), volume.id);
    if (path == u"/"_s)
        return done(u"The volume is locked."_s, {});
    mountWithRetry(path, 1, done);
}

void UDisksBackend::unmount(const Volume &volume, bool force, Callback done)
{
    const auto path = filesystemPath(managedObjects(), volume.id);
    unmountFilesystem(path, volume.mount_points.value(0), force,
                      [done](const QString &error) { done(error, {}); });
}

void UDisksBackend::unlockAndMount(const Volume &volume, const QString &passphrase, Callback done)
{
    call(volume.id, encrypted_iface, u"Unlock"_s, {passphrase, QVariantMap{}},
         [done](const QString &error, const QDBusMessage &reply) {
        if (!error.isEmpty())
            return done(error, {});
        mountWithRetry(objectPath(reply.arguments().value(0)), 6, done);
    });
}

void UDisksBackend::lock(const Volume &volume, Callback done)
{
    auto steps = make_shared<vector<Step>>();
    if (!volume.mount_points.isEmpty())
        steps->push_back([this, volume](auto next) {
            unmount(volume, false, [next](const QString &error, const QString &) { next(error); });
        });
    steps->push_back([=](auto next) {
        call(volume.id, encrypted_iface, u"Lock"_s, {QVariantMap{}},
             [next](const QString &error, const QDBusMessage &) { next(error); });
    });
    runSteps(steps, 0, done);
}

void UDisksBackend::safelyRemove(const Volume &volume, Callback done)
{
    const auto objects = managedObjects();
    const auto drive_path = objectPath(objects.value(volume.id).value(block_iface).value(u"Drive"_s));
    const auto drive = objects.value(drive_path).value(drive_iface);

    auto steps = make_shared<vector<Step>>();

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
        const auto points = fs == u"/"_s ? QStringList{}
            : mountPoints(objects.value(fs).value(filesystem_iface).value(u"MountPoints"_s));
        if (!points.isEmpty())
            steps->push_back([fs, point = points.constFirst()](auto next) {
                unmountFilesystem(fs, point, false, next);
            });

        if (it.value().contains(encrypted_iface) && fs != u"/"_s)
            steps->push_back([path](auto next) {
                call(path, encrypted_iface, u"Lock"_s, {QVariantMap{}},
                     [next](const QString &error, const QDBusMessage &) { next(error); });
            });
    }

    if (drive.value(u"CanPowerOff"_s).toBool())
        steps->push_back([drive_path](auto next) {
            call(drive_path, drive_iface, u"PowerOff"_s, {QVariantMap{}},
                 [next](const QString &error, const QDBusMessage &) { next(error); });
        });
    else if (drive.value(u"Ejectable"_s).toBool())
        steps->push_back([drive_path](auto next) {
            call(drive_path, drive_iface, u"Eject"_s, {QVariantMap{}},
                 [next](const QString &error, const QDBusMessage &) { next(error); });
        });

    runSteps(steps, 0, done);
}
