#pragma once
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <functional>

namespace dm {
struct CommandResult {
    int code = -1;
    QString out, error;
    bool ok() const { return code == 0; }
};
CommandResult command(const QString &program, const QStringList &args, int timeout = 30000);
QString bytes(qint64 value, bool binary = true);
bool within(const QString &path, const QString &parent);
QString canonical(const QString &path);
QString stateDirectory();
bool writeJson(const QString &path, const QJsonObject &object, QString *error = nullptr);
QJsonObject readJson(const QString &path);
QString checksum(const QString &path, QString *error = nullptr);

// One asynchronous operation at a time. Never invokes a shell.
class ProcessJob : public QObject {
    Q_OBJECT
public:
    explicit ProcessJob(QObject *parent = nullptr);
    bool start(const QString &program, const QStringList &arguments);
    bool running() const;
    void cancel();
    void pause(bool paused);
signals:
    void output(QString text);
    void finished(bool success, QString detail);
private:
    QProcess process_;
    QString errors_;
    bool cancelled_ = false;
    bool paused_ = false;
};
}
