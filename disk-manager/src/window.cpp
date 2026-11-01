#include "window.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QListWidget>
#include <QMessageBox>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace dm {
QLabel *heading(const QString &text) {
    auto l = new QLabel(text); l->setObjectName("heading"); return l;
}
QLabel *note(const QString &text) {
    auto l = new QLabel(text); l->setWordWrap(true); l->setObjectName("note"); l->setTextFormat(Qt::PlainText); return l;
}
void message(QWidget *parent, bool ok, const QString &text) {
    QMessageBox box(ok ? QMessageBox::Information : QMessageBox::Warning,
                    ok ? "Disk Manager" : "Operation stopped",text,QMessageBox::Ok,parent);
    box.setTextFormat(Qt::PlainText); box.exec();
}
Window::Window() {
    setWindowTitle("Disk Manager"); resize(1180,800); setMinimumSize(900,650);
    auto central = new QWidget; setCentralWidget(central);
    auto outer = new QHBoxLayout(central); outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);
    auto sidebar = new QWidget; sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(205);
    auto navLayout = new QVBoxLayout(sidebar); navLayout->setContentsMargins(20,28,20,24);
    auto brand = new QLabel("DISK\nMANAGER"); brand->setObjectName("brand"); navLayout->addWidget(brand);
    navLayout->addWidget(note("Your drives. Your data.")); navLayout->addSpacing(36);
    auto nav = new QListWidget; nav->setObjectName("navigation");
    nav->addItems({"Devices","Space","Backup"}); nav->setCurrentRow(0);
    navLayout->addWidget(nav); navLayout->addStretch();
    navLayout->addWidget(note("NATIVE LINUX APP\nC++20 · Qt6\n\nSafety first.\nNo forced unmounts.\nNo silent deletions."));
    outer->addWidget(sidebar);
    auto pages = new QStackedWidget; outer->addWidget(pages,1);
    auto devicePage = new QWidget;
    auto layout = new QVBoxLayout(devicePage); layout->setContentsMargins(32,30,32,28); layout->setSpacing(16);
    auto top = new QHBoxLayout;
    top->addWidget(heading("Your devices")); top->addStretch();
    refresh_ = new QPushButton("Refresh drives"); top->addWidget(refresh_);
    layout->addLayout(top);
    layout->addWidget(note("Mount, inspect, and safely remove external storage. Internal and system disks are locked."));
    deviceStatus_ = note("Discovering storage…"); layout->addWidget(deviceStatus_);
    devices_ = new QTreeWidget;
    devices_->setHeaderLabels({"Drive / partition","Capacity","Filesystem","Mount point","Protection"});
    devices_->setRootIsDecorated(false); devices_->setAlternatingRowColors(true);
    devices_->setSelectionMode(QAbstractItemView::SingleSelection);
    devices_->header()->setSectionResizeMode(QHeaderView::Interactive);
    devices_->header()->setSectionResizeMode(3,QHeaderView::Stretch);
    devices_->setColumnWidth(0,240); devices_->setColumnWidth(1,95);
    devices_->setColumnWidth(2,100); devices_->setColumnWidth(4,100);
    devices_->header()->setStretchLastSection(false);
    layout->addWidget(devices_,1);
    deviceActions_ = new QWidget;
    auto buttons = new QHBoxLayout(deviceActions_); buttons->setContentsMargins(0,0,0,0);
    for (const auto &name : {"Mount","Unmount","Safely remove","Health","Format…"}) {
        auto button = new QPushButton(name);
        if (QString(name).startsWith("Format")) button->setObjectName("danger");
        buttons->addWidget(button); connect(button,&QPushButton::clicked,this,[this,name]{ action(name); });
    }
    buttons->addStretch();
    readOnly_ = new QCheckBox("Mount read-only"); buttons->addWidget(readOnly_);
    layout->addWidget(deviceActions_);
    layout->addWidget(note("Formatting erases the selected partition permanently. It requires a typed confirmation and Linux authorization. Zero-fill is not a guaranteed secure erase on SSDs or flash storage."));
    pages->addWidget(devicePage);
    space_ = new SpacePage; backup_ = new BackupPage;
    pages->addWidget(space_); pages->addWidget(backup_);
    connect(nav,&QListWidget::currentRowChanged,pages,&QStackedWidget::setCurrentIndex);
    connect(refresh_,&QPushButton::clicked,this,&Window::refresh);
    connect(&service_,&DeviceService::finished,this,[this](bool ok,QString text) {
        deviceBusy_ = false; deviceActions_->setEnabled(true); refresh_->setEnabled(true);
        refresh(); message(this,ok,text);
    });
    QTimer::singleShot(0,this,&Window::refresh);
}
void Window::refresh() {
    if (deviceBusy_) return;
    refresh_->setEnabled(false);
    auto w = new QFutureWatcher<DeviceList>(this);
    connect(w,&QFutureWatcher<DeviceList>::finished,this,[this,w]{
        list_ = w->result(); w->deleteLater(); devices_->clear(); refresh_->setEnabled(true);
        int external = 0;
        for (int i = 0; i < list_.items.size(); ++i) {
            const auto &d = list_.items[i];
            if (d.type == "loop" || d.type == "rom" || d.size <= 0) continue;
            if (d.external && d.type == "disk") ++external;
            QString title = d.path + (d.label.isEmpty() ? "" : " · " + d.label);
            if (!d.model.isEmpty()) title += "\n" + d.model;
            auto item = new QTreeWidgetItem(devices_,{title,bytes(d.size),d.fs.isEmpty() ? "—" : d.fs,
                d.mounts.isEmpty() ? "Not mounted" : d.mounts.join(", "),d.locked ? "Locked" : "External"});
            item->setData(0,Qt::UserRole,i);
            if (d.locked) item->setForeground(4,QColor("#97a4b7"));
        }
        deviceStatus_->setText(!list_.error.isEmpty() ? list_.error :
            QString("%1 external drive(s) connected · %2 block devices discovered").arg(external).arg(list_.items.size())
            + (external == 0 ? "\nNo external drives detected. Connect a USB drive or SD card on the Linux computer running this app." : ""));
        deviceActions_->setEnabled(list_.error.isEmpty());
    });
    w->setFuture(QtConcurrent::run(listDevices));
}
Device Window::selected() const {
    auto item = devices_->currentItem();
    if (!item) return {};
    int index = item->data(0,Qt::UserRole).toInt();
    return index >= 0 && index < list_.items.size() ? list_.items[index] : Device{};
}
void Window::action(const QString &name) {
    if (deviceBusy_) return;
    const Device d = selected();
    if (d.path.isEmpty()) { message(this,false,"Select a drive or partition first."); return; }
    if (name == "Health") { message(this,true,service_.health(d)); return; }
    if (name.startsWith("Format")) { formatSelected(); return; }
    if (name == "Safely remove" && QMessageBox::question(this,"Safely remove",
        "Unmount every volume on " + d.disk + " and power down this physical drive?") != QMessageBox::Yes) return;
    deviceBusy_ = true; deviceActions_->setEnabled(false); refresh_->setEnabled(false);
    deviceStatus_->setText("Working… Respond to any Linux authorization prompt. Do not unplug the drive.");
    if (name == "Mount") service_.mount(d,readOnly_->isChecked());
    else if (name == "Unmount") service_.unmount(d);
    else service_.safelyRemove(d);
}
void Window::formatSelected() {
    auto d = selected();
    QString reason = formatBlockReason(d,list_.items);
    if (!reason.isEmpty()) { message(this,false,reason); return; }
    QDialog dialog(this); dialog.setWindowTitle("Format partition — permanent data loss"); dialog.setMinimumWidth(540);
    auto layout = new QVBoxLayout(&dialog);
    layout->addWidget(heading("Erase and format"));
    layout->addWidget(note("This will permanently erase 1 partition, " + bytes(d.size) + ", on " + d.path +
        (d.label.isEmpty() ? "" : " (" + d.label + ")") + ". Other partitions are not formatted. Back up anything you need first."));
    auto form = new QFormLayout;
    auto filesystem = new QComboBox;
    filesystem->addItem("ext4 — Linux","ext4"); filesystem->addItem("exFAT — cross-platform","exfat");
    filesystem->addItem("NTFS","ntfs"); filesystem->addItem("FAT32","vfat");
    filesystem->addItem("Btrfs","btrfs"); filesystem->addItem("XFS","xfs");
    auto label = new QLineEdit; label->setPlaceholderText("New volume label");
    auto zero = new QCheckBox("Overwrite partition with zeros first (slow)");
    auto confirm = new QLineEdit;
    form->addRow("Filesystem",filesystem); form->addRow("Label",label); form->addRow("",zero);
    form->addRow("Type exactly: " + d.confirmation(),confirm);
    layout->addLayout(form);
    layout->addWidget(note("Zero-fill is not guaranteed to erase remapped or wear-levelled flash cells. Use the manufacturer's secure-erase tools if you need a certified sanitization process.\n\nPartition-table editing (GPT/MBR) is not included. Use GNOME Disks or KDE Partition Manager for repartitioning."));
    auto box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto erase = box->addButton("Permanently erase partition",QDialogButtonBox::AcceptRole);
    erase->setObjectName("danger"); erase->setEnabled(false);
    connect(confirm,&QLineEdit::textChanged,&dialog,[erase,d](const QString &t){ erase->setEnabled(t == d.confirmation()); });
    connect(box,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(box,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    layout->addWidget(box);
    if (dialog.exec() != QDialog::Accepted) return;
    deviceBusy_ = true; deviceActions_->setEnabled(false); refresh_->setEnabled(false);
    deviceStatus_->setText("Formatting… Do not unplug the drive. Linux may request authorization.");
    service_.format(d,filesystem->currentData().toString(),label->text(),zero->isChecked(),confirm->text());
}
void Window::closeEvent(QCloseEvent *event) {
    if (deviceBusy_ || backup_->busy() || space_->busy()) {
        message(this,false,"An operation is still running. Cancel it or wait for it to finish before closing.");
        event->ignore(); return;
    }
    QMainWindow::closeEvent(event);
}
}
