#pragma once
#include "backup.h"
#include "devices.h"
#include <QMainWindow>
#include <QWidget>
#include <QFutureWatcher>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QCloseEvent>

namespace dm {
QLabel *heading(const QString &text);
QLabel *note(const QString &text);
void message(QWidget *parent, bool ok, const QString &text);

class SpacePage : public QWidget {
    Q_OBJECT
public:
    explicit SpacePage(QWidget *parent = nullptr);
    bool busy() const { return busy_; }
    ~SpacePage() override;
private:
    void scan(const QString &path);
    void display(const ScanResult &scan);
    void duplicateScan();
    void trashSelected();
    QLineEdit *path_;
    QLabel *status_;
    QComboBox *view_, *units_;
    QTreeWidget *files_, *summary_, *duplicates_, *history_;
    QStackedWidget *visuals_;
    QWidget *treemap_, *sunburst_;
    QPushButton *scanButton_, *duplicatesButton_;
    ScanResult result_;
    std::shared_ptr<std::atomic_bool> cancel_;
    QFutureWatcher<ScanResult> scanWatcher_;
    QFutureWatcher<QVector<QStringList>> duplicateWatcher_;
    bool busy_ = false;
};
class BackupPage : public QWidget {
    Q_OBJECT
public:
    explicit BackupPage(QWidget *parent = nullptr);
    bool busy() const { return engine_.running() || checking_; }
private:
    BackupOptions options() const;
    void refreshDestinations();
    void checkSpace();
    void invalidate();
    void setBusy(bool busy);
    void restore();
    void schedule();
    QLineEdit *source_, *destination_, *exclusions_;
    QComboBox *preset_, *destinations_, *mode_;
    QCheckBox *verify_, *local_;
    QLabel *space_, *health_;
    QPushButton *check_, *run_, *pause_, *cancel_;
    QProgressBar *progress_;
    QPlainTextEdit *log_;
    BackupEngine engine_;
    BackupPlan plan_;
    bool checking_ = false, paused_ = false;
    quint64 revision_ = 0;
};
class Window : public QMainWindow {
    Q_OBJECT
public:
    Window();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void refresh();
    Device selected() const;
    void formatSelected();
    void action(const QString &name);
    QTreeWidget *devices_;
    QLabel *deviceStatus_;
    QWidget *deviceActions_;
    QPushButton *refresh_;
    QCheckBox *readOnly_;
    DeviceList list_;
    DeviceService service_;
    SpacePage *space_;
    BackupPage *backup_;
    bool deviceBusy_ = false;
};
}
