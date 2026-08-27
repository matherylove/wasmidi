#pragma once

#include <QAtomicInt>
#include <QDialog>
#include <QThread>
#include <QVector>

#include "rom_scanner.h"
#include "input_manager.h"

class QCloseEvent;
class QKeyEvent;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

class LibretroMetadataWorker : public QThread
{
    Q_OBJECT
public:
    explicit LibretroMetadataWorker(QObject *parent = 0);
    ~LibretroMetadataWorker();

public slots:
    void requestStop();

signals:
    void metadataStarted(int totalGames);
    void metadataProgress(const QString &title, const QString &path,
                          const QString &stage, int currentIndex,
                          int totalGames, int filePercent, int overallPercent);
    void metadataSummary(int matchedCount, int fallbackCount,
                         int notFoundCount, bool cancelled);
    void metadataFinished();

protected:
    void run() Q_DECL_OVERRIDE;

private:
    QAtomicInt m_stop;
};

class LibretroMetadataProgressDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LibretroMetadataProgressDialog(QWidget *parent = 0);
    bool completed() const;
    bool cancelled() const;

signals:
    void cancelRequested();

protected:
    void keyPressEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
    void closeEvent(QCloseEvent *event) Q_DECL_OVERRIDE;

public slots:
    void onMetadataStarted(int totalGames);
    void onMetadataProgress(const QString &title, const QString &path,
                            const QString &stage, int currentIndex,
                            int totalGames, int filePercent, int overallPercent);
    void onMetadataSummary(int matchedCount, int fallbackCount,
                           int notFoundCount, bool cancelled);

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
