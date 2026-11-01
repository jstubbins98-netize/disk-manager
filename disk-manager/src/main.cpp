#include "window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListWidget>
#include <QStackedWidget>
#include <QTextStream>
#include <QTimer>

namespace {
int cli(QCoreApplication &app) {
    QCommandLineParser parser;
    parser.setApplicationDescription("Safety-first Linux Disk Manager — native C++ / Qt6");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({
        {{"l","list"},"Print the real block-device list as JSON."},
        {"scan","Analyze one directory and print JSON.","folder"},
        {"backup-config","Run a backup using a saved JSON configuration (systemd).","file"},
        {"backup","Run a backup from --source to --dest."},
        {"source","Source directory.","folder"},
        {"dest","Destination directory on a mounted external drive.","folder"},
        {"mode","full, incremental, or snapshot.","mode","full"},
        {"exclude","Exclude a literal file/directory name or source-relative path; repeatable.","path"},
        {"allow-local","Explicitly allow a local backup destination; intended for testing."},
        {"no-verify","Do not perform SHA-256 verification after copying."},
        {"check","Only calculate the backup space requirement; do not copy."}
    });
    parser.process(app);
    QTextStream out(stdout), err(stderr);
    if(parser.isSet("list")) {
        auto devices=dm::listDevices();
        if(!devices.error.isEmpty()) {err<<devices.error<<'\n';return 1;}
        QJsonArray list;
        for(const auto &d : devices.items) list.append(QJsonObject{
            {"path",d.path},{"label",d.label},{"bytes",d.size},{"filesystem",d.fs},
            {"mounts",QJsonArray::fromStringList(d.mounts)},{"external",d.external},{"locked",d.locked}});
        out<<QJsonDocument(list).toJson(); return 0;
    }
    if(parser.isSet("scan")) {
        auto scan=dm::scanFolder(parser.value("scan"));
        QJsonObject categories;
        for(auto i=scan.categories.cbegin();i!=scan.categories.cend();++i) categories[i.key()]=i.value();
        out<<QJsonDocument(QJsonObject{{"root",scan.root},{"bytes",scan.total},{"files",scan.files},
                {"categories",categories},{"errors",QJsonArray::fromStringList(scan.errors)}}).toJson();
        return scan.errors.isEmpty()?0:1;
    }
    if(parser.isSet("backup") || parser.isSet("backup-config") || parser.isSet("check")) {
        dm::BackupOptions options;
        if(parser.isSet("backup-config")) {
            auto json=dm::readJson(parser.value("backup-config"));
            if(json.isEmpty()) { err<<"Cannot read the backup configuration.\n"; return 1; }
            options=dm::optionsFromJson(json);
        } else {
            options.source=parser.value("source"); options.destination=parser.value("dest");
            options.mode=parser.value("mode"); options.allowLocal=parser.isSet("allow-local");
            options.verify=!parser.isSet("no-verify");
            options.excludes.append(parser.values("exclude"));
        }
        const auto plan=dm::planBackup(options);
        out<<"Source: "<<plan.options.source<<"\nNeeds "<<dm::bytes(plan.required)
           <<", "<<dm::bytes(plan.available)<<" available.\n"; out.flush();
        if(!plan.ready()) { err<<plan.error<<'\n';return 1; }
        if(parser.isSet("check")) return 0;
        dm::BackupEngine engine;
        QObject::connect(&engine,&dm::BackupEngine::output,&app,[&out](const QString &s){out<<s;out.flush();});
        QObject::connect(&engine,&dm::BackupEngine::finished,&app,[&app,&out](bool success,const QString &text){
            out<<'\n'<<text<<'\n';out.flush();app.exit(success?0:1);
        });
        QTimer::singleShot(0,&app,[&engine,plan]{engine.start(plan);});
        return app.exec();
    }
    parser.showHelp(0);
}
}
int main(int argc,char **argv) {
    bool headless=false, screenshot=false;
    for(int i=1;i<argc;++i) {
        const QString arg=QString::fromLocal8Bit(argv[i]);
        if(arg=="--screenshot") screenshot=true;
        else headless=true;
    }
    QCoreApplication::setOrganizationName("DiskManager");
    QCoreApplication::setApplicationName("DiskManager");
    QCoreApplication::setApplicationVersion("0.4.0");
    if(!screenshot && headless) { QCoreApplication app(argc,argv); return cli(app); }
    if(screenshot) qputenv("QT_QPA_PLATFORM","offscreen");
    QApplication app(argc,argv);
    app.setStyle("Fusion");
    app.setStyleSheet(R"(
        QWidget { background: #111c2a; color: #e5edf7; font-family: "DejaVu Sans"; font-size: 13px; }
        QMainWindow { background: #111c2a; }
        #sidebar { background: #0b1420; border-right: 1px solid #263447; }
        #brand { background: transparent; color: #75dec7; font-size: 24px; font-weight: 800; letter-spacing: 3px; }
        #heading { font-size: 28px; font-weight: 700; color: #f0f5fc; }
        #note { color: #9faec2; font-size: 12px; background: transparent; }
        #spaceCheck { background: #1c3441; color: #9ce6d3; border: 1px solid #335861; padding: 13px; border-radius: 6px; }
        QListWidget#navigation { border: none; background: transparent; outline: none; }
        QListWidget#navigation::item { padding: 14px 12px; border-radius: 5px; margin-bottom: 5px; color: #afbed1; }
        QListWidget#navigation::item:selected { background: #1b3c40; color: #9cf1d8; }
        QPushButton { background: #203145; border: 1px solid #334a62; padding: 9px 13px; border-radius: 5px; }
        QPushButton:hover { background: #2d435c; border-color: #65b5a6; }
        QPushButton:pressed { background: #122d31; }
        QPushButton:disabled { color: #64748b; background: #192536; border-color: #263447; }
        QPushButton#primary { background: #6bd4b9; color: #092c28; font-weight: bold; border-color: #6bd4b9; }
        QPushButton#primary:disabled { background: #244541; color: #67877f; border-color: #244541; }
        QPushButton#danger { color: #f1b2a5; border-color: #674140; }
        QLineEdit, QComboBox, QPlainTextEdit { background: #0e1825; border: 1px solid #33465c; border-radius: 5px; padding: 7px; selection-background-color: #306c68; }
        QLineEdit:focus, QComboBox:focus { border-color: #6bd4b9; }
        QComboBox::drop-down { border: none; width: 24px; }
        QComboBox QAbstractItemView { background: #1d3043; selection-background-color: #306c68; }
        QTreeWidget { background: #101b29; alternate-background-color: #142132; border: 1px solid #293c51; border-radius: 5px; }
        QTreeWidget::item { padding: 10px 6px; }
        QTreeWidget::item:selected { background: #254e50; color: white; }
        QHeaderView::section { background: #1c2a3c; color: #a8bdd4; padding: 10px 8px; border: none; border-bottom: 1px solid #34465d; }
        QTabWidget::pane { border: 1px solid #2a3e54; }
        QTabBar::tab { background: #172537; color: #a8bdd4; padding: 11px 13px; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { color: #93e3ce; background: #1b303d; border-bottom-color: #6bd4b9; }
        QCheckBox { spacing: 8px; }
        QProgressBar { border: 1px solid #2c435a; border-radius: 4px; text-align: center; height: 18px; }
        QProgressBar::chunk { background: #3b9b87; }
        QScrollBar:vertical { background: #111c2a; width: 10px; }
        QScrollBar::handle:vertical { background: #3b526b; min-height: 25px; border-radius: 4px; }
        QToolTip { background: #294659; color: white; border: 1px solid #6bd4b9; padding: 5px; }
    )");
    dm::Window window; window.show();
    if(screenshot) {
        QString folder="generated-artifacts/screenshots";
        for(int i=1;i<argc-1;++i) if(QString::fromLocal8Bit(argv[i])=="--screenshot") folder=QString::fromLocal8Bit(argv[i+1]);
        QDir().mkpath(folder);
        QTimer::singleShot(1500,&app,[&window,&app,folder]{
            auto navigation=window.findChild<QListWidget*>("navigation");
            const QStringList names{"devices","space","backup"};
            for(int i=0;i<3;++i) {
                if(navigation) navigation->setCurrentRow(i);
                app.processEvents();
                window.grab().save(folder+"/"+names.at(i)+".png");
            }
            app.quit();
        });
    }
    return app.exec();
}
