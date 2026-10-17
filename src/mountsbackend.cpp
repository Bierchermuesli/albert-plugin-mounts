#include "mountsbackend.h"
#include "procfs.h"
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <qcoro/qcoroprocess.h>
using namespace Qt::StringLiterals;
using namespace std;

namespace {

// Network shares and user space filesystems the user manages on their own
const QStringList network_filesystems{
    u"fuse.sshfs"_s, u"fuse.rclone"_s, u"fuse.curlftpfs"_s, u"fuse.davfs2"_s, u"davfs"_s,
    u"nfs"_s, u"nfs4"_s, u"cifs"_s, u"smb3"_s,
};

struct Mount
{
    QString source;
    QString target;
    QString fstype;
    QString options;
};

bool isNetwork(const Mount &m)
{
    return network_filesystems.contains(m.fstype)
           || m.options.split(u',').contains(u"_netdev"_s);
}

// The mount table and fstab escape spaces and such as octal sequences, e.g. \040
QString unescape(const QString &s)
{
    static const QRegularExpression re(uR"(\\([0-7]{3}))"_s);
    QString out;
    qsizetype last = 0;
    for (auto it = re.globalMatch(s); it.hasNext();)
    {
        const auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        out += QChar(m.captured(1).toInt(nullptr, 8));
        last = m.capturedEnd();
    }
    return out + s.mid(last);
}

vector<Mount> mountTable()
{
    vector<Mount> mounts;
    QFile file(u"/proc/self/mountinfo"_s);
    if (!file.open(QIODevice::ReadOnly))
        return mounts;

    // 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
    for (const auto &line : QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts))
    {
        const auto sep = line.indexOf(u" - "_s);
        if (sep < 0)
            continue;
        const auto pre = line.left(sep).split(u' ');
        const auto post = line.mid(sep + 3).split(u' ');
        if (pre.size() < 6 || post.size() < 3)
            continue;
        mounts.push_back({unescape(post[1]), unescape(pre[4]), post[0], pre[5]});
    }
    return mounts;
}

vector<Mount> fstab()
{
    vector<Mount> entries;
    QFile file(u"/etc/fstab"_s);
    if (!file.open(QIODevice::ReadOnly))
        return entries;

    static const QRegularExpression whitespace(uR"(\s+)"_s);
    for (const auto &line : QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts))
    {
        if (line.trimmed().startsWith(u'#'))
            continue;
        const auto fields = line.split(whitespace, Qt::SkipEmptyParts);
        if (fields.size() < 4)
            continue;
        entries.push_back({unescape(fields[0]), unescape(fields[1]), fields[2], fields[3]});
    }
    return entries;
}

Volume makeVolume(const Mount &m, bool mounted)
{
    Volume v;
    v.id = m.target;
    v.source = m.source;
    v.drive = m.fstype.startsWith(u"fuse."_s) ? m.fstype.mid(5) : m.fstype;
    v.kind = Volume::Kind::Network;
    if (mounted)
        v.mount_points << m.target;
    return v;
}

/// Runs _command_ and returns its error output, empty on success. If the user is not allowed
/// to, the command is run again with pkexec, which asks for authentication.
QCoro::Task<QString> run(QStringList command, bool elevate = true)
{
    QProcess process;
    auto coro = qCoro(process);
    co_await coro.start(command.first(), command.mid(1));
    co_await coro.waitForFinished(120'000);  // the authentication dialog may take a while

    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        co_return {};

    auto error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (error.isEmpty())
        error = u"%1 failed with exit code %2"_s.arg(command.first()).arg(process.exitCode());

    static const QRegularExpression permission(u"only root|permission|not permitted|superuser"_s,
                                               QRegularExpression::CaseInsensitiveOption);
    if (elevate && permission.match(error).hasMatch())
        co_return co_await run(QStringList{u"pkexec"_s} + command, false);

    co_return error;
}

}  // namespace

vector<Volume> MountsBackend::volumes()
{
    vector<Volume> volumes;
    QStringList mounted;
    for (const auto &m : mountTable())
        if (isNetwork(m))
        {
            mounted << m.target;
            volumes.push_back(makeVolume(m, true));
        }

    for (const auto &m : fstab())
        if (isNetwork(m) && !mounted.contains(m.target))
            volumes.push_back(makeVolume(m, false));

    return volumes;
}

QCoro::Task<Result> MountsBackend::mount(Volume volume)
{
    const auto error = co_await run({u"mount"_s, volume.id});
    co_return Result{error, error.isEmpty() ? volume.id : QString()};
}

QCoro::Task<Result> MountsBackend::unmount(Volume volume, bool force)
{
    // Look the filesystem up again, the volume may be stale
    QString fstype;
    for (const auto &m : mountTable())
        if (m.target == volume.id)
            fstype = m.fstype;
    if (fstype.isEmpty())
        co_return Result{u"Not mounted"_s, {}};

    QStringList command;
    if (fstype.startsWith(u"fuse."_s))
    {
        // FUSE filesystems are unmounted by their owner
        auto fusermount = QStandardPaths::findExecutable(u"fusermount3"_s);
        if (fusermount.isEmpty())
            fusermount = QStandardPaths::findExecutable(u"fusermount"_s);
        command = {fusermount.isEmpty() ? u"fusermount3"_s : fusermount, u"-u"_s};
        if (force)
            command << u"-z"_s;
    }
    else
    {
        command = {u"umount"_s};
        if (force)
            command << u"-l"_s;
    }
    command << volume.id;

    auto error = co_await run(command);
    if (!error.isEmpty() && error.contains(u"busy"_s))
        if (const auto processes = blockingProcesses(volume.id); !processes.isEmpty())
            error = u"The share is used by: %1"_s.arg(processes.join(u", "_s));

    co_return Result{error, {}};
}

QCoro::Task<Result> MountsBackend::unlockAndMount(Volume, QString)
{ co_return Result{u"Not supported"_s, {}}; }

QCoro::Task<Result> MountsBackend::lock(Volume)
{ co_return Result{u"Not supported"_s, {}}; }

QCoro::Task<Result> MountsBackend::safelyRemove(Volume)
{ co_return Result{u"Not supported"_s, {}}; }
