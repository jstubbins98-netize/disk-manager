#include "window.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStorageInfo>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <cmath>
#include <sys/stat.h>

namespace dm {
namespace {
class SizeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override {
        const int col = treeWidget()->sortColumn();
        if (col == 1) return data(1,Qt::UserRole).toLongLong() < other.data(1,Qt::UserRole).toLongLong();
        return QTreeWidgetItem::operator<(other);
    }
};
class FolderMap : public QWidget {
public:
    QMap<QString,qint64> folders;
    qint64 total = 0;
    bool radial = false;
    QString root;
    std::function<void(QString)> open;
    explicit FolderMap(QWidget *parent=nullptr) : QWidget(parent) { setMinimumHeight(220); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(),QColor("#101b29"));
        if (!total) { p.setPen(QColor("#a5b3c7")); p.drawText(rect(),Qt::AlignCenter,"Scan a folder to see its space distribution."); return; }
        QVector<QPair<QString,qint64>> sorted;
        for (auto i=folders.cbegin();i!=folders.cend();++i) sorted << qMakePair(i.key(),i.value());
        std::sort(sorted.begin(),sorted.end(),[](auto a,auto b){return a.second>b.second;});
        regions.clear(); double used = 0;
        int n=0;
        const QRectF area = QRectF(rect()).adjusted(16,16,-16,-16);
        for (const auto &pair : sorted) {
            if (!pair.second) continue;
            const double ratio = double(pair.second)/double(total);
            const QColor color = QColor::fromHsv((168+n*37)%360,130,185);
            p.setBrush(color); p.setPen(QPen(QColor("#101b29"),2));
            QPainterPath path;
            if (radial) {
                const double side = qMin(area.width(),area.height());
                const QRectF circle(area.center().x()-side/2,area.center().y()-side/2,side,side);
                path.moveTo(circle.center()); path.arcTo(circle,90-used*360,-ratio*360); path.closeSubpath();
                p.drawPath(path);
                // One ring per hierarchy level is not claimed: this is explicitly a folder ring.
            } else {
                QRectF r(area.x()+used*area.width(),area.y(),ratio*area.width(),area.height());
                path.addRect(r); p.drawPath(path);
                if (r.width()>65) {
                    p.setPen(Qt::white);
                    p.drawText(r.adjusted(10,12,-8,-8),Qt::TextWordWrap|Qt::AlignTop,
                               pair.first + "\n" + bytes(pair.second));
                }
            }
            regions << qMakePair(path,pair.first); used += ratio; ++n;
        }
        if (radial) {
            const double radius = qMin(area.width(),area.height())*0.25;
            p.setBrush(QColor("#101b29")); p.setPen(Qt::NoPen); p.drawEllipse(area.center(),radius,radius);
            p.setPen(Qt::white);
            p.drawText(QRectF(area.center().x()-radius,area.center().y()-30,2*radius,60),Qt::AlignCenter,bytes(total));
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        for (const auto &r : regions) if (r.first.contains(e->position()) && r.second != "(files in this folder)" && open) {
            open(QDir(root).filePath(r.second)); return;
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        for (const auto &r : regions) if (r.first.contains(e->position())) {
            setToolTip(r.second + ": " + bytes(folders.value(r.second))); return;
        }
    }
private:
    QVector<QPair<QPainterPath,QString>> regions;
};
}
SpacePage::SpacePage(QWidget *parent) : QWidget(parent) {
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(32,30,32,28); layout->setSpacing(14);
    layout->addWidget(heading("Understand your space"));
    layout->addWidget(note("Scan real folders, find large files, and review duplicates. Nothing is deleted automatically."));
    auto row = new QHBoxLayout;
    path_ = new QLineEdit(QDir::homePath());
    auto browse = new QPushButton("Choose folder"); scanButton_ = new QPushButton("Analyze"); scanButton_->setObjectName("primary");
    auto stop = new QPushButton("Stop");
    row->addWidget(path_,1); row->addWidget(browse); row->addWidget(scanButton_); row->addWidget(stop); layout->addLayout(row);
    status_ = note("Choose a folder and select Analyze. Symlinks and other filesystems are not traversed.");
    layout->addWidget(status_);
    auto controls = new QHBoxLayout;
    units_ = new QComboBox; units_->addItems({"GiB / binary","GB / decimal","Percent"});
    controls->addWidget(new QLabel("Display")); controls->addWidget(units_); controls->addStretch();
    auto parentButton = new QPushButton("Parent folder"); controls->addWidget(parentButton);
    layout->addLayout(controls);
    auto tabs = new QTabWidget;
    summary_ = new QTreeWidget; summary_->setHeaderLabels({"Category / filesystem","Size / capacity","Share / available"});
    summary_->header()->setSectionResizeMode(QHeaderView::Stretch);
    tabs->addTab(summary_,"Overview");
    files_ = new QTreeWidget; files_->setHeaderLabels({"File","Size","Last accessed"});
    files_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    files_->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    files_->setSortingEnabled(true);
    tabs->addTab(files_,"Largest & old files");
    treemap_ = new FolderMap; sunburst_ = new FolderMap;
    static_cast<FolderMap*>(sunburst_)->radial = true;
    for (auto map : {treemap_,sunburst_}) {
        auto m = static_cast<FolderMap*>(map); m->setMouseTracking(true);
        m->open = [this](const QString &path){ if (!busy_) { path_->setText(path); scan(path); } };
    }
    tabs->addTab(treemap_,"Folder map"); tabs->addTab(sunburst_,"Folder ring");
    duplicates_ = new QTreeWidget;
    duplicates_->setHeaderLabels({"Identical files (SHA-256)","Size"});
    duplicates_->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    duplicates_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tabs->addTab(duplicates_,"Duplicates");
    history_ = new QTreeWidget;
    history_->setHeaderLabels({"Scan time","Folder","Logical file size"});
    history_->header()->setSectionResizeMode(QHeaderView::Stretch);
    tabs->addTab(history_,"History");
    layout->addWidget(tabs,1);
    auto bottom = new QHBoxLayout;
    duplicatesButton_ = new QPushButton("Find duplicates");
    auto trash = new QPushButton("Move selected files to Trash"); trash->setObjectName("danger");
    auto cleanup = new QPushButton("Cleanup suggestions");
    bottom->addWidget(duplicatesButton_); bottom->addWidget(trash); bottom->addWidget(cleanup); bottom->addStretch();
    layout->addLayout(bottom);
    layout->addWidget(note("Last-access times may be approximate on noatime/relatime filesystems. File sizes are logical, not allocated blocks. History records successful scans; it is not a continuous monitor."));
    connect(browse,&QPushButton::clicked,this,[this] { auto p = QFileDialog::getExistingDirectory(this,"Analyze folder",path_->text()); if(!p.isEmpty()) path_->setText(p); });
    connect(scanButton_,&QPushButton::clicked,this,[this]{ scan(path_->text()); });
    connect(stop,&QPushButton::clicked,this,[this]{ if(cancel_) cancel_->store(true); });
    connect(parentButton,&QPushButton::clicked,this,[this]{ if(!busy_) { QDir d(path_->text()); d.cdUp(); path_->setText(d.path()); scan(d.path()); } });
    connect(units_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{ if(!result_.root.isEmpty()) display(result_); });
    connect(duplicatesButton_,&QPushButton::clicked,this,&SpacePage::duplicateScan);
    connect(trash,&QPushButton::clicked,this,[this,tabs] {
        if (tabs->currentWidget() != files_ && tabs->currentWidget() != duplicates_) {
            message(this,false,"Select files in Largest & old files or Duplicates first."); return;
        }
        trashSelected();
    });
    connect(cleanup,&QPushButton::clicked,this,[this] {
        message(this,true,"Review these folders using Choose folder:\n\n" + QDir::homePath() +
            "/.cache\n" + QDir::homePath() + "/.cache/thumbnails\n" + QDir::homePath() +
            "/.local/share/Trash\n/var/cache\n/var/log\n\nNo cleanup runs automatically. Do not remove active logs or package-manager files from this app. Use your distribution's tools for system cleanup.");
    });
    connect(&scanWatcher_,&QFutureWatcher<ScanResult>::finished,this,[this]{
        busy_ = false; scanButton_->setEnabled(true); duplicatesButton_->setEnabled(true);
        result_ = scanWatcher_.result(); display(result_);
        if (!result_.cancelled && result_.errors.isEmpty()) {
            auto object = readJson(stateDirectory()+"/space-history.json");
            auto entries = object["scans"].toArray();
            entries.append(QJsonObject{{"time",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                      {"path",result_.root},{"size",result_.total}});
            while (entries.size()>365) entries.removeFirst();
            QString error;
            if (!writeJson(stateDirectory()+"/space-history.json",{{"scans",entries}},&error))
                status_->setText(status_->text()+"\nCould not save history: "+error);
            history_->clear();
            for (auto v : entries) { auto o=v.toObject(); new QTreeWidgetItem(history_,{o["time"].toString(),o["path"].toString(),bytes(o["size"].toInteger())}); }
        }
    });
    connect(&duplicateWatcher_,&QFutureWatcher<QVector<QStringList>>::finished,this,[this]{
        busy_ = false; scanButton_->setEnabled(true); duplicatesButton_->setEnabled(true); duplicates_->clear();
        int index = 0;
        for (const auto &group : duplicateWatcher_.result()) {
            auto parent = new QTreeWidgetItem(duplicates_,{"Duplicate group " + QString::number(++index)});
            for (const auto &p : group) {
                auto child = new QTreeWidgetItem(parent,{p,bytes(QFileInfo(p).size())});
                child->setData(0,Qt::UserRole,p);
            }
            parent->setExpanded(true);
        }
        status_->setText(cancel_->load() ? "Duplicate search cancelled; results are partial." : QString("%1 duplicate groups found. Hard links are not counted as reclaimable duplicates. Review every selection before moving it to Trash.").arg(index));
    });
}
SpacePage::~SpacePage() {
    if(cancel_) cancel_->store(true);
    scanWatcher_.waitForFinished(); duplicateWatcher_.waitForFinished();
}
void SpacePage::scan(const QString &path) {
    if (busy_) return;
    busy_ = true; cancel_ = std::make_shared<std::atomic_bool>(false);
    scanButton_->setEnabled(false); duplicatesButton_->setEnabled(false); duplicates_->clear();
    status_->setText("Scanning… Large folders may take a while. Stop is safe.");
    auto token = cancel_;
    scanWatcher_.setFuture(QtConcurrent::run([path,token]{ return scanFolder(path,{},token); }));
}
void SpacePage::display(const ScanResult &s) {
    summary_->clear(); files_->clear();
    const bool binary = units_->currentIndex() == 0;
    auto size = [this,binary,&s](qint64 n) {
        return units_->currentIndex()==2 ? QString::number(s.total ? 100.0*n/s.total : 0,'f',1)+"%" : bytes(n,binary);
    };
    QStorageInfo drive(s.root); drive.refresh();
    if (drive.isValid() && drive.isReady()) {
        const qint64 used = drive.bytesTotal()-drive.bytesFree(), reserved = drive.bytesFree()-drive.bytesAvailable();
        new QTreeWidgetItem(summary_,{"Filesystem: "+drive.rootPath(),bytes(drive.bytesTotal(),binary),"Total capacity"});
        new QTreeWidgetItem(summary_,{"Used",bytes(used,binary),QString::number(100.0*used/qMax<qint64>(1,drive.bytesTotal()),'f',1)+"%"});
        new QTreeWidgetItem(summary_,{"Available",bytes(drive.bytesAvailable(),binary),"Free to your user"});
        new QTreeWidgetItem(summary_,{"Reserved",bytes(reserved,binary),"Free but unavailable to your user"});
    }
    for (auto i=s.categories.cbegin();i!=s.categories.cend();++i)
        new QTreeWidgetItem(summary_,{i.key(),size(i.value()),QString::number(s.total?100.0*i.value()/s.total:0,'f',1)+"%"});
    files_->setSortingEnabled(false);
    // A bounded GUI list; complete entries remain available for duplicate detection and backups.
    for (const auto &f : s.entries.mid(0,10000)) {
        auto item = new SizeItem(files_,{f.path,size(f.size),QDateTime::fromSecsSinceEpoch(f.accessed).toString("yyyy-MM-dd")});
        item->setData(0,Qt::UserRole,f.path); item->setData(1,Qt::UserRole,f.size);
        if (f.accessed < QDateTime::currentSecsSinceEpoch()-180LL*86400) item->setForeground(2,QColor("#e6b977"));
    }
    files_->setSortingEnabled(true); files_->sortItems(1,Qt::DescendingOrder);
    for (auto widget : {treemap_,sunburst_}) {
        auto map = static_cast<FolderMap*>(widget);
        map->folders = s.folders; map->root = s.root; map->total = s.total; map->update();
    }
    status_->setText(QString("%1 files · %2 · %3\n%4").arg(s.files).arg(bytes(s.total,binary),s.root,
        s.cancelled ? "Cancelled — partial results." : s.errors.isEmpty() ? "Scan complete. Click a folder in the map to drill down." : "Incomplete scan: "+s.errors.mid(0,3).join("; ")));
}
void SpacePage::duplicateScan() {
    if (busy_ || result_.root.isEmpty()) { message(this,false,"Analyze a folder first."); return; }
    busy_ = true; scanButton_->setEnabled(false); duplicatesButton_->setEnabled(false);
    status_->setText("Comparing same-size files using SHA-256. Nothing will be removed.");
    cancel_ = std::make_shared<std::atomic_bool>(false);
    duplicateWatcher_.setFuture(QtConcurrent::run([scan=result_,token=cancel_]{return findDuplicates(scan,token);}));
}
void SpacePage::trashSelected() {
    if (busy_) { message(this,false,"Wait for the scan to finish."); return; }
    QStringList paths;
    for (auto tree : {files_,duplicates_}) {
        if (!tree->isVisible()) continue;
        for (auto item : tree->selectedItems()) {
            auto p = item->data(0,Qt::UserRole).toString();
            if (!p.isEmpty()) paths << p;
        }
    }
    paths.removeDuplicates();
    if (paths.isEmpty()) { message(this,false,"Select individual files, not duplicate group headings."); return; }
    if (QMessageBox::warning(this,"Move to Trash",
        "Move " + QString::number(paths.size()) + " selected file(s) to Trash?\n\n" +
        paths.mid(0,8).join('\n') + "\n\nNo permanent-delete fallback will be used.",
        QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes) return;
    QStringList errors;
    for (const auto &p : paths) {
        struct stat st{};
        // Revalidate the exact file and location, never follow a replaced symbolic link.
        const QString real = canonical(p);
        if (::lstat(QFile::encodeName(p).constData(),&st) != 0 || !S_ISREG(st.st_mode) ||
            real.isEmpty() || !within(real,result_.root) || !QFile::moveToTrash(p))
            errors << p;
    }
    message(this,errors.isEmpty(),errors.isEmpty() ? "The selected files were moved to Trash." :
        "These files could not be moved to Trash. No permanent deletion was attempted:\n"+errors.join('\n'));
    scan(result_.root);
}
}
