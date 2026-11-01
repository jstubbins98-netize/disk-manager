#include "scanner.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace dm {
QString category(const QString &s) {
    const QString ext = s.toLower();
    const QList<QPair<QString,QStringList>> types = {
        {"Documents",{"pdf","txt","md","doc","docx","odt","xls","xlsx","csv","pptx"}},
        {"Images",{"png","jpg","jpeg","webp","svg","gif","raw","heic"}},
        {"Video",{"mp4","mkv","mov","avi","webm"}},
        {"Audio",{"mp3","flac","wav","ogg","m4a"}},
        {"Archives",{"zip","gz","xz","tar","7z","bz2","zst"}},
        {"Code",{"cpp","c","h","hpp","js","ts","tsx","py","rs","go","java","json","yaml","yml","html","css"}}
    };
    for (const auto &t : types) if (t.second.contains(ext)) return t.first;
    return "Other";
}
ScanResult scanFolder(const QString &root, const QStringList &excluded,
                       const std::shared_ptr<std::atomic_bool> &cancel) {
    ScanResult result; result.root = canonical(root);
    if (result.root.isEmpty() || !QFileInfo(root).isDir()) { result.errors << "The source directory does not exist."; return result; }
    struct stat rootStat{};
    if (::stat(QFile::encodeName(result.root).constData(),&rootStat) != 0) { result.errors << "Cannot stat the source directory."; return result; }
    const fs::path start(QFile::encodeName(result.root).constData());
    std::error_code error;
    fs::recursive_directory_iterator it(start,fs::directory_options::none,error), end;
    if (error) { result.errors << QString::fromStdString(error.message()); return result; }
    while (it != end) {
        if (cancel && cancel->load()) { result.cancelled = true; break; }
        const QString path = QFile::decodeName(it->path().native().c_str());
        const QFileInfo info(path);
        const bool isDir = it->is_directory(error);
        if (error) { result.errors << path + ": " + QString::fromStdString(error.message()); error.clear(); }
        struct stat st{};
        const bool statOk = ::lstat(QFile::encodeName(path).constData(),&st) == 0;
        bool exclude = excluded.contains(info.fileName());
        for (const auto &ex : excluded) {
            if (ex.startsWith('/') && within(path,QDir::cleanPath(ex))) exclude = true;
            else if (ex.contains('/') && within(path,QDir(result.root).filePath(ex))) exclude = true;
        }
        if (!statOk || exclude || info.isSymLink() || st.st_dev != rootStat.st_dev) {
            if (isDir) it.disable_recursion_pending();
            if (!statOk) result.errors << "Cannot read metadata: " + path;
        } else if (S_ISREG(st.st_mode)) {
            // Count logical size once per path: conservative for backups of hard links / sparse files.
            result.total += st.st_size; ++result.files;
            result.categories[category(info.suffix())] += st.st_size;
            const QString rel = QDir(result.root).relativeFilePath(path);
            result.folders[rel.contains('/') ? rel.section('/',0,0) : "(files in this folder)"] += st.st_size;
            result.entries << FileEntry{path,st.st_size,st.st_atime};
        } else if (S_ISLNK(st.st_mode)) {
            // No link traversal. rsync will preserve the link itself.
        }
        it.increment(error);
        if (error) { result.errors << "Cannot scan part of " + path + ": " + QString::fromStdString(error.message()); error.clear(); }
    }
    std::sort(result.entries.begin(),result.entries.end(),[](const FileEntry&a,const FileEntry&b){ return a.size > b.size; });
    return result;
}
QVector<QStringList> findDuplicates(const ScanResult &scan, const std::shared_ptr<std::atomic_bool> &cancel) {
    QMap<qint64,QStringList> sizes;
    for (const auto &f : scan.entries) if (f.size > 0) sizes[f.size] << f.path;
    QVector<QStringList> result;
    for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) {
        if (it.value().size() < 2) continue;
        QMap<QString,QStringList> hashes;
        QSet<QString> inodes;
        for (const auto &path : it.value()) {
            if (cancel && cancel->load()) return result;
            struct stat st{};
            if (::lstat(QFile::encodeName(path).constData(),&st) != 0 || !S_ISREG(st.st_mode)) continue;
            QString inode = QString::number(st.st_dev) + ":" + QString::number(st.st_ino);
            if (inodes.contains(inode)) continue; // Hard links are not reclaimable duplicate data.
            inodes.insert(inode);
            QString hash = checksum(path);
            if (!hash.isEmpty()) hashes[hash] << path;
        }
        for (const auto &group : hashes) if (group.size() > 1) result << group;
    }
    return result;
}
}
