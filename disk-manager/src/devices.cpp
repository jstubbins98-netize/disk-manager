#include "devices.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QFileInfo>
#include <QSet>
#include <QtConcurrent>
#include <QFutureWatcher>

namespace dm {
namespace {
bool flag(const QJsonValue &v) { return v.isBool() ? v.toBool() : v.toInt() != 0; }
qint64 integer(const QJsonValue &v) { return v.isString() ? v.toString().toLongLong() : v.toInteger(); }
QString objectName(QString name) {
    QByteArray out;
    for (unsigned char c : name.toUtf8()) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += char(c);
        else out += "_" + QByteArray::number(c,16).rightJustified(2,'0');
    }
    return "/org/freedesktop/UDisks2/block_devices/" + QString::fromLatin1(out);
}
bool systemMount(const QString &m) {
    return !m.isEmpty() && !(within(m,"/media") || within(m,"/run/media") || within(m,"/mnt"));
}
void flatten(const QJsonObject &o, const QString &disk, bool external, QVector<Device> &out) {
    Device d;
    d.path = o["path"].toString(); d.name = QFileInfo(o["kname"].toString()).fileName();
    d.label = o["label"].toString(); d.fs = o["fstype"].toString();
    d.uuid = o["uuid"].toString(); d.model = o["model"].toString().trimmed();
    d.serial = o["serial"].toString(); d.type = o["type"].toString();
    d.size = integer(o["size"]); d.readOnly = flag(o["ro"]);
    d.disk = disk.isEmpty() ? d.path : disk;
    // Only the physical parent can establish external status. A partition's
    // removable flag must never unlock an internal disk. Built-in eMMC also
    // uses mmcblk names, so the name alone is not evidence of removability.
    const QString transport = o["tran"].toString().toLower();
    const bool internalTransport = QStringList{"ata","sata","sas","nvme","virtio"}.contains(transport);
    d.external = disk.isEmpty()
        ? d.type == "disk" && !internalTransport && (transport == "usb" || flag(o["rm"]))
        : external;
    for (const auto &m : o["mountpoints"].toArray()) if (!m.isNull() && !m.toString().isEmpty()) d.mounts << m.toString();
    d.objectPath = objectName(d.name);
    d.locked = true;
    out << d;
    for (auto child : o["children"].toArray()) flatten(child.toObject(),d.disk,d.external,out);
}
Device refreshed(const Device &wanted, QString &error) {
    auto list = listDevices();
    if (!list.error.isEmpty()) { error = list.error; return {}; }
    for (const auto &d : list.items) {
        if (d.path == wanted.path && d.identity() == wanted.identity()) return d;
    }
    error = "The device changed or disappeared. Refresh and select it again.";
    return {};
}
QString driveFor(const Device &d) {
    QDBusInterface block("org.freedesktop.UDisks2", d.objectPath,
                         "org.freedesktop.DBus.Properties", QDBusConnection::systemBus());
    QDBusReply<QDBusVariant> reply = block.call("Get", "org.freedesktop.UDisks2.Block", "Drive");
    return reply.isValid() ? qvariant_cast<QDBusObjectPath>(reply.value().variant()).path() : QString{};
}
}
QString Device::identity() const { return path + "|" + serial + "|" + QString::number(size) + "|" + uuid; }
DeviceList parseDevices(const QByteArray &json) {
    QJsonParseError e;
    auto doc = QJsonDocument::fromJson(json, &e);
    if (e.error != QJsonParseError::NoError || !doc.object()["blockdevices"].isArray())
        return {{}, "Cannot parse the device list; disk actions are disabled."};
    DeviceList result;
    for (const auto &v : doc.object()["blockdevices"].toArray()) flatten(v.toObject(),{},false,result.items);
    QSet<QString> lockedDisks;
    for (const auto &d : result.items) {
        if (!d.external || d.fs == "swap" || d.type == "crypt" || d.type == "lvm" ||
            d.type.startsWith("raid") || d.readOnly) lockedDisks.insert(d.disk);
        for (const auto &m : d.mounts) if (systemMount(m)) lockedDisks.insert(d.disk);
    }
    for (auto &d : result.items)
        d.locked = lockedDisks.contains(d.disk) || !d.external || !(d.type == "part" || d.type == "disk");
    return result;
}
DeviceList listDevices() {
    auto r = command("lsblk", {"--json","--bytes","--paths","--output",
        "PATH,KNAME,TYPE,SIZE,FSTYPE,LABEL,UUID,MOUNTPOINTS,RM,RO,TRAN,MODEL,SERIAL"});
    if (!r.ok()) return {{}, "Device discovery failed: " + r.error};
    return parseDevices(r.out.toUtf8());
}
QString deviceBlockReason(const Device &d) {
    if (!d.external)
        return "Internal or unverified drive: unmounting, ejecting, and erasing are prohibited.";
    if (d.locked || d.readOnly)
        return "This drive is protected: system, read-only, swap, or an unsupported device stack.";
    return {};
}
QString formatBlockReason(const Device &d, const QVector<Device> &all) {
    const QString protection = deviceBlockReason(d);
    if (!protection.isEmpty()) return protection;
    // Whole-disk formatting is deliberately excluded; only existing partitions.
    if (d.type != "part") return "Select an existing partition. Partition-table creation is not supported in this release.";
    for (const auto &sibling : all) if (sibling.disk == d.disk && !sibling.mounts.isEmpty())
        return "Unmount every volume on this physical drive before formatting.";
    if (d.size == 0 || !d.path.startsWith("/dev/")) return "The device identity could not be validated.";
    return {};
}
void DeviceService::call(const QString &path, const QString &interface, const QString &method,
                         const QVariantList &args, std::function<void(bool,QString)> done) {
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) { done(false,"The system D-Bus is unavailable. Run this application on your Linux desktop."); return; }
    auto message = QDBusMessage::createMethodCall("org.freedesktop.UDisks2",path,interface,method);
    message.setInteractiveAuthorizationAllowed(true);
    message.setArguments(args);
    auto watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 24*60*60*1000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[watcher,done](auto*) {
        auto reply = watcher->reply();
        const bool ok = reply.type() != QDBusMessage::ErrorMessage;
        QString text = ok ? "Operation completed." : reply.errorMessage();
        if (ok && !reply.arguments().isEmpty() && reply.arguments().first().metaType().id() == QMetaType::QString)
            text = reply.arguments().first().toString();
        done(ok,text); watcher->deleteLater();
    });
}
void DeviceService::mount(const Device &device, bool ro) {
    QString error; auto d = refreshed(device,error);
    if (!error.isEmpty() || d.locked || d.fs.isEmpty() || !d.mounts.isEmpty()) {
        emit finished(false,error.isEmpty() ? "Select an unmounted, supported external filesystem." : error); return;
    }
    QVariantMap opts; opts["options"] = ro ? "ro" : "rw";
    call(d.objectPath,"org.freedesktop.UDisks2.Filesystem","Mount",{opts},
         [this](bool ok,QString text){ emit finished(ok,text); });
}
void DeviceService::unmount(const Device &device) {
    const QString protection = deviceBlockReason(device);
    if (!protection.isEmpty()) { emit finished(false,protection); return; }
    QString error; auto d = refreshed(device,error);
    if (error.isEmpty()) error = deviceBlockReason(d);
    if (!error.isEmpty() || d.mounts.isEmpty()) {
        emit finished(false,error.isEmpty() ? "Select a mounted, unprotected external volume." : error); return;
    }
    auto w = new QFutureWatcher<CommandResult>(this);
    connect(w,&QFutureWatcher<CommandResult>::finished,this,[this,w,d] {
        auto r = w->result(); w->deleteLater();
        if (!r.ok()) { emit finished(false,"Sync failed: " + r.error); return; }
        QString error; const auto current = refreshed(d,error);
        if (error.isEmpty()) error = deviceBlockReason(current);
        if (!error.isEmpty()) { emit finished(false,error); return; }
        call(d.objectPath,"org.freedesktop.UDisks2.Filesystem","Unmount",{QVariantMap{}},
             [this,d](bool ok,QString text) {
            if (!ok && !d.mounts.isEmpty()) {
                auto holders = command("fuser",{"-vm",d.mounts.first()},5000);
                text += "\n\nPrograms using this volume (fuser):\n" + holders.out + holders.error;
                text += "\nClose the listed programs and try again. Forced unmount is never used.";
            }
            emit finished(ok,text);
        });
    });
    w->setFuture(QtConcurrent::run([] { return command("sync",{}); }));
}
void DeviceService::unmountSequence(QVector<Device> volumes, const QString &driveObject, const Device &device) {
    // Recheck after sync and between asynchronous unmounts, including the final
    // power-off when there were no mounted partitions.
    QString error; const auto current = refreshed(device,error);
    if (error.isEmpty()) error = deviceBlockReason(current);
    const auto list = listDevices();
    if (error.isEmpty()) error = list.error;
    for (const auto &v : list.items) {
        if (error.isEmpty() && v.disk == current.disk) error = deviceBlockReason(v);
    }
    if (!error.isEmpty()) { emit finished(false,error); return; }
    if (driveFor(current) != driveObject) {
        emit finished(false,"The physical drive changed. Safe removal stopped."); return;
    }
    if (volumes.isEmpty()) {
        call(driveObject,"org.freedesktop.UDisks2.Drive","PowerOff",{QVariantMap{}},
             [this](bool ok,QString text){ emit finished(ok,ok ? "Powered off. The drive can now be unplugged." : text); });
        return;
    }
    auto d = volumes.takeLast();
    const auto volume = refreshed(d,error);
    if (error.isEmpty()) error = deviceBlockReason(volume);
    if (!error.isEmpty()) { emit finished(false,error); return; }
    call(d.objectPath,"org.freedesktop.UDisks2.Filesystem","Unmount",{QVariantMap{}},
         [this,volumes,driveObject,device](bool ok,QString text) {
        if (!ok) { emit finished(false,"Safe removal stopped: " + text + "\nClose files on this drive and retry."); return; }
        unmountSequence(volumes,driveObject,device);
    });
}
void DeviceService::safelyRemove(const Device &device) {
    const QString protection = deviceBlockReason(device);
    if (!protection.isEmpty()) { emit finished(false,protection); return; }
    QString error; auto d = refreshed(device,error);
    if (error.isEmpty()) error = deviceBlockReason(d);
    if (!error.isEmpty()) { emit finished(false,error); return; }
    auto list = listDevices();
    if (!list.error.isEmpty()) { emit finished(false,list.error); return; }
    QVector<Device> volumes;
    for (const auto &v : list.items) if (v.disk == d.disk) {
        if (!deviceBlockReason(v).isEmpty()) { emit finished(false,"A volume on this disk is protected."); return; }
        if (!v.mounts.isEmpty()) volumes << v;
    }
    const QString drive = driveFor(d);
    if (drive.isEmpty() || drive == "/") { emit finished(false,"UDisks2 cannot identify this drive."); return; }
    auto w = new QFutureWatcher<CommandResult>(this);
    connect(w,&QFutureWatcher<CommandResult>::finished,this,[this,w,volumes,drive,d] {
        auto r = w->result(); w->deleteLater();
        if (!r.ok()) { emit finished(false,"Sync failed: " + r.error); return; }
        unmountSequence(volumes,drive,d);
    });
    w->setFuture(QtConcurrent::run([]{ return command("sync",{}); }));
}
void DeviceService::format(const Device &device, const QString &fs, const QString &label,
                           bool zeroFill, const QString &confirmation) {
    const QString protection = deviceBlockReason(device);
    if (!protection.isEmpty()) { emit finished(false,protection); return; }
    auto list = listDevices(); QString error;
    auto d = refreshed(device,error);
    if (error.isEmpty()) error = list.error;
    if (error.isEmpty()) error = formatBlockReason(d,list.items);
    if (error.isEmpty() && confirmation != d.confirmation()) error = "The confirmation did not match.";
    if (error.isEmpty() && !QStringList{"ext4","exfat","ntfs","vfat","btrfs","xfs"}.contains(fs)) error = "Unsupported filesystem.";
    const int maxLength = fs == "vfat" ? 11 : fs == "exfat" ? 15 : fs == "ext4" ? 16 : fs == "xfs" ? 12 : 32;
    if (label.toUtf8().size() > maxLength || label.contains('/') || label.contains(QChar('\0'))) error = "The label is too long or contains invalid characters.";
    if (!error.isEmpty()) { emit finished(false,error); return; }
    QVariantMap options{{"label",label},{"take-ownership", fs == "ext4" || fs == "btrfs" || fs == "xfs"}};
    if (zeroFill) options["erase"] = "zero";
    call(d.objectPath,"org.freedesktop.UDisks2.Block","Format",{fs,options},
         [this](bool ok,QString text){ emit finished(ok,ok ? "The selected partition was formatted." : text); });
}
QString DeviceService::health(const Device &device) {
    const QString drive = driveFor(device);
    if (drive.isEmpty()) return "SMART unavailable: no UDisks2 drive connection.";
    QDBusInterface props("org.freedesktop.UDisks2",drive,"org.freedesktop.DBus.Properties",QDBusConnection::systemBus());
    QDBusReply<QDBusVariant> support = props.call("Get","org.freedesktop.UDisks2.Drive.Ata","SmartSupported");
    if (!support.isValid() || !support.value().variant().toBool()) return "SMART is not available through this drive or USB bridge.";
    QDBusReply<QDBusVariant> failing = props.call("Get","org.freedesktop.UDisks2.Drive.Ata","SmartFailing");
    if (!failing.isValid()) return "SMART status could not be read.";
    return failing.value().variant().toBool() ? "WARNING: SMART reports that this drive is failing. Back up immediately."
                                            : "SMART: no failure reported. This does not guarantee drive health.";
}
}
