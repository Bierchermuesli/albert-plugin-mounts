#pragma once
#include <QString>
#include <QStringList>

/// Returns the names of the processes using files below _mount_point_. Only processes of the
/// current user are visible, but those are the ones the user can close anyway.
QStringList blockingProcesses(const QString &mount_point);
