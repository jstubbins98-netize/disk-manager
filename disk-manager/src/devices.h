#pragma once
#include "common.h"
#include <QVector>

namespace dm {
struct Device {
    QString path, name, label, fs, uuid, model, serial, type, disk, objectPath, driveObject;
    QStringList mounts;
    qint64 size = 0;
    bool external = false, locked = true, readOnly = true;
    QString identity() const;
    QString confirmation() const { return label.isEmpty() ? name : label; }
};
struct DeviceList { QVector<Device> items; QString error; };
DeviceList parseDevices(const QByteArray &json);
DeviceList listDevices();
QString formatBlockReason(const Device &device, const QVector<Device> &all);

class DeviceService : public QObject {
    Q_OBJECT
public:
    explicit DeviceService(QObject *parent = nullptr) : QObject(parent) {}
    void mount(const Device &device, bool readOnly);
    void unmount(const Device &device);
    void safelyRemove(const Device &device);
    void format(const Device &device, const QString &fs, const QString &label,
                bool zeroFill, const QString &confirmation);
    QString health(const Device &device);
signals:
    void finished(bool success, QString message);
private:
    void call(const QString &path, const QString &interface, const QString &method, const QVariantList &args,
              std::function<void(bool,QString)> done);
    void unmountSequence(QVector<Device> volumes, const QString &driveObject);
};
}
