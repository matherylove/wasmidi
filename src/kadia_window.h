#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <QWidget>
#include <QRect>
#include <QQueue>
#include <QPair>

#include "d3d9_renderer.h"
#include "input_manager.h"
#include "kadia_scene.h"

class QMouseEvent;
class QWheelEvent;
class RomScanner;
class RomScanProgressDialog;
class LibretroMetadataWorker;
class LibretroMetadataProgressDialog;

class KadiaWindow : public QWidget
{
    Q_OBJECT
public:
    explicit KadiaWindow(QWidget *parent = 0);
    ~KadiaWindow();

    void showOnPrimaryMonitor();
    void showWindowed();

public slots:
    void runPostStartupChecks();

protected:
    void paintEvent(QPaintEvent *event) Q_DECL_OVERRIDE;
    void showEvent(QShowEvent *event) Q_DECL_OVERRIDE;
    void resizeEvent(QResizeEvent *event) Q_DECL_OVERRIDE;
    void keyPressEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
    void mouseMoveEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    void mousePressEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    void mouseDoubleClickEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    void wheelEvent(QWheelEvent *event) Q_DECL_OVERRIDE;
    void closeEvent(QCloseEvent *event) Q_DECL_OVERRIDE;

private slots:
    void frameTick();
    void onRomDiscovered(const QString &path, const QString &hint,
                         const QString &internalTitle, const QString &format);
    void onRomRecognized(const QString &path, const QString &system, const QString &title);
    void onRomScanSummary(int recognizedCount, int unresolvedCount,
                          int testedCandidates, bool cancelled);
    void onRomScanDialogFinished(int result);
    void onMetadataDialogFinished(int result);
    void showNextRomDialog();

private:
    void ensureRenderer();
    void dispatch(InputManager::Action action);
    void processSceneCommands();

    D3D9Renderer m_renderer;
    InputManager m_input;
    KadiaScene m_scene;
    QImage m_frame;
    QTimer m_timer;
    QElapsedTimer m_clock;
    bool m_rendererAttempted;
    bool m_closing;
    bool m_monitorMode;
    QRect m_windowedGeometry;
    struct PendingRom {
        QString path;
        QString hint;
        QString internalTitle;
        QString format;
    };

    RomScanner *m_romScanner;
    RomScanProgressDialog *m_romScanDialog;
    LibretroMetadataWorker *m_metadataWorker;
    LibretroMetadataProgressDialog *m_metadataDialog;
    QQueue<PendingRom> m_romQueue;
    bool m_romDialogActive;
    bool m_romScanCancelled;
};
