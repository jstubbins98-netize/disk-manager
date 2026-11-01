#include "window.h"
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace dm {
BackupPage::BackupPage(QWidget *parent) : QWidget(parent), engine_(this) {
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(32,30,32,28); layout->setSpacing(12);
    layout->addWidget(heading("Back up with confidence"));
    layout->addWidget(note("Check the space before copying. Every backup has its own folder and a completion report."));
    auto form = new QFormLayout;
    preset_ = new QComboBox;
    preset_->addItems({"Home folder","Documents & Media","Full system (readable files only)","Custom folder"});
    source_ = new QLineEdit(QDir::homePath());
    auto sourceRow = new QHBoxLayout; auto sourceBrowse = new QPushButton("Choose…");
    sourceRow->addWidget(source_,1); sourceRow->addWidget(sourceBrowse);
    destinations_ = new QComboBox;
    auto destRow = new QHBoxLayout; auto refresh = new QPushButton("Refresh"); destRow->addWidget(destinations_,1); destRow->addWidget(refresh);
    destination_ = new QLineEdit; auto destBrowse = new QPushButton("Choose folder…");
    auto folderRow = new QHBoxLayout; folderRow->addWidget(destination_,1); folderRow->addWidget(destBrowse);
    exclusions_ = new QLineEdit(".cache,node_modules,.local/share/Trash,.thumbnails");
    mode_ = new QComboBox; mode_->addItems({"Full copy","Incremental (hard-linked version)","Snapshot (hard-linked version)"});
    verify_ = new QCheckBox("Verify every scanned regular file using SHA-256"); verify_->setChecked(true);
    local_ = new QCheckBox("Allow a local destination (testing / not protection against disk failure)");
    form->addRow("Source preset",preset_); form->addRow("Source folder",sourceRow);
    form->addRow("External volume",destRow); form->addRow("Destination folder",folderRow);
    form->addRow("Exclusions, comma-separated",exclusions_); form->addRow("Backup type",mode_);
    form->addRow("",verify_); form->addRow("",local_);
    layout->addLayout(form);
    layout->addWidget(note("Incremental and snapshot modes keep complete browsable versions using hard links to unchanged files from the last backup of this source. Use an ext4, Btrfs, XFS, or another hard-link-capable destination. The space check is conservative and does not assume savings."));
    space_ = note("01  Choose your source and destination, then check the space."); space_->setObjectName("spaceCheck");
    layout->addWidget(space_);
    auto actions = new QHBoxLayout;
    check_ = new QPushButton("Check space"); check_->setObjectName("primary");
    run_ = new QPushButton("Start backup"); run_->setEnabled(false); run_->setObjectName("primary");
    pause_ = new QPushButton("Pause"); cancel_ = new QPushButton("Cancel");
    pause_->setEnabled(false); cancel_->setEnabled(false);
    actions->addWidget(check_); actions->addWidget(run_); actions->addWidget(pause_); actions->addWidget(cancel_); actions->addStretch();
    layout->addLayout(actions);
    progress_ = new QProgressBar; progress_->setRange(0,100); progress_->setValue(0); layout->addWidget(progress_);
    log_ = new QPlainTextEdit; log_->setReadOnly(true); log_->setMaximumBlockCount(2000); log_->setMinimumHeight(90);
    layout->addWidget(log_,1);
    auto extras = new QHBoxLayout;
    auto restoreButton = new QPushButton("Restore…"); auto scheduleButton = new QPushButton("Schedule…");
    auto reports = new QPushButton("Backup reports"); auto retention = new QPushButton("Review old backups…");
    extras->addWidget(restoreButton); extras->addWidget(scheduleButton); extras->addWidget(reports); extras->addWidget(retention); extras->addStretch();
    layout->addLayout(extras);
    health_ = note("No backup destination selected."); layout->addWidget(health_);
    connect(sourceBrowse,&QPushButton::clicked,this,[this] {
        if (busy()) return;
        auto path = QFileDialog::getExistingDirectory(this,"Source folder",source_->text());
        if(!path.isEmpty()) { preset_->setCurrentIndex(3); source_->setText(path); }
    });
    connect(destBrowse,&QPushButton::clicked,this,[this] {
        if (busy()) return;
        auto path = QFileDialog::getExistingDirectory(this,"Destination folder",destination_->text());
        if(!path.isEmpty()) destination_->setText(path);
    });
    connect(refresh,&QPushButton::clicked,this,&BackupPage::refreshDestinations);
    connect(destinations_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this] {
        const auto path = destinations_->currentData().toString();
        if(!path.isEmpty()) destination_->setText(path);
    });
    connect(preset_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index) {
        if (index < 2) source_->setText(QDir::homePath());
        else if (index == 2) source_->setText("/");
        invalidate();
    });
    for (auto field : {source_,destination_,exclusions_})
        connect(field,&QLineEdit::textChanged,this,&BackupPage::invalidate);
    connect(mode_,qOverload<int>(&QComboBox::currentIndexChanged),this,&BackupPage::invalidate);
    connect(verify_,&QCheckBox::toggled,this,&BackupPage::invalidate);
    connect(local_,&QCheckBox::toggled,this,&BackupPage::invalidate);
    connect(check_,&QPushButton::clicked,this,&BackupPage::checkSpace);
    connect(run_,&QPushButton::clicked,this,[this] {
        if (!plan_.ready() || engine_.running()) return;
        log_->clear(); progress_->setValue(0);
        setBusy(true);
        if (!engine_.start(plan_)) setBusy(false);
    });
    connect(pause_,&QPushButton::clicked,this,[this] {
        paused_ = !paused_; engine_.pause(paused_); pause_->setText(paused_ ? "Resume" : "Pause");
    });
    connect(cancel_,&QPushButton::clicked,&engine_,&BackupEngine::cancel);
    connect(&engine_,&BackupEngine::output,this,[this](QString text) {
        log_->appendPlainText(text.trimmed());
        QRegularExpression rx("(\\d+)%");
        auto it = rx.globalMatch(text);
        while(it.hasNext()) progress_->setValue(it.next().captured(1).toInt());
    });
    connect(&engine_,&BackupEngine::finished,this,[this](bool ok,QString text) {
        setBusy(false); invalidate(); if (ok) progress_->setValue(100);
        log_->appendPlainText(text); message(this,ok,text);
        auto backups = completedBackups(destination_->text());
        health_->setText(backups.isEmpty() ? "No completed backups at this destination." :
            "Last completed backup: " + QFileInfo(backups.last()).fileName());
    });
    connect(restoreButton,&QPushButton::clicked,this,&BackupPage::restore);
    connect(scheduleButton,&QPushButton::clicked,this,&BackupPage::schedule);
    connect(reports,&QPushButton::clicked,this,[this] {
        auto backups = completedBackups(destination_->text());
        if(backups.isEmpty()) { message(this,false,"No completed backups found in this destination."); return; }
        QString text;
        for (const auto &p : backups.mid(qMax(0,backups.size()-10))) {
            auto m=readJson(p+"/manifest.json");
            text += QFileInfo(p).fileName()+"\n"+m["source"].toString()+"\n"+
                QString::number(m["files"].toInteger())+" scanned regular files · "+bytes(m["bytes"].toInteger())+
                (m["verified"].toBool() ? " · SHA-256 verified" : " · not verified")+"\n"+m["report"].toString()+"\n\n";
        }
        message(this,true,text);
    });
    connect(retention,&QPushButton::clicked,this,[this] {
        if(busy()) { message(this,false,"Wait for the current operation to finish."); return; }
        auto backups=completedBackups(destination_->text());
        if(backups.size()<2) { message(this,false,"Keep at least one completed backup. No older version can be removed."); return; }
        QStringList names; for (const auto &p : backups.mid(0,backups.size()-1)) names << QFileInfo(p).fileName();
        bool ok=false; auto choice=QInputDialog::getItem(this,"Review an old backup","Choose one version to move to Trash (latest version protected)",names,0,false,&ok);
        if(!ok) return;
        const QString folder = backups.at(names.indexOf(choice));
        if(QMessageBox::warning(this,"Move backup to Trash","Move this complete backup version to Trash?\n"+folder+
            "\n\nHard-linked files in other versions are kept. Trash may not free disk space until emptied.",
            QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes) return;
        if(QFileInfo(folder).isSymLink() || !QFile::moveToTrash(folder))
            message(this,false,"Could not move the version to Trash. It was not permanently deleted.");
        else message(this,true,"The selected backup version was moved to Trash.");
    });
    refreshDestinations();
}
BackupOptions BackupPage::options() const {
    BackupOptions o; o.source=source_->text(); o.destination=destination_->text();
    o.mode = QStringList{"full","incremental","snapshot"}.at(mode_->currentIndex());
    o.excludes.clear();
    for(const auto &e : exclusions_->text().split(',',Qt::SkipEmptyParts)) o.excludes << e.trimmed();
    if(preset_->currentIndex()==1) {
        const QStringList include{"Documents","Pictures","Videos","Music"};
        for(const auto &name : QDir(o.source).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot))
            if(!include.contains(name)) o.excludes << QDir(o.source).filePath(name);
    }
    o.verify=verify_->isChecked(); o.allowLocal=local_->isChecked(); return o;
}
void BackupPage::refreshDestinations() {
    if(busy()) return;
    destinations_->clear(); destinations_->addItem("Choose a mounted external volume",QString());
    auto list=listDevices();
    for (const auto &d : list.items) if(d.external && !d.locked)
        for(const auto &mount : d.mounts) {
            QStorageInfo storage(mount); storage.refresh();
            destinations_->addItem((d.label.isEmpty()?d.name:d.label)+" · "+bytes(storage.bytesAvailable())+" available",mount);
        }
    if(!list.error.isEmpty()) health_->setText(list.error);
}
void BackupPage::invalidate() {
    ++revision_; plan_=BackupPlan{}; run_->setEnabled(false);
    if(!busy()) space_->setText("01  Settings changed. Check space before starting.");
}
void BackupPage::checkSpace() {
    if(busy()) return;
    checking_=true; setBusy(true); space_->setText("Calculating required space… No data is being copied.");
    auto w=new QFutureWatcher<BackupPlan>(this);
    const auto revision=revision_;
    connect(w,&QFutureWatcher<BackupPlan>::finished,this,[this,w,revision] {
        checking_=false; auto result=w->result(); w->deleteLater(); setBusy(false);
        if(revision!=revision_) { invalidate(); return; }
        plan_=result; run_->setEnabled(plan_.ready());
        space_->setText(plan_.ready() ?
            "02  Ready · Needs "+bytes(plan_.required)+", "+bytes(plan_.available)+" available.\nIncludes metadata allowance and a 64 MiB safety margin." :
            "Cannot start · "+plan_.error);
        if(!plan_.ready() && !plan_.scan.entries.isEmpty()) {
            QString text="\nLargest source files to consider excluding:\n";
            for(const auto &f : plan_.scan.entries.mid(0,5)) text+=bytes(f.size)+"  "+f.path+"\n";
            log_->appendPlainText(text);
        }
    });
    w->setFuture(QtConcurrent::run([o=options()]{return planBackup(o);}));
}
void BackupPage::setBusy(bool busy) {
    for(auto widget : QList<QWidget*>{preset_,source_,destination_,exclusions_,mode_,verify_,local_,destinations_,check_}) widget->setEnabled(!busy);
    run_->setEnabled(!busy && plan_.ready());
    pause_->setEnabled(busy && !checking_); cancel_->setEnabled(busy && !checking_);
    if(!busy) { paused_=false; pause_->setText("Pause"); }
}
void BackupPage::restore() {
    if(busy()) { message(this,false,"Wait for the current operation to finish."); return; }
    auto folder=QFileDialog::getExistingDirectory(this,"Select a completed backup version",destination_->text()+"/DiskManager");
    if(folder.isEmpty()) return;
    bool ok=false; auto kind=QInputDialog::getItem(this,"Restore","Restore scope",{"Everything","One file"},0,false,&ok);
    if(!ok) return;
    QString relative=".";
    if(kind=="One file") {
        auto file=QFileDialog::getOpenFileName(this,"Select a file inside the backup",folder+"/data");
        if(file.isEmpty()) return;
        relative=QDir(folder+"/data").relativeFilePath(file);
    }
    auto dest=QFileDialog::getExistingDirectory(this,"Choose an EMPTY restore destination",QDir::homePath());
    if(dest.isEmpty()) return;
    if(QMessageBox::question(this,"Restore backup","Copy "+kind.toLower()+" to:\n"+dest+
        "\n\nThe destination must be empty. Existing files will not be overwritten.")!=QMessageBox::Yes) return;
    log_->clear(); setBusy(true);
    if(!engine_.restore(folder,relative,dest)) setBusy(false);
}
void BackupPage::schedule() {
    if(busy()) return;
    if(!plan_.ready()) { message(this,false,"Check space successfully with the desired settings before scheduling."); return; }
    bool ok=false;
    auto frequency=QInputDialog::getItem(this,"Schedule backup","User-session systemd timer",
        {"daily","weekly","disable schedule"},0,false,&ok);
    if(!ok) return;
    if(frequency=="disable schedule") {
        auto r=command("systemctl",{"--user","disable","--now","diskmanager-backup.timer"});
        message(this,r.ok(),r.ok()?"The scheduled backup timer is disabled.":r.error); return;
    }
    QString error;
    bool saved=saveSchedule(plan_.options,frequency,&error);
    message(this,saved,saved ? "Schedule enabled. The Linux user session must be running and the same external volume must be mounted at the configured path. Missing drives and failed space checks cause the run to fail safely.\n\nCheck reports with:\njournalctl --user -u diskmanager-backup.service" : error);
}
}
