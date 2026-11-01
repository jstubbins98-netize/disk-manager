#include "backup.h"
#include "devices.h"
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>
#include <QtConcurrent>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace dm {
namespace {
QString repository(const QString &dest) { return QDir(dest).filePath("DiskManager"); }
bool safeRepository(const QString &dest, QString *error) {
    const QFileInfo info(repository(dest));
    if (info.isSymLink() || (info.exists() && (!info.isDir() || !within(canonical(info.filePath()), canonical(dest))))) {
        if(error) *error = "The backup repository must be a real directory, not a symbolic link.";
        return false;
    }
    return true;
}
QStringList rsyncArgs(const BackupOptions &o) {
    QStringList args{"--archive","--hard-links","--one-file-system","--no-owner","--no-group",
                     "--no-devices","--no-specials","--safe-links","--protect-args"};
    QStorageInfo destination(o.destination);
    if (QList<QByteArray>{"vfat","exfat","fuseblk","ntfs","ntfs3"}.contains(destination.fileSystemType()))
        args << "--no-perms" << "--omit-dir-times" << "--modify-window=2" << "--no-hard-links" << "--no-links";
    for (const auto &ex : o.excludes) {
        QString pattern = ex;
        if (ex.startsWith('/')) {
            if (!within(ex,o.source)) continue;
            pattern = "/" + QDir(o.source).relativeFilePath(ex);
        } else if (ex.contains('/')) pattern = "/" + QDir::cleanPath(ex);
        args << "--exclude=" + pattern;
    }
    if (canonical(o.source) == "/")
        for (const auto &ex : {"dev","proc","sys","run","tmp","mnt","media","lost+found"})
            args << "--exclude=/" + QString(ex) + "/***";
    return args;
}
QString externalDestinationUuid(const QString &path) {
    const auto devices = listDevices();
    if (!devices.error.isEmpty()) return {};
    QStorageInfo target(path); target.refresh();
    for (const auto &d : devices.items) if (d.external && !d.locked)
        for (const auto &m : d.mounts)
            if (canonical(m) == canonical(target.rootPath())) return d.uuid;
    return {};
}
QString verifyFiles(const ScanResult &scan, const QString &folder) {
    for (const auto &entry : scan.entries) {
        const QString rel = QDir(scan.root).relativeFilePath(entry.path);
        const QString dest = folder + "/data/" + rel;
        const auto sourceHash = checksum(entry.path);
        if (sourceHash.isEmpty() || sourceHash != checksum(dest))
            return "Checksum mismatch or unreadable file: " + rel + "\nSource files may have changed during backup.";
    }
    return {};
}
}
QJsonObject optionsJson(const BackupOptions &o) {
    return {{"source",o.source},{"destination",o.destination},{"volumeUuid",o.volumeUuid},{"mode",o.mode},
            {"excludes",QJsonArray::fromStringList(o.excludes)},{"verify",o.verify},{"allowLocal",o.allowLocal}};
}
BackupOptions optionsFromJson(const QJsonObject &j) {
    BackupOptions o;
    o.source = j["source"].toString(); o.destination = j["destination"].toString();
    o.volumeUuid = j["volumeUuid"].toString();
    o.mode = j["mode"].toString("full"); o.verify = j["verify"].toBool(true);
    o.allowLocal = j["allowLocal"].toBool(false);
    if (j["excludes"].isArray()) {
        o.excludes.clear(); for (auto e : j["excludes"].toArray()) o.excludes << e.toString();
    }
    return o;
}
BackupPlan planBackup(const BackupOptions &input) {
    BackupPlan p; p.options = input;
    auto &o = p.options;
    o.source = canonical(o.source); o.destination = canonical(o.destination);
    if (o.source.isEmpty() || o.destination.isEmpty() ||
        !QFileInfo(o.source).isDir() || !QFileInfo(o.destination).isDir()) {
        p.error = "Select an existing source folder and an existing destination folder."; return p;
    }
    if (!QStringList{"full","incremental","snapshot"}.contains(o.mode)) { p.error = "Invalid backup mode."; return p; }
    for (const auto &ex : o.excludes) {
        const QString cleaned = QDir::cleanPath(ex);
        if (ex.isEmpty() || ex.contains('*') || ex.contains('?') || ex.contains('[') ||
            ex.contains('\n') || cleaned == "." || cleaned == ".." || cleaned.startsWith("../") ||
            (ex.startsWith('/') && cleaned == o.source)) {
            p.error = "Exclusions must be literal file/directory names, source-relative paths, or absolute subfolder paths. Wildcards and parent-directory traversal are not supported.";
            return p;
        }
    }
    if (QStandardPaths::findExecutable("rsync").isEmpty()) { p.error = "Install rsync before creating backups."; return p; }
    if (!QFileInfo(o.destination).isWritable()) { p.error = "The destination is not writable."; return p; }
    struct stat source{}, dest{};
    if (::stat(QFile::encodeName(o.source).constData(),&source) ||
        ::stat(QFile::encodeName(o.destination).constData(),&dest)) { p.error = "Cannot inspect the source or destination."; return p; }
    if (within(o.source,o.destination) || (within(o.destination,o.source) && source.st_dev == dest.st_dev)) {
        p.error = "Source and destination must not overlap on the same filesystem."; return p;
    }
    if (!o.allowLocal) {
        const QString uuid = externalDestinationUuid(o.destination);
        if (uuid.isEmpty()) {
            p.error = "The destination is not a mounted, unprotected external drive with a filesystem UUID. Mount a drive in Devices first."; return p;
        }
        if (!o.volumeUuid.isEmpty() && o.volumeUuid != uuid) {
            p.error = "A different drive is mounted at the saved destination. Connect the original drive."; return p;
        }
        o.volumeUuid = uuid;
    }
    if (!safeRepository(o.destination,&p.error)) return p;
    struct statvfs space{};
    if (::statvfs(QFile::encodeName(o.destination).constData(),&space)) { p.error = "Cannot read destination free space."; return p; }
    p.available = static_cast<qint64>(space.f_bavail) * static_cast<qint64>(space.f_frsize);
    if (o.source == "/") {
        for (const auto &ex : {"/dev","/proc","/sys","/run","/tmp","/mnt","/media","/lost+found"})
            if (!o.excludes.contains(ex)) o.excludes << ex;
    }
    if (o.mode != "full") {
        QStorageInfo volume(o.destination);
        if (QList<QByteArray>{"vfat","exfat","fuseblk","ntfs","ntfs3"}.contains(volume.fileSystemType())) {
            p.error = "Use Full copy on this filesystem. Versioned hard-link backups require a Linux filesystem such as ext4, Btrfs, or XFS."; return p;
        }
    }
    p.scan = scanFolder(o.source,o.excludes);
    if (!p.scan.errors.isEmpty()) { p.error = "The source scan is incomplete. No backup will start.\n" + p.scan.errors.mid(0,8).join('\n'); return p; }
    // Conservative ceiling: never guess a compression ratio or assume hard-link savings.
    p.required = p.scan.total + p.scan.files * 4096 + 64LL*1024*1024;
    if (p.available < p.required)
        p.error = QString("Needs %1, but only %2 is available (%3 short).\nExclude large folders or choose another drive. Incremental backups may use less space, but this check deliberately reserves the full source size.")
            .arg(bytes(p.required),bytes(p.available),bytes(p.required-p.available));
    return p;
}
QStringList completedBackups(const QString &destination) {
    QString error;
    if (!safeRepository(destination,&error)) return {};
    QStringList result;
    QDir dir(repository(destination));
    for (const auto &name : dir.entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name)) {
        QString folder = dir.filePath(name);
        if (QFileInfo(folder).isSymLink()) continue;
        auto manifest = readJson(folder + "/manifest.json");
        if (manifest["status"].toString() == "complete" && QFileInfo::exists(folder + "/data")) result << folder;
    }
    return result;
}
BackupEngine::BackupEngine(QObject *parent) : QObject(parent), job_(this) {
    connect(&job_,&ProcessJob::output,this,&BackupEngine::output);
    connect(&job_,&ProcessJob::finished,this,&BackupEngine::copied);
}
bool BackupEngine::start(const BackupPlan &plan) {
    if (busy_) return false;
    if (!plan.ready()) { emit finished(false,plan.error); return false; }
    // Revalidate in a worker immediately before start; do not trust an old wizard result.
    busy_ = true; restoring_ = false;
    auto watcher = new QFutureWatcher<BackupPlan>(this);
    connect(watcher,&QFutureWatcher<BackupPlan>::finished,this,[this,watcher] {
        plan_ = watcher->result(); watcher->deleteLater();
        if (!plan_.ready()) { complete(false,plan_.error); return; }
        const QString repo = repository(plan_.options.destination);
        if (!QDir().mkpath(repo)) { complete(false,"Cannot create the backup repository."); return; }
        lock_ = std::make_unique<QLockFile>(repo + "/.lock");
        lock_->setStaleLockTime(0);
        if (!lock_->tryLock(0)) { complete(false,"Another backup is using this destination."); return; }
        folder_ = repo + "/" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")
                + "-" + QUuid::createUuid().toString(QUuid::Id128).left(8);
        if (!QDir().mkpath(folder_ + "/data")) { complete(false,"Cannot create the backup directory."); return; }
        QJsonObject manifest = optionsJson(plan_.options);
        manifest["status"] = "incomplete";
        manifest["created"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        manifest["files"] = plan_.scan.files; manifest["bytes"] = plan_.scan.total;
        QString error;
        if (!writeJson(folder_ + "/manifest.json",manifest,&error)) { complete(false,error); return; }
        auto args = rsyncArgs(plan_.options);
        if (plan_.options.mode != "full") {
            auto backups = completedBackups(plan_.options.destination);
            // Link only to a backup of precisely the same canonical source.
            for (auto it = backups.crbegin(); it != backups.crend(); ++it) {
                auto previous = readJson(*it + "/manifest.json");
                if (previous["source"].toString() == plan_.options.source) {
                    args << "--link-dest=" + *it + "/data"; break;
                }
            }
        }
        args << "--stats" << "--info=progress2,name1" << "--"
             << plan_.options.source + (plan_.options.source.endsWith('/') ? "" : "/") << folder_ + "/data/";
        emit output("Backup directory: " + folder_ + "\n");
        job_.start("rsync",args);
    });
    watcher->setFuture(QtConcurrent::run([options=plan.options]{ return planBackup(options); }));
    return true;
}
void BackupEngine::copied(bool success, const QString &detail) {
    if (restoring_) { complete(success,detail); return; }
    if (!success) { complete(false,detail); return; }
    if (!plan_.options.verify) { complete(true,detail); return; }
    emit output("\nCopy complete. Comparing SHA-256 checksums; please wait.\n");
    auto w = new QFutureWatcher<QString>(this);
    connect(w,&QFutureWatcher<QString>::finished,this,[this,w] {
        QString error = w->result(); w->deleteLater();
        complete(error.isEmpty(),error.isEmpty() ? "All scanned regular files passed SHA-256 verification." : error);
    });
    w->setFuture(QtConcurrent::run([scan=plan_.scan,folder=folder_] { return verifyFiles(scan,folder); }));
}
void BackupEngine::complete(bool success, const QString &detail) {
    QString finalDetail = detail;
    if (!restoring_ && !folder_.isEmpty()) {
        auto manifest = readJson(folder_ + "/manifest.json");
        manifest["status"] = success ? "complete" : "incomplete";
        manifest["finished"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        manifest["report"] = detail;
        manifest["verified"] = success && plan_.options.verify;
        QString error;
        if (!writeJson(folder_ + "/manifest.json",manifest,&error)) {
            success = false; finalDetail += "\nCannot save the backup report: " + error;
        }
        finalDetail += "\nBackup: " + folder_;
    }
    lock_.reset(); busy_ = false; folder_.clear();
    emit finished(success,finalDetail);
}
void BackupEngine::cancel() {
    if (job_.running()) job_.cancel();
    else emit output("Preparation or checksum verification is in progress. Cancellation is available while rsync is copying.\n");
}
void BackupEngine::pause(bool value) { job_.pause(value); }
bool BackupEngine::restore(const QString &backupFolder, const QString &relativePath, const QString &destination) {
    if (busy_) return false;
    const auto manifest = readJson(backupFolder + "/manifest.json");
    const QString root = canonical(backupFolder + "/data");
    const QString source = canonical(root + "/" + relativePath);
    const QString dest = canonical(destination);
    if (manifest["status"].toString() != "complete" || root.isEmpty() || source.isEmpty() || dest.isEmpty() ||
        !within(source,root) || within(dest,root) || within(root,dest) ||
        !QFileInfo(dest).isDir() || !QDir(dest).isEmpty()) {
        emit finished(false,"Choose a completed Disk Manager backup, a file inside it, and an empty destination folder outside the backup.");
        return false;
    }
    restoring_ = busy_ = true;
    const QString src = source + (QFileInfo(source).isDir() ? "/" : "");
    return job_.start("rsync",{"--archive","--no-owner","--no-group","--safe-links","--protect-args",
                             "--info=progress2,name1","--stats","--",src,dest + "/"});
}
bool saveSchedule(const BackupOptions &options, const QString &frequency, QString *error) {
    if (!QStringList{"daily","weekly"}.contains(frequency)) { *error = "Invalid schedule."; return false; }
    const QString config = stateDirectory() + "/scheduled-backup.json";
    if (!writeJson(config,optionsJson(options),error)) return false;
    // Unit-file quoting is not shell quoting: reject control characters and escape systemd specifiers.
    auto quote = [](QString s) {
        s.replace("\\","\\\\"); s.replace("\"","\\\""); s.replace("%","%%");
        return "\"" + s + "\"";
    };
    const QString exe = QCoreApplication::applicationFilePath();
    if (exe.contains('\n') || config.contains('\n')) { *error = "Paths with newlines are unsupported."; return false; }
    const QString unitDir = QDir::homePath() + "/.config/systemd/user";
    if (!QDir().mkpath(unitDir)) { *error = "Cannot create the user systemd directory."; return false; }
    const QString service = "[Unit]\nDescription=Disk Manager scheduled backup\n\n[Service]\nType=oneshot\nExecStart="
        + quote(exe) + " --backup-config " + quote(config) + "\n";
    const QString timer = "[Unit]\nDescription=Disk Manager backup timer\n\n[Timer]\nOnCalendar=" + frequency
        + "\nPersistent=true\n\n[Install]\nWantedBy=timers.target\n";
    for (const auto &file : {qMakePair("diskmanager-backup.service",service),qMakePair("diskmanager-backup.timer",timer)}) {
        QSaveFile out(unitDir + "/" + file.first);
        if (!out.open(QIODevice::WriteOnly) || out.write(file.second.toUtf8()) < 0 || !out.commit()) { *error = out.errorString(); return false; }
    }
    auto reload = command("systemctl",{"--user","daemon-reload"});
    if (!reload.ok()) { *error = "Units saved, but systemd could not reload: " + reload.error; return false; }
    auto enable = command("systemctl",{"--user","enable","--now","diskmanager-backup.timer"});
    if (!enable.ok()) { *error = "Units saved, but timer could not start: " + enable.error; return false; }
    return true;
}
}
