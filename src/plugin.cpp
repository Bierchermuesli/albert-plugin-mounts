#include "plugin.h"
#include "udisksbackend.h"
#include <QInputDialog>
#include <QLocale>
#include <QStorageInfo>
#include <QTimer>
#include <albert/icon.h>
#include <albert/logging.h>
#include <albert/matcher.h>
#include <albert/notification.h>
#include <albert/querycontext.h>
#include <albert/standarditem.h>
#include <albert/systemutil.h>
ALBERT_LOGGING_CATEGORY("drives")
using namespace Qt::StringLiterals;
using namespace albert;
using namespace std;

namespace {

QString formatSize(qint64 bytes)
{ return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat); }

void notify(const QString &title, const QString &text)
{
    auto *n = new Notification(title, text);
    n->send();
    QTimer::singleShot(10000, n, &QObject::deleteLater);
}

QString volumeName(const Volume &v)
{
    if (!v.label.isEmpty())
        return v.label;
    const auto size = formatSize(qint64(v.size));
    return v.drive.isEmpty() ? size : u"%1 %2"_s.arg(size, v.drive);
}

QString volumeState(const Volume &v)
{
    if (!v.mount_points.isEmpty())
    {
        const auto &path = v.mount_points.constFirst();
        if (const QStorageInfo storage(path); storage.isValid() && storage.bytesTotal() > 0)
            return u"Mounted at %1 · %2 free of %3"_s.arg(path,
                                                          formatSize(storage.bytesAvailable()),
                                                          formatSize(storage.bytesTotal()));
        return u"Mounted at %1"_s.arg(path);
    }
    if (v.encrypted && !v.unlocked)
        return u"Encrypted, locked"_s;
    return u"Not mounted"_s;
}

QString iconName(const Volume &v)
{
    if (v.encrypted && !v.unlocked)
        return u"drive-harddisk-encrypted"_s;
    switch (v.kind) {
    case Volume::Kind::Usb:       return u"drive-removable-media-usb"_s;
    case Volume::Kind::Optical:   return u"media-optical"_s;
    case Volume::Kind::Removable: return u"drive-removable-media"_s;
    case Volume::Kind::Disk:      break;
    }
    return u"drive-harddisk"_s;
}

/// Returns a callback that reports errors and optionally opens the mount point.
Backend::Callback report(const QString &what, const QString &volume, bool open_after = false)
{
    return [=](const QString &error, const QString &mount_point) {
        if (!error.isEmpty())
        {
            WARN << u"%1 %2 failed: %3"_s.arg(what, volume, error);
            notify(u"%1 failed"_s.arg(what), u"%1: %2"_s.arg(volume, error));
        }
        else if (open_after && !mount_point.isEmpty())
            albert::open(mount_point);
    };
}

vector<Action> actions(const shared_ptr<Backend> &backend, const Volume &v)
{
    vector<Action> actions;
    const auto n = volumeName(v);

    if (!v.mount_points.isEmpty())
    {
        const auto path = v.mount_points.constFirst();
        actions.push_back({u"open"_s, u"Open"_s, [path]{ albert::open(path); }});
        actions.push_back({u"copy"_s, u"Copy path"_s, [path]{ setClipboardText(path); }});
        actions.push_back({u"unmount"_s, u"Unmount"_s,
                           [=]{ backend->unmount(v, false, report(u"Unmount"_s, n)); }});
        // Lazy unmount, detaches the filesystem even if programs still use it
        actions.push_back({u"forceunmount"_s, u"Force unmount"_s,
                           [=]{ backend->unmount(v, true, report(u"Force unmount"_s, n)); }});
    }
    else if (v.encrypted && !v.unlocked)
    {
        auto unlock = [=](bool open_after) {
            bool ok = false;
            const auto passphrase = QInputDialog::getText(
                nullptr, u"Unlock %1"_s.arg(n), u"Passphrase for %1 (%2):\n\n"
                u"The passphrase is passed directly to UDisks2 to unlock the drive.\n"
                u"Albert does not store it."_s.arg(n, v.device),
                QLineEdit::Password, {}, &ok);
            if (ok)
                backend->unlockAndMount(v, passphrase, report(u"Unlock"_s, n, open_after));
        };
        actions.push_back({u"unlockopen"_s, u"Unlock and open"_s, [=]{ unlock(true); }});
        actions.push_back({u"unlock"_s, u"Unlock and mount"_s, [=]{ unlock(false); }});
    }
    else
    {
        actions.push_back({u"mountopen"_s, u"Mount and open"_s,
                           [=]{ backend->mount(v, report(u"Mount"_s, n, true)); }});
        actions.push_back({u"mount"_s, u"Mount"_s,
                           [=]{ backend->mount(v, report(u"Mount"_s, n)); }});
    }

    if (v.encrypted && v.unlocked)
        actions.push_back({u"lock"_s, u"Lock"_s,
                           [=]{ backend->lock(v, report(u"Lock"_s, n)); }});

    if (v.can_safely_remove)
        actions.push_back({u"remove"_s, u"Safely remove"_s, [=]{
            backend->safelyRemove(v, [n](const QString &error, const QString &) {
                if (error.isEmpty())
                    notify(u"Safe to remove"_s, u"%1 can be removed now."_s.arg(n));
                else
                    report(u"Safely remove"_s, n)(error, {});
            });
        }});

    return actions;
}

}  // namespace

Plugin::Plugin() : backend_(make_shared<UDisksBackend>()) {}

Plugin::~Plugin() = default;

QString Plugin::defaultTrigger() const { return u"mount "_s; }

vector<RankItem> Plugin::rankItems(QueryContext &ctx)
{
    vector<RankItem> results;
    const Matcher matcher(ctx.query());

    for (const auto &v : backend_->volumes())
    {
        const auto n = volumeName(v);
        const auto match = matcher.match(QStringList{n, v.drive, v.device, u"drive"_s,
                                                     u"mount"_s, u"unmount"_s, u"eject"_s,
                                                     u"usb"_s, u"disk"_s});
        if (!match)
            continue;

        auto subtext = volumeState(v);
        if (!v.label.isEmpty() && !v.drive.isEmpty())
            subtext += u" · "_s + v.drive;

        results.emplace_back(
            StandardItem::make(v.id, n, subtext,
                               [icon = iconName(v)]{ return Icon::theme(icon); },
                               actions(backend_, v)),
            match.score());
    }

    return results;
}
