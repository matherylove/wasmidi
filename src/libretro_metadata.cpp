#include "libretro_metadata.h"
#include "screenscraper_client.h"

#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDesktopWidget>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <wininet.h>
#endif

namespace {

struct SystemInfo
{
    QString datPath;
    QString thumbnailRepo;
};

struct DbEntry
{
    QString name;
    QString description;
    QString serial;
    QString developer;
    QString publisher;
    QString genre;
    QString releaseYear;
    QString region;
    QString crc;
    QString md5;
    QString sha1;
};

struct LibretroDb
{
    QVector<DbEntry> entries;
    QHash<QString, int> bySerial;
    QHash<QString, int> byCrc;
    QHash<QString, int> byMd5;
    QHash<QString, int> bySha1;
};

struct FileHashes
{
    QString crc;
    QString md5;
    QString sha1;
};

static QString appDataBase()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.mathery-kadia");
    QDir().mkpath(base);
    return base;
}

static QString databaseCacheDir()
{
    const QString path = QDir(appDataBase()).filePath(QStringLiteral("libretro/database"));
    QDir().mkpath(path);
    return path;
}

static QString coverCacheDir()
{
    const QString path = QDir(appDataBase()).filePath(QStringLiteral("covers/libretro"));
    QDir().mkpath(path);
    return path;
}

static QString safeLocalName(const QString &name)
{
    QString out = name;
    const QString bad = QStringLiteral("\\/:*?\"<>|");
    for (int i = 0; i < bad.size(); ++i)
        out.replace(bad.at(i), QLatin1Char('_'));
    return out;
}

static QString normalizedSerial(const QString &value)
{
    QString out;
    const QString upper = value.toUpper();
    for (int i = 0; i < upper.size(); ++i) {
        const QChar c = upper.at(i);
        if (c.isLetterOrNumber())
            out.append(c);
    }
    return out;
}

static QString normalizedHash(const QString &value)
{
    QString out = value.trimmed().toUpper();
    out.remove(QLatin1Char(' '));
    return out;
}

static bool systemInfoForName(const QString &system, SystemInfo *out)
{
    if (!out)
        return false;

    const QString s = system.trimmed().toLower();
    struct Mapping { const char *system; const char *dat; const char *repo; };
    static const Mapping mappings[] = {
        { "nintendo entertainment system", "dat/Nintendo - Nintendo Entertainment System.dat", "Nintendo_-_Nintendo_Entertainment_System" },
        { "super nintendo", "metadat/no-intro/Nintendo - Super Nintendo Entertainment System.dat", "Nintendo_-_Super_Nintendo_Entertainment_System" },
        { "nintendo 64", "metadat/no-intro/Nintendo - Nintendo 64.dat", "Nintendo_-_Nintendo_64" },
        { "game boy", "metadat/no-intro/Nintendo - Game Boy.dat", "Nintendo_-_Game_Boy" },
        { "game boy color", "metadat/no-intro/Nintendo - Game Boy Color.dat", "Nintendo_-_Game_Boy_Color" },
        { "game boy advance", "metadat/no-intro/Nintendo - Game Boy Advance.dat", "Nintendo_-_Game_Boy_Advance" },
        { "nintendo ds", "metadat/no-intro/Nintendo - Nintendo DS.dat", "Nintendo_-_Nintendo_DS" },
        { "nintendo 3ds", "metadat/no-intro/Nintendo - Nintendo 3DS.dat", "Nintendo_-_Nintendo_3DS" },
        { "nintendo gamecube", "dat/Nintendo - GameCube.dat", "Nintendo_-_GameCube" },
        { "nintendo wii", "dat/Nintendo - Wii.dat", "Nintendo_-_Wii" },
        { "nintendo wii u", "metadat/no-intro/Nintendo - Wii U.dat", "Nintendo_-_Wii_U" },
        { "nintendo switch", "metadat/no-intro/Nintendo - Nintendo Switch.dat", "Nintendo_-_Nintendo_Switch" },
        { "sega master system", "metadat/no-intro/Sega - Master System - Mark III.dat", "Sega_-_Master_System_-_Mark_III" },
        { "sega genesis / mega drive", "metadat/no-intro/Sega - Mega Drive - Genesis.dat", "Sega_-_Mega_Drive_-_Genesis" },
        { "sega game gear", "metadat/no-intro/Sega - Game Gear.dat", "Sega_-_Game_Gear" },
        { "sega saturn", "metadat/redump/Sega - Saturn.dat", "Sega_-_Saturn" },
        { "sega dreamcast", "metadat/redump/Sega - Dreamcast.dat", "Sega_-_Dreamcast" },
        { "playstation", "metadat/redump/Sony - PlayStation.dat", "Sony_-_PlayStation" },
        { "playstation 2", "metadat/redump/Sony - PlayStation 2.dat", "Sony_-_PlayStation_2" },
        { "playstation 3", "metadat/redump/Sony - PlayStation 3.dat", "Sony_-_PlayStation_3" },
        { "playstation portable", "metadat/redump/Sony - PlayStation Portable.dat", "Sony_-_PlayStation_Portable" },
        { "playstation vita", "metadat/no-intro/Sony - PlayStation Vita.dat", "Sony_-_PlayStation_Vita" },
        { "atari 2600", "metadat/no-intro/Atari - 2600.dat", "Atari_-_2600" },
        { "atari 5200", "metadat/no-intro/Atari - 5200.dat", "Atari_-_5200" },
        { "atari 7800", "metadat/no-intro/Atari - 7800.dat", "Atari_-_7800" },
        { "atari lynx", "metadat/no-intro/Atari - Lynx.dat", "Atari_-_Lynx" },
        { "pc engine / turbografx-16", "metadat/no-intro/NEC - PC Engine - TurboGrafx 16.dat", "NEC_-_PC_Engine_-_TurboGrafx_16" },
        { "neo geo", "dat/SNK - Neo Geo.dat", "SNK_-_Neo_Geo" },
        { "neo geo pocket", "metadat/no-intro/SNK - Neo Geo Pocket.dat", "SNK_-_Neo_Geo_Pocket" },
        { "neo geo pocket color", "metadat/no-intro/SNK - Neo Geo Pocket Color.dat", "SNK_-_Neo_Geo_Pocket_Color" },
        { "wonderswan", "metadat/no-intro/Bandai - WonderSwan.dat", "Bandai_-_WonderSwan" },
        { "wonderswan color", "metadat/no-intro/Bandai - WonderSwan Color.dat", "Bandai_-_WonderSwan_Color" },
        { "msx", "metadat/no-intro/Microsoft - MSX.dat", "Microsoft_-_MSX" },
        { "commodore 64", "metadat/no-intro/Commodore - 64.dat", "Commodore_-_64" },
        { "amiga", "metadat/no-intro/Commodore - Amiga.dat", "Commodore_-_Amiga" },
        { "xbox", "metadat/redump/Microsoft - Xbox.dat", "Microsoft_-_Xbox" },
        { "xbox 360", "metadat/redump/Microsoft - Xbox 360.dat", "Microsoft_-_Xbox_360" }
    };

    for (unsigned int i = 0; i < sizeof(mappings) / sizeof(mappings[0]); ++i) {
        if (s == QString::fromLatin1(mappings[i].system)) {
            out->datPath = QString::fromLatin1(mappings[i].dat);
            out->thumbnailRepo = QString::fromLatin1(mappings[i].repo);
            return true;
        }
    }
    return false;
}

#ifdef Q_OS_WIN
static bool httpGet(const QString &url, QByteArray *data, int *statusCode)
{
    if (!data)
        return false;
    data->clear();
    if (statusCode)
        *statusCode = 0;

    HINTERNET internet = InternetOpenW(L"MatheryKadia/1.0", INTERNET_OPEN_TYPE_PRECONFIG, 0, 0, 0);
    if (!internet)
        return false;

    const DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                        INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_NO_UI |
                        INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_SECURE;
    HINTERNET request = InternetOpenUrlW(internet,
                                         reinterpret_cast<LPCWSTR>(url.utf16()),
                                         0, 0, flags, 0);
    if (!request) {
        InternetCloseHandle(internet);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &statusSize, 0) && statusCode)
        *statusCode = int(status);

    if (status >= 400) {
        InternetCloseHandle(request);
        InternetCloseHandle(internet);
        return false;
    }

    char buffer[16384];
    DWORD bytesRead = 0;
    while (InternetReadFile(request, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        data->append(buffer, int(bytesRead));
        bytesRead = 0;
    }

    InternetCloseHandle(request);
    InternetCloseHandle(internet);
    return !data->isEmpty();
}
#else
static bool httpGet(const QString &, QByteArray *, int *) { return false; }
#endif

static QString rawDatabaseUrl(const QString &relativePath)
{
    const QByteArray encoded = QUrl::toPercentEncoding(relativePath, QByteArray("/"));
    return QStringLiteral("https://raw.githubusercontent.com/libretro/libretro-database/master/") +
           QString::fromLatin1(encoded);
}

static QString thumbnailFileName(const QString &title)
{
    QString out = title;
    const QString forbidden = QStringLiteral("&*/:`<>?\\|\"");
    for (int i = 0; i < forbidden.size(); ++i)
        out.replace(forbidden.at(i), QLatin1Char('_'));
    return out + QStringLiteral(".png");
}

static QString rawThumbnailUrl(const SystemInfo &info, const QString &title)
{
    const QByteArray encoded = QUrl::toPercentEncoding(thumbnailFileName(title));
    return QStringLiteral("https://raw.githubusercontent.com/libretro-thumbnails/") +
           info.thumbnailRepo + QStringLiteral("/master/Named_Boxarts/") +
           QString::fromLatin1(encoded);
}

static QString quotedValue(const QString &line, const QString &key)
{
    const QString needle = key + QStringLiteral(" \"");
    const int begin = line.indexOf(needle, 0, Qt::CaseInsensitive);
    if (begin < 0)
        return QString();
    const int valueBegin = begin + needle.length();
    const int end = line.indexOf(QLatin1Char('"'), valueBegin);
    if (end < 0)
        return QString();
    return line.mid(valueBegin, end - valueBegin).trimmed();
}

static QString tokenValue(const QString &line, const QString &key)
{
    const QString needle = key + QLatin1Char(' ');
    const int begin = line.indexOf(needle, 0, Qt::CaseInsensitive);
    if (begin < 0)
        return QString();
    int valueBegin = begin + needle.length();
    while (valueBegin < line.length() && line.at(valueBegin).isSpace())
        ++valueBegin;
    int end = valueBegin;
    while (end < line.length()) {
        const QChar c = line.at(end);
        if (c.isSpace() || c == QLatin1Char(')'))
            break;
        ++end;
    }
    return line.mid(valueBegin, end - valueBegin).trimmed();
}

static void indexEntry(LibretroDb *db, const DbEntry &entry)
{
    if (!db)
        return;
    const int index = db->entries.size();
    db->entries.push_back(entry);

    const QString serial = normalizedSerial(entry.serial);
    const QString crc = normalizedHash(entry.crc);
    const QString md5 = normalizedHash(entry.md5);
    const QString sha1 = normalizedHash(entry.sha1);
    if (!serial.isEmpty() && !db->bySerial.contains(serial)) db->bySerial.insert(serial, index);
    if (!crc.isEmpty() && !db->byCrc.contains(crc)) db->byCrc.insert(crc, index);
    if (!md5.isEmpty() && !db->byMd5.contains(md5)) db->byMd5.insert(md5, index);
    if (!sha1.isEmpty() && !db->bySha1.contains(sha1)) db->bySha1.insert(sha1, index);
}

static bool parseDatabase(const QString &path, LibretroDb *db)
{
    if (!db)
        return false;
    db->entries.clear();
    db->bySerial.clear();
    db->byCrc.clear();
    db->byMd5.clear();
    db->bySha1.clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);
    in.setCodec("UTF-8");
    bool inGame = false;
    bool inRomBlock = false;
    DbEntry current;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!inGame) {
            if (line.compare(QStringLiteral("game ("), Qt::CaseInsensitive) == 0) {
                inGame = true;
                inRomBlock = false;
                current = DbEntry();
            }
            continue;
        }

        if (line == QStringLiteral(")")) {
            if (inRomBlock) {
                inRomBlock = false;
                continue;
            }
            if (!current.name.isEmpty() || !current.description.isEmpty())
                indexEntry(db, current);
            inGame = false;
            continue;
        }

        if (line.startsWith(QStringLiteral("name "), Qt::CaseInsensitive) && current.name.isEmpty())
            current.name = quotedValue(line, QStringLiteral("name"));
        else if (line.startsWith(QStringLiteral("description "), Qt::CaseInsensitive))
            current.description = quotedValue(line, QStringLiteral("description"));
        else if (line.startsWith(QStringLiteral("serial "), Qt::CaseInsensitive))
            current.serial = quotedValue(line, QStringLiteral("serial"));
        else if (line.startsWith(QStringLiteral("developer "), Qt::CaseInsensitive))
            current.developer = quotedValue(line, QStringLiteral("developer"));
        else if (line.startsWith(QStringLiteral("publisher "), Qt::CaseInsensitive))
            current.publisher = quotedValue(line, QStringLiteral("publisher"));
        else if (line.startsWith(QStringLiteral("genre "), Qt::CaseInsensitive))
            current.genre = quotedValue(line, QStringLiteral("genre"));
        else if (line.startsWith(QStringLiteral("releaseyear "), Qt::CaseInsensitive))
            current.releaseYear = quotedValue(line, QStringLiteral("releaseyear"));
        else if (line.startsWith(QStringLiteral("region "), Qt::CaseInsensitive))
            current.region = quotedValue(line, QStringLiteral("region"));
        else if (line.startsWith(QStringLiteral("comment "), Qt::CaseInsensitive) && current.name.isEmpty())
            current.name = quotedValue(line, QStringLiteral("comment"));
        else if (line.startsWith(QStringLiteral("rom "), Qt::CaseInsensitive)) {
            if (line.compare(QStringLiteral("rom ("), Qt::CaseInsensitive) == 0)
                inRomBlock = true;
            if (current.crc.isEmpty()) current.crc = tokenValue(line, QStringLiteral("crc"));
            if (current.md5.isEmpty()) current.md5 = tokenValue(line, QStringLiteral("md5"));
            if (current.sha1.isEmpty()) current.sha1 = tokenValue(line, QStringLiteral("sha1"));
        }
    }
    return !db->entries.isEmpty();
}

static bool ensureDatabase(const SystemInfo &info, LibretroDb *db, bool *networkAvailable)
{
    if (networkAvailable)
        *networkAvailable = true;
    const QString localPath = QDir(databaseCacheDir()).filePath(safeLocalName(info.datPath));
    const QFileInfo cachedInfo(localPath);
    const bool stale = cachedInfo.exists() && cachedInfo.lastModified().daysTo(QDateTime::currentDateTime()) >= 7;
    if (!cachedInfo.exists() || cachedInfo.size() < 100 || stale) {
        QByteArray bytes;
        int status = 0;
        if (!httpGet(rawDatabaseUrl(info.datPath), &bytes, &status)) {
            if (networkAvailable)
                *networkAvailable = false;
            return false;
        }
        QFile out(localPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        out.write(bytes);
        out.close();
    }
    return parseDatabase(localPath, db);
}

static quint32 crc32Update(quint32 crc, const unsigned char *data, int len)
{
    static quint32 table[256];
    static bool initialized = false;
    if (!initialized) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int bit = 0; bit < 8; ++bit)
                c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        initialized = true;
    }
    quint32 c = crc;
    for (int i = 0; i < len; ++i)
        c = table[(c ^ data[i]) & 0xFFU] ^ (c >> 8);
    return c;
}

static FileHashes computeHashes(const QString &path,
                                const std::function<void(int)> &progress,
                                QAtomicInt *stop)
{
    FileHashes hashes;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return hashes;

    const qint64 total = file.size();
    qint64 done = 0;
    quint32 crc = 0xFFFFFFFFU;
    QCryptographicHash md5(QCryptographicHash::Md5);
    QCryptographicHash sha1(QCryptographicHash::Sha1);
    QElapsedTimer timer;
    timer.start();

    while (!file.atEnd()) {
        if (stop && stop->loadAcquire())
            return FileHashes();
        const QByteArray chunk = file.read(1024 * 512);
        if (chunk.isEmpty())
            break;
        done += chunk.size();
        crc = crc32Update(crc, reinterpret_cast<const unsigned char *>(chunk.constData()), chunk.size());
        md5.addData(chunk);
        sha1.addData(chunk);
        if (progress && timer.elapsed() >= 80) {
            progress(total > 0 ? int((done * 100) / total) : 100);
            timer.restart();
        }
        QThread::yieldCurrentThread();
    }

    crc ^= 0xFFFFFFFFU;
    hashes.crc = QString::number(crc, 16).toUpper().rightJustified(8, QLatin1Char('0'));
    hashes.md5 = QString::fromLatin1(md5.result().toHex()).toUpper();
    hashes.sha1 = QString::fromLatin1(sha1.result().toHex()).toUpper();
    if (progress)
        progress(100);
    return hashes;
}

static const DbEntry *matchDatabase(const LibretroDb &db, const QString &serial,
                                    const FileHashes *hashes)
{
    const QString serialKey = normalizedSerial(serial);
    if (!serialKey.isEmpty() && db.bySerial.contains(serialKey))
        return &db.entries.at(db.bySerial.value(serialKey));
    if (!hashes)
        return 0;
    const QString sha1 = normalizedHash(hashes->sha1);
    const QString md5 = normalizedHash(hashes->md5);
    const QString crc = normalizedHash(hashes->crc);
    if (!sha1.isEmpty() && db.bySha1.contains(sha1))
        return &db.entries.at(db.bySha1.value(sha1));
    if (!md5.isEmpty() && db.byMd5.contains(md5))
        return &db.entries.at(db.byMd5.value(md5));
    if (!crc.isEmpty() && db.byCrc.contains(crc))
        return &db.entries.at(db.byCrc.value(crc));
    return 0;
}

static QString metadataDescription(const DbEntry &entry)
{
    QStringList details;
    if (!entry.developer.isEmpty()) details << QStringLiteral("Developer: %1").arg(entry.developer);
    if (!entry.publisher.isEmpty() && entry.publisher.compare(entry.developer, Qt::CaseInsensitive) != 0)
        details << QStringLiteral("Publisher: %1").arg(entry.publisher);
    if (!entry.genre.isEmpty()) details << QStringLiteral("Genre: %1").arg(entry.genre);
    if (!entry.releaseYear.isEmpty()) details << QStringLiteral("Released: %1").arg(entry.releaseYear);
    if (!entry.region.isEmpty()) details << QStringLiteral("Region: %1").arg(entry.region);

    const QString base = entry.description.simplified();
    if (!base.isEmpty() && base.compare(entry.name, Qt::CaseInsensitive) != 0)
        details.prepend(base);
    return details.join(QStringLiteral("  •  "));
}

static QString downloadCover(const SystemInfo &info, const QString &title)
{
    if (title.trimmed().isEmpty() || info.thumbnailRepo.isEmpty())
        return QString();

    const QByteArray id = QCryptographicHash::hash((info.thumbnailRepo + QLatin1Char('|') + title).toUtf8(),
                                                   QCryptographicHash::Sha1).toHex();
    const QString localPath = QDir(coverCacheDir()).filePath(QString::fromLatin1(id) + QStringLiteral(".png"));
    if (QFileInfo(localPath).exists() && QFileInfo(localPath).size() > 100)
        return localPath;

    QByteArray bytes;
    int status = 0;
    if (!httpGet(rawThumbnailUrl(info, title), &bytes, &status) || bytes.size() < 100)
        return QString();

    QFile out(localPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    out.write(bytes);
    out.close();
    return localPath;
}

static QString downloadCoverCandidates(const SystemInfo &info, const QStringList &titles)
{
    QStringList tried;
    for (int i = 0; i < titles.size(); ++i) {
        const QString title = titles.at(i).simplified();
        if (title.isEmpty() || tried.contains(title, Qt::CaseInsensitive))
            continue;
        tried << title;
        const QString cover = downloadCover(info, title);
        if (!cover.isEmpty())
            return cover;
    }
    return QString();
}

static QString dialogStyle()
{
    return QStringLiteral(
        "QDialog { background:transparent; }"
        "QFrame#glassPanel { background:qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 rgba(24,33,50,238), stop:0.52 rgba(10,16,28,230), stop:1 rgba(6,10,18,238)); border:1px solid rgba(255,248,231,54); border-radius:18px; }"
        "QFrame#accentGlow { background:qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 rgba(255,240,200,8), stop:0.18 rgba(255,240,200,120), stop:0.52 rgba(166,194,255,78), stop:1 rgba(166,194,255,0)); border:none; border-radius:3px; }"
        "QLabel { color:#fff8e7; background:transparent; }"
        "QLabel#dialogTitle { color:rgba(255,248,231,0.94); }"
        "QLabel#dialogPath { color:rgba(255,248,231,0.66); }"
        "QProgressBar { border:1px solid rgba(255,248,231,52); border-radius:8px; padding:1px; background:rgba(9,14,23,180); color:#fff8e7; text-align:center; min-height:20px; }"
        "QProgressBar::chunk { border-radius:6px; background:qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 rgba(117,131,168,230), stop:0.45 rgba(184,176,158,235), stop:1 rgba(255,240,200,250)); }"
        "QPushButton { color:#fff8e7; background:qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 rgba(40,48,69,220), stop:1 rgba(18,24,37,220)); border:1px solid rgba(255,248,231,68); border-radius:12px; padding:8px 22px; min-width:104px; }"
        "QPushButton:hover, QPushButton:focus { background:qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 rgba(71,82,112,235), stop:1 rgba(26,34,54,230)); border:1px solid rgba(255,240,200,160); }" );
}

static void centerDialog(QDialog *dialog, QWidget *parent)
{
    const QRect target = parent ? parent->frameGeometry() :
        QApplication::desktop()->screenGeometry(QApplication::desktop()->primaryScreen());
    dialog->move(target.center() - QPoint(dialog->width() / 2, dialog->height() / 2));
}

} // namespace

LibretroMetadataWorker::LibretroMetadataWorker(QObject *parent)
    : QThread(parent), m_stop(0)
{
}

LibretroMetadataWorker::~LibretroMetadataWorker()
{
    requestStop();
    wait();
}

void LibretroMetadataWorker::requestStop()
{
    m_stop.storeRelease(1);
}

void LibretroMetadataWorker::run()
{
    m_stop.storeRelease(0);
    const QVector<RomCatalogRecord> all = RomCatalog::recognizedRecords();
    QVector<RomCatalogRecord> pending;
    for (int i = 0; i < all.size(); ++i) {
        if (!RomCatalog::metadataLookupCurrent(all.at(i).path))
            pending.push_back(all.at(i));
    }

    const int total = pending.size();
    emit metadataStarted(total);
    if (total == 0) {
        emit metadataSummary(0, 0, 0, false);
        emit metadataFinished();
        return;
    }

    QHash<QString, LibretroDb> databases;
    QHash<QString, bool> databaseAvailable;
    int matchedCount = 0;
    int fallbackCount = 0;
    int notFoundCount = 0;

    for (int i = 0; i < total && !m_stop.loadAcquire(); ++i) {
        const RomCatalogRecord record = pending.at(i);
        const QString display = record.internalTitle.isEmpty() ? QFileInfo(record.path).completeBaseName() : record.internalTitle;
        const int baseOverall = (i * 100) / qMax(1, total);
        emit metadataProgress(display, record.path, QStringLiteral("Preparing metadata lookup"),
                              i + 1, total, 0, baseOverall);

        bool matched = false;
        bool conclusiveAttempt = false;
        SystemInfo info;
        const bool mappedSystem = systemInfoForName(record.classification, &info);
        if (mappedSystem) {
            if (!databaseAvailable.contains(record.classification)) {
                emit metadataProgress(display, record.path, QStringLiteral("Downloading / loading Libretro database"),
                                      i + 1, total, 10, baseOverall);
                LibretroDb db;
                bool networkAvailable = true;
                const bool ok = ensureDatabase(info, &db, &networkAvailable);
                databaseAvailable.insert(record.classification, ok);
                if (ok)
                    databases.insert(record.classification, db);
            }

            if (databaseAvailable.value(record.classification, false)) {
                conclusiveAttempt = true;
                const LibretroDb &db = databases[record.classification];
                const DbEntry *entry = matchDatabase(db, record.internalId, 0);
                FileHashes hashes;
                if (!entry) {
                    emit metadataProgress(display, record.path, QStringLiteral("Matching CRC / MD5 / SHA1 against Libretro"),
                                          i + 1, total, 20, baseOverall);
                    const std::function<void(int)> progress = [this, &record, &display, i, total](int percent) {
                        if (m_stop.loadAcquire())
                            return;
                        const int filePercent = 20 + (percent * 55) / 100;
                        const int overall = qBound(0, ((i * 100) + filePercent) / qMax(1, total), 100);
                        emit metadataProgress(display, record.path,
                                              QStringLiteral("Matching CRC / MD5 / SHA1 against Libretro"),
                                              i + 1, total, filePercent, overall);
                    };
                    hashes = computeHashes(record.path, progress, &m_stop);
                    if (m_stop.loadAcquire())
                        break;
                    entry = matchDatabase(db, record.internalId, &hashes);
                }

                if (entry) {
                    const QString title = !entry->name.isEmpty() ? entry->name : entry->description;
                    emit metadataProgress(title, record.path, QStringLiteral("Downloading Libretro box art"),
                                          i + 1, total, 82,
                                          qBound(0, ((i * 100) + 82) / qMax(1, total), 100));
                    const QString cover = downloadCoverCandidates(info, QStringList()
                                                                      << title
                                                                      << entry->description
                                                                      << record.internalTitle
                                                                      << QFileInfo(record.path).completeBaseName());
                    RomCatalog::saveExternalMetadata(record.path, title,
                                                     metadataDescription(*entry), cover,
                                                     QStringLiteral("Libretro"));
                    RomCatalog::markMetadataLookup(record.path, QStringLiteral("matched"));
                    ++matchedCount;
                    matched = true;
                }
            }
        }

        if (!mappedSystem)
            conclusiveAttempt = true;

        if (!matched && ScreenScraperClient::isConfigured() && !m_stop.loadAcquire()) {
            emit metadataProgress(display, record.path, QStringLiteral("Trying optional ScreenScraper fallback"),
                                  i + 1, total, 86,
                                  qBound(0, ((i * 100) + 86) / qMax(1, total), 100));
            const ScreenScraperMetadata meta = ScreenScraperClient::fetchMetadata(record.path,
                                                                                  record.classification,
                                                                                  record.internalTitle,
                                                                                  record.internalId);
            if (meta.success) {
                RomCatalog::saveExternalMetadata(record.path, meta.title, meta.description,
                                                 meta.coverPath, QStringLiteral("ScreenScraper"));
                RomCatalog::markMetadataLookup(record.path, QStringLiteral("matched"));
                ++fallbackCount;
                matched = true;
            }
        }

        if (!matched && conclusiveAttempt) {
            RomCatalog::markMetadataLookup(record.path, QStringLiteral("notfound"));
            ++notFoundCount;
        }

        emit metadataProgress(display, record.path,
                              matched ? QStringLiteral("Metadata ready") : QStringLiteral("No matching metadata found"),
                              i + 1, total, 100, ((i + 1) * 100) / qMax(1, total));
        QThread::msleep(1);
    }

    const bool cancelled = m_stop.loadAcquire() != 0;
    emit metadataSummary(matchedCount, fallbackCount, notFoundCount, cancelled);
    emit metadataFinished();
}

LibretroMetadataProgressDialog::LibretroMetadataProgressDialog(QWidget *parent)
    : QDialog(parent)
    , m_title(new QLabel(this))
    , m_status(new QLabel(this))
    , m_path(new QLabel(this))
    , m_fileCaption(new QLabel(this))
    , m_overallCaption(new QLabel(this))
    , m_fileProgress(new QProgressBar(this))
    , m_overallProgress(new QProgressBar(this))
    , m_action(new QPushButton(QStringLiteral("Cancel"), this))
    , m_input(this)
    , m_inputTimer(new QTimer(this))
    , m_completed(false)
    , m_cancelled(false)
    , m_cancelPending(false)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setModal(false);
    setFixedSize(700, 360);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setStyleSheet(dialogStyle());

    QFrame *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("glassPanel"));
    QFrame *accent = new QFrame(panel);
    accent->setObjectName(QStringLiteral("accentGlow"));
    accent->setFixedHeight(6);

    m_title->setObjectName(QStringLiteral("dialogTitle"));
    m_title->setText(QStringLiteral("Downloading game information"));
    QFont titleFont = m_title->font();
    titleFont.setPixelSize(27); titleFont.setWeight(QFont::Light);
    m_title->setFont(titleFont);

    m_status->setText(QStringLiteral("Preparing Libretro metadata..."));
    QFont statusFont = m_status->font();
    statusFont.setPixelSize(16);
    m_status->setFont(statusFont);
    m_path->setObjectName(QStringLiteral("dialogPath"));
    m_path->setWordWrap(true);

    m_fileCaption->setText(QStringLiteral("Current game"));
    m_overallCaption->setText(QStringLiteral("Overall metadata"));
    m_fileProgress->setRange(0, 100);
    m_overallProgress->setRange(0, 100);
    m_fileProgress->setValue(0);
    m_overallProgress->setValue(0);

    QVBoxLayout *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(26, 18, 26, 24);
    panelLayout->setSpacing(8);
    panelLayout->addWidget(accent);
    panelLayout->addSpacing(3);
    panelLayout->addWidget(m_title);
    panelLayout->addWidget(m_status);
    panelLayout->addWidget(m_path);
    panelLayout->addSpacing(5);
    panelLayout->addWidget(m_fileCaption);
    panelLayout->addWidget(m_fileProgress);
    panelLayout->addWidget(m_overallCaption);
    panelLayout->addWidget(m_overallProgress);

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addStretch(); buttons->addWidget(m_action);
    panelLayout->addLayout(buttons);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(panel);

    connect(m_action, SIGNAL(clicked()), this, SLOT(actionPressed()));
    m_input.initialize();
    connect(m_inputTimer, SIGNAL(timeout()), this, SLOT(pollController()));
    m_inputTimer->start(16);
    centerDialog(this, parent);
    m_action->setFocus();
}

bool LibretroMetadataProgressDialog::completed() const { return m_completed; }
bool LibretroMetadataProgressDialog::cancelled() const { return m_cancelled; }

void LibretroMetadataProgressDialog::onMetadataStarted(int totalGames)
{
    if (totalGames <= 0) {
        m_status->setText(QStringLiteral("Game metadata is already up to date."));
        m_path->clear();
        m_fileProgress->setValue(100);
        m_overallProgress->setValue(100);
    } else {
        m_status->setText(QStringLiteral("Checking %1 new or changed game(s) against Libretro.").arg(totalGames));
    }
}

void LibretroMetadataProgressDialog::onMetadataProgress(const QString &title, const QString &path,
                                                        const QString &stage, int currentIndex,
                                                        int totalGames, int filePercent,
                                                        int overallPercent)
{
    m_status->setText(QStringLiteral("%1  —  %2").arg(title, stage));
    m_path->setText(QDir::toNativeSeparators(path));
    m_fileCaption->setText(QStringLiteral("Current game — %1%").arg(filePercent));
    m_overallCaption->setText(QStringLiteral("Overall metadata — game %1 of %2").arg(currentIndex).arg(totalGames));
    m_fileProgress->setValue(qBound(0, filePercent, 100));
    m_overallProgress->setValue(qBound(0, overallPercent, 100));
}

void LibretroMetadataProgressDialog::onMetadataSummary(int matchedCount, int fallbackCount,
                                                       int notFoundCount, bool cancelled)
{
    m_completed = true;
    m_cancelled = cancelled;
    m_cancelPending = false;
    m_fileProgress->setValue(100);
    if (!cancelled)
        m_overallProgress->setValue(100);

    if (cancelled) {
        m_status->setText(QStringLiteral("Metadata download cancelled."));
    } else if (matchedCount == 0 && fallbackCount == 0 && notFoundCount == 0) {
        m_status->setText(QStringLiteral("Game metadata is already up to date."));
    } else {
        m_status->setText(QStringLiteral("Metadata complete — Libretro: %1, fallback: %2, no match: %3")
                          .arg(matchedCount).arg(fallbackCount).arg(notFoundCount));
    }
    m_path->clear();
    m_action->setText(QStringLiteral("Continue"));
    m_action->setEnabled(true);
    m_action->setFocus();
}

void LibretroMetadataProgressDialog::actionPressed()
{
    if (m_completed) {
        accept();
        return;
    }
    if (!m_cancelPending) {
        m_cancelPending = true;
        m_action->setEnabled(false);
        m_status->setText(QStringLiteral("Stopping metadata lookup safely..."));
        emit cancelRequested();
    }
}

void LibretroMetadataProgressDialog::pollController()
{
    const InputManager::Action action = m_input.poll();
    if (action == InputManager::Accept || action == InputManager::Back)
        actionPressed();
}

void LibretroMetadataProgressDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Escape || event->key() == Qt::Key_Backspace) {
        actionPressed();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void LibretroMetadataProgressDialog::closeEvent(QCloseEvent *event)
{
    if (m_completed) {
        QDialog::closeEvent(event);
        return;
    }
    actionPressed();
    event->ignore();
}
