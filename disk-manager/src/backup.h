#pragma once
#include "common.h"
#include "scanner.h"
#include <QLockFile>
#include <memory>

namespace dm {
struct BackupOptions {
    QString source, destination, volumeUuid, mode = "full";
    QStringList excludes{".cache",".local/share/Trash","node_modules",".thumbnails"};
    bool verify = true, allowLocal = false;
};
struct BackupPlan {
    BackupOptions options;
    ScanResult scan;
    qint64 available = 0, required = 0;
    QString error;
    bool ready() const { return error.isEmpty() && !scan.cancelled && available >= required; }
};
BackupPlan planBackup(const BackupOptions &options);
QJsonObject optionsJson(const BackupOptions &options);
BackupOptions optionsFromJson(const QJsonObject &json);

class BackupEngine : public QObject {
    Q_OBJECT
public:
    explicit BackupEngine(QObject *parent = nullptr);
    bool start(const BackupPlan &plan);
    bool running() const { return busy_; }
    void cancel();
    void pause(bool value);
    bool restore(const QString &backupFolder, const QString &relativePath, const QString &destination);
signals:
    void output(QString text);
    void finished(bool success, QString message);
private:
    void copied(bool success, const QString &detail);
    void complete(bool success, const QString &detail);
    ProcessJob job_;
    bool busy_ = false, restoring_ = false;
    BackupPlan plan_;
    QString folder_;
    std::unique_ptr<QLockFile> lock_;
};
QStringList completedBackups(const QString &destination);
bool saveSchedule(const BackupOptions &options, const QString &frequency, QString *error);
}
