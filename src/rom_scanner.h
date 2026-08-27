#pragma once

#include <QAtomicInt>
#include <QDialog>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include "input_manager.h"

class QCloseEvent;
class QKeyEvent;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTimer;


struct RomCatalogRecord
{
    QString path;
    QString classification;
    QString internalTitle;
    QString internalId;
    QString format;
    QString scrapedTitle;
    QString scrapedDescription;
    QString coverArtPath;
    QString metadataSource;
    bool automaticDetection;

    RomCatalogRecord() : automaticDetection(false) {}
};

namespace RomCatalog
{
    bool isKnown(const QString &path);
    void saveClassification(const QString &path, const QString &system);
    void saveInspectionMetadata(const QString &path, const QString &detectedSystem,
                                const QString &title, const QString &internalId,
                                const QString &format, int confidence);
    void saveDetectedRom(const QString &path, const QString &system, const QString &title,
                         const QString &internalId, const QString &format, int confidence);
    void removeEntry(const QString &path);
    QString classification(const QString &path);
    QString internalTitle(const QString &path);
    QString displayTitle(const QString &path);
    QString internalId(const QString &path);
    QString format(const QString &path);
    QString description(const QString &path);
    QString coverArtPath(const QString &path);
    bool hasScreenScraperMetadata(const QString &path);
    void saveScreenScraperMetadata(const QString &path, const QString &title,
                                   const QString &description, const QString &coverPath,
                                   const QString &source);
    void saveExternalMetadata(const QString &path, const QString &title,
                              const QString &description, const QString &coverPath,
                              const QString &source);
    bool metadataLookupCurrent(const QString &path);
    void markMetadataLookup(const QString &path, const QString &state);
    bool isAutomaticDetection(const QString &path);
    QStringList systems();
    QStringList pathsForClassification(const QString &classification);
    QStringList recognizedPaths();
    bool recordForPath(const QString &path, RomCatalogRecord *record);
    QVector<RomCatalogRecord> recognizedRecords();
}

class RomScanner : public QThread
{
    Q_OBJECT
public:
    explicit RomScanner(QObject *parent = 0);
    ~RomScanner();

public slots:
    void requestStop();

signals:
    // Heavy filesystem walking and header analysis are performed only inside
    // RomScanner::run(), never on the GUI thread.
    void discoveryProgress(const QString &currentPath, int candidatesFound);
    void analysisStarted(int totalCandidates);
    void fileProgress(const QString &path, int filePercent, int overallPercent,
                      const QString &stage, int currentIndex, int totalCandidates);

    // Only structurally ROM-like files whose console remains unresolved require
    // user interaction. Metadata is passed from the worker so the dialog never
    // opens or analyzes the ROM itself.
    void romDiscovered(const QString &path, const QString &hint,
                       const QString &internalTitle, const QString &format);
    void romRecognized(const QString &path, const QString &system, const QString &title);
    void scanSummary(int recognizedCount, int unresolvedCount,
                     int testedCandidates, bool cancelled);
    void scanFinished();

protected:
    void run() Q_DECL_OVERRIDE;

private:
    QAtomicInt m_stop;
};

class RomScanProgressDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RomScanProgressDialog(QWidget *parent = 0);
    bool scanCompleted() const;
    bool scanCancelled() const;

signals:
    void cancelRequested();

protected:
    void keyPressEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
    void closeEvent(QCloseEvent *event) Q_DECL_OVERRIDE;

public slots:
    void onDiscoveryProgress(const QString &currentPath, int candidatesFound);
    void onAnalysisStarted(int totalCandidates);
    void onFileProgress(const QString &path, int filePercent, int overallPercent,
                        const QString &stage, int currentIndex, int totalCandidates);
    void onScanSummary(int recognizedCount, int unresolvedCount,
                       int testedCandidates, bool cancelled);

private slots:
    void actionPressed();
    void pollController();

private:
    QLabel *m_title;
    QLabel *m_status;
    QLabel *m_path;
    QLabel *m_fileCaption;
    QLabel *m_overallCaption;
    QProgressBar *m_fileProgress;
    QProgressBar *m_overallProgress;
    QPushButton *m_action;
    InputManager m_input;
    QTimer *m_inputTimer;
    bool m_completed;
    bool m_cancelled;
    bool m_cancelPending;
};

class RomClassificationDialog : public QDialog
{
    Q_OBJECT
public:
    RomClassificationDialog(const QString &path, const QString &hint,
                            const QString &internalTitle, const QString &format,
                            QWidget *parent = 0);
    QString selectedSystem() const;

private slots:
    void confirmSelection();
    void deferSelection();
    void pollController();

private:
    QString m_path;
    QString m_selectedSystem;
    QLabel *m_pathLabel;
    QLabel *m_hintLabel;
    QListWidget *m_systems;
    QPushButton *m_confirm;
    QPushButton *m_later;
    InputManager m_input;
    QTimer *m_inputTimer;
};
