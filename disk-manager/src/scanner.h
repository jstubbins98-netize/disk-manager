#pragma once
#include "common.h"
#include <QMap>
#include <QVector>
#include <atomic>
#include <memory>

namespace dm {
struct FileEntry { QString path; qint64 size = 0; qint64 accessed = 0; };
struct ScanResult {
    QString root;
    qint64 total = 0, files = 0;
    QMap<QString,qint64> categories, folders;
    QVector<FileEntry> entries;
    QStringList errors;
    bool cancelled = false;
};
QString category(const QString &suffix);
ScanResult scanFolder(const QString &root, const QStringList &excluded = {},
                      const std::shared_ptr<std::atomic_bool> &cancel = {});
QVector<QStringList> findDuplicates(const ScanResult &scan,
                                   const std::shared_ptr<std::atomic_bool> &cancel = {});
}
