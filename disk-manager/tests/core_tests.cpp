#include "backup.h"
#include "devices.h"
#include "scanner.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool b,const char *s) { if(!b) { std::fprintf(stderr,"FAIL: %s\n",s); std::exit(1); } }
void put(const QString &path,const QByteArray &data) { QFile f(path); require(f.open(QIODevice::WriteOnly),"open test file"); require(f.write(data)==data.size(),"write test file"); }
QJsonObject block(QString path,QString type,bool external,QString mount={},QJsonArray children={}) {
    return {{"path",path},{"kname",QFileInfo(path).fileName()},{"type",type},{"size",1000000000},
            {"ro",false},{"rm",external},{"fstype","ext4"},{"uuid",path},
            {"mountpoints",mount.isEmpty()?QJsonArray{}:QJsonArray{mount}},{"children",children}};
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    require(dm::within("/home/user/x","/home/user"),"path child");
    require(!dm::within("/home/user2","/home/user"),"path prefix boundary");
    require(dm::within("/etc","/"),"root boundary");
    auto root=block("/dev/sda","disk",false,{},QJsonArray{block("/dev/sda1","part",false,"/")});
    auto usb=block("/dev/sdb","disk",true,{},QJsonArray{block("/dev/sdb1","part",true)});
    usb["kname"]="/dev/sdb";
    auto list=dm::parseDevices(QJsonDocument(QJsonObject{{"blockdevices",QJsonArray{root,usb}}}).toJson());
    require(list.error.isEmpty() && list.items.size()==4,"parse device tree");
    require(list.items[0].locked && list.items[1].locked,"internal system disk locked");
    require(!list.items[3].locked,"external partition unlocked");
    require(list.items[2].name=="sdb" && list.items[2].objectPath.endsWith("/sdb"),"lsblk --paths kernel name normalized for D-Bus");
    require(dm::formatBlockReason(list.items[2],list.items).size()>0,"whole disk format refused");
    require(dm::formatBlockReason(list.items[3],list.items).isEmpty(),"unmounted external partition eligible");
    auto mounted=usb; auto children=mounted["children"].toArray(); auto part=children[0].toObject();
    part["mountpoints"]=QJsonArray{"/home"}; children[0]=part; mounted["children"]=children;
    auto locked=dm::parseDevices(QJsonDocument(QJsonObject{{"blockdevices",QJsonArray{mounted}}}).toJson());
    require(locked.items[0].locked && locked.items[1].locked,"external system volume locks whole disk");
    require(!dm::parseDevices("not json").error.isEmpty(),"bad discovery fails closed");
    QTemporaryDir temp;
    require(temp.isValid(),"temporary test directory");
    QDir d(temp.path()); d.mkpath("source/sub"); d.mkpath("source/node_modules"); d.mkpath("source/.local/share/Trash");
    d.mkpath("destination");
    put(d.filePath("source/a.txt"),"hello"); put(d.filePath("source/sub/copy.txt"),"hello");
    put(d.filePath("source/node_modules/skip"),"skip"); put(d.filePath("source/.local/share/Trash/skip"),"skip");
    QFile::link(d.filePath("source"),d.filePath("source/sub/loop"));
    auto scan=dm::scanFolder(d.filePath("source"),{"node_modules",".local/share/Trash"});
    require(scan.errors.isEmpty() && scan.files==2 && scan.total==10,"scan exclusions and symlink loop");
    auto duplicates=dm::findDuplicates(scan);
    require(duplicates.size()==1 && duplicates[0].size()==2,"SHA-256 duplicate detection");
    auto token=std::make_shared<std::atomic_bool>(true);
    require(dm::scanFolder(d.filePath("source"),{},token).cancelled,"scan cancellation");
    dm::BackupOptions o; o.source=d.filePath("source"); o.destination=d.filePath("source/sub"); o.allowLocal=true;
    require(!dm::planBackup(o).ready(),"overlap blocked");
    o.destination=d.filePath("destination");
    auto plan=dm::planBackup(o);
    require(plan.ready() && plan.required>=scan.total+64LL*1024*1024,"space check and safety margin");
    o.allowLocal=false;
    require(!dm::planBackup(o).ready(),"local destination opt-in required");
    require(dm::optionsFromJson(dm::optionsJson(o)).source==o.source,"backup configuration roundtrip");
    std::puts("PASS: core safety, device locks, scan, duplicates, exclusions, cancellation, backup preflight");
    return 0;
}
