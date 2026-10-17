#include "procfs.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
using namespace Qt::StringLiterals;

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
