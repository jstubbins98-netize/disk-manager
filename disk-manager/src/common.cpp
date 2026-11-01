#include "common.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <csignal>

namespace dm {
CommandResult command(const QString &program, const QStringList &args, int timeout) {
    QProcess p;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL","C");
    p.setProcessEnvironment(environment);
    p.start(program, args);
    if (!p.waitForStarted(3000)) return {-1, {}, p.errorString()};
    if (!p.waitForFinished(timeout)) {
        p.kill(); p.waitForFinished();
        return {-1, {}, "Command timed out: " + program};
    }
    return {p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1,
            QString::fromUtf8(p.readAllStandardOutput()), QString::fromUtf8(p.readAllStandardError())};
}
QString bytes(qint64 value, bool binary) {
    const double base = binary ? 1024.0 : 1000.0;
    const QStringList units = binary ? QStringList{"B","KiB","MiB","GiB","TiB","PiB"}
                                     : QStringList{"B","kB","MB","GB","TB","PB"};
    double n = value; int i = 0;
    while (n >= base && i < units.size()-1) { n /= base; ++i; }
    return QString::number(n, 'f', i ? 1 : 0) + " " + units[i];
}
QString canonical(const QString &path) { return QFileInfo(path).canonicalFilePath(); }
bool within(const QString &path, const QString &parent) {
    const QString p = QDir::cleanPath(path), r = QDir::cleanPath(parent);
    return p == r || p.startsWith(r == "/" ? "/" : r + "/");
}
QString stateDirectory() {
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(path); return path;
}
bool writeJson(const QString &path, const QJsonObject &object, QString *error) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(object).toJson()) < 0 || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}
QJsonObject readJson(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
QString checksum(const QString &path, QString *error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if(error) *error = f.errorString(); return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) { if(error) *error = "Unable to read file"; return {}; }
    return QString::fromLatin1(hash.result().toHex());
}
ProcessJob::ProcessJob(QObject *parent) : QObject(parent) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        emit output(QString::fromUtf8(process_.readAllStandardOutput()));
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(process_.readAllStandardError());
        errors_ += text; errors_ = errors_.right(32000); emit output(text);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) emit finished(false, process_.errorString());
    });
    connect(&process_, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        emit finished(!cancelled_ && code == 0 && status == QProcess::NormalExit,
                      cancelled_ ? "Cancelled. Partial files have been kept; this is not a completed backup."
                                 : (code == 0 ? "Completed successfully." : QString("Exit %1: %2").arg(code).arg(errors_)));
    });
}
bool ProcessJob::start(const QString &program, const QStringList &args) {
    if (running()) return false;
    cancelled_ = paused_ = false; errors_.clear();
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL","C");
    process_.setProcessEnvironment(environment);
    process_.start(program, args); return true;
}
bool ProcessJob::running() const { return process_.state() != QProcess::NotRunning; }
void ProcessJob::cancel() {
    if (!running()) return;
    cancelled_ = true;
    if (paused_) pause(false);
    process_.terminate();
}
void ProcessJob::pause(bool paused) {
    if (process_.processId() > 0) {
        ::kill(static_cast<pid_t>(process_.processId()), paused ? SIGSTOP : SIGCONT);
        paused_ = paused;
    }
}
}
