#include "kadia_window.h"
#include "background_settings.h"
#include "rom_scanner.h"
#include "libretro_metadata.h"
#include "windspro_bootstrap.h"
#include "ui_model.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QDesktopWidget>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>

KadiaWindow::KadiaWindow(QWidget *parent)
    : QWidget(parent)
    , m_input(this)
    , m_frame(m_scene.logicalSize(), QImage::Format_ARGB32_Premultiplied)
    , m_rendererAttempted(false)
    , m_closing(false)
    , m_monitorMode(false)
    , m_romScanner(0)
    , m_romScanDialog(0)
    , m_metadataWorker(0)
    , m_metadataDialog(0)
    , m_romDialogActive(false)
    , m_romScanCancelled(false)
{
    setWindowTitle(QStringLiteral("Mathery Kadia!"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
    resize(m_scene.logicalSize());
    m_scene.setViewportSize(size());
    BackgroundSettings::applyToScene(&m_scene, BackgroundSettings::load());
    refreshKadiaGameLibrary();
    setKadiaUnknownRoms(RomCatalog::pathsForClassification(QStringLiteral("Unknown")));

    m_input.initialize();

    // Present() is VSync-capped by D3D9. A zero-interval precise timer feeds the
    // renderer immediately after each vertical blank instead of hard-capping at 60 Hz.
    m_timer.setInterval(0);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(frameTick()));
}

KadiaWindow::~KadiaWindow()
{
    m_timer.stop();
    if (m_romScanner) {
        m_romScanner->requestStop();
        m_romScanner->wait();
    }
    if (m_romScanDialog)
        m_romScanDialog->close();
    if (m_metadataWorker) {
        m_metadataWorker->requestStop();
        m_metadataWorker->wait();
    }
    if (m_metadataDialog)
        m_metadataDialog->close();
    m_renderer.shutdown();
}

void KadiaWindow::showOnPrimaryMonitor()
{
    QDesktopWidget *desktop = QApplication::desktop();
    const int primary = desktop ? desktop->primaryScreen() : 0;
    const QRect target = desktop ? desktop->screenGeometry(primary) : QRect(0, 0, 1280, 720);

    if (!m_monitorMode && isVisible())
        m_windowedGeometry = geometry();

    m_monitorMode = true;
    setGeometry(target);
    m_scene.setViewportSize(target.size());
    show();
    activateWindow();
}

void KadiaWindow::showWindowed()
{
    QDesktopWidget *desktop = QApplication::desktop();
    const int primary = desktop ? desktop->primaryScreen() : 0;
    const QRect available = desktop ? desktop->availableGeometry(primary) : QRect(0, 0, 1280, 720);

    QSize wanted(1280, 720);
    if (wanted.width() > available.width() || wanted.height() > available.height()) {
        const QSize safe(qMax(320, static_cast<int>(available.width() * 0.90)),
                         qMax(240, static_cast<int>(available.height() * 0.90)));
        wanted.scale(safe, Qt::KeepAspectRatio);
    }

    QRect target(QPoint(0, 0), wanted);
    target.moveCenter(available.center());
    m_monitorMode = false;
    m_windowedGeometry = target;
    setGeometry(target);
    m_scene.setViewportSize(target.size());
    show();
    activateWindow();
}

void KadiaWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    // Intentionally empty. Direct3D owns presentation.
}

void KadiaWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ensureRenderer();
    m_clock.start();
    if (!m_timer.isActive())
        m_timer.start();
}

void KadiaWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_scene.setViewportSize(event->size());
    if (m_renderer.isReady())
        m_renderer.resize(qMax(1, event->size().width()), qMax(1, event->size().height()));
}

void KadiaWindow::keyPressEvent(QKeyEvent *event)
{
    if ((m_romScanDialog && m_romScanDialog->isVisible()) ||
        (m_metadataDialog && m_metadataDialog->isVisible()) || m_romDialogActive) {
        event->accept();
        return;
    }

    if (event->isAutoRepeat() &&
        (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down ||
         event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        event->accept();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Up:
        m_scene.handle(KadiaScene::MoveUp);
        break;
    case Qt::Key_Down:
        m_scene.handle(KadiaScene::MoveDown);
        break;
    case Qt::Key_Left:
        m_scene.handle(KadiaScene::MoveLeft);
        break;
    case Qt::Key_Right:
        m_scene.handle(KadiaScene::MoveRight);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        m_scene.handle(KadiaScene::Accept);
        break;
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
        m_scene.handle(KadiaScene::Back);
        break;
    case Qt::Key_F:
        m_scene.cycleWordmarkFont();
        break;
    case Qt::Key_F11:
        m_monitorMode ? showWindowed() : showOnPrimaryMonitor();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void KadiaWindow::mouseMoveEvent(QMouseEvent *event)
{
    if ((m_romScanDialog && m_romScanDialog->isVisible()) ||
        (m_metadataDialog && m_metadataDialog->isVisible()) || m_romDialogActive) {
        event->accept();
        return;
    }
    if (m_scene.hoverAt(event->localPos()))
        event->accept();
    else
        QWidget::mouseMoveEvent(event);
}

void KadiaWindow::mousePressEvent(QMouseEvent *event)
{
    if ((m_romScanDialog && m_romScanDialog->isVisible()) ||
        (m_metadataDialog && m_metadataDialog->isVisible()) || m_romDialogActive) {
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        m_scene.handle(KadiaScene::Back);
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_scene.clickAt(event->localPos())) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KadiaWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if ((m_romScanDialog && m_romScanDialog->isVisible()) ||
        (m_metadataDialog && m_metadataDialog->isVisible()) || m_romDialogActive) {
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_scene.doubleClickAt(event->localPos())) {
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void KadiaWindow::wheelEvent(QWheelEvent *event)
{
    if ((m_romScanDialog && m_romScanDialog->isVisible()) ||
        (m_metadataDialog && m_metadataDialog->isVisible()) || m_romDialogActive) {
        event->accept();
        return;
    }
    const int delta = event->angleDelta().y();
    if (delta != 0) {
        m_scene.wheelAt(QPointF(event->pos()), delta);
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void KadiaWindow::closeEvent(QCloseEvent *event)
{
    m_closing = true;
    m_timer.stop();
    m_renderer.shutdown();
    QWidget::closeEvent(event);
}

void KadiaWindow::frameTick()
{
    if (m_closing)
        return;

    ensureRenderer();
    if (!m_renderer.isReady())
        return;

    const qint64 elapsedNs = m_clock.isValid() ? m_clock.nsecsElapsed() : 0;
    if (m_clock.isValid())
        m_clock.restart();
    else
        m_clock.start();
    const double fallbackDt = 1.0 / static_cast<double>(qMax(1, m_renderer.refreshRate()));
    double dt = elapsedNs > 0 ? static_cast<double>(elapsedNs) / 1000000000.0 : fallbackDt;
    if (dt > 0.1)
        dt = 0.1;

    // Keep polling to update edge state, but do not navigate the main menu
    // behind a scanner/classification dialog. Dialogs use their own XInput
    // manager while the renderer continues at the monitor refresh rate.
    const InputManager::Action action = m_input.poll();
    const bool scannerDialogVisible = m_romScanDialog && m_romScanDialog->isVisible();
    const bool metadataDialogVisible = m_metadataDialog && m_metadataDialog->isVisible();
    if (!scannerDialogVisible && !metadataDialogVisible && !m_romDialogActive && action != InputManager::None)
        dispatch(action);
    if (!scannerDialogVisible && !metadataDialogVisible && !m_romDialogActive)
        processSceneCommands();
    m_scene.setControllerConnected(m_input.controllerConnected());

    m_scene.setViewportSize(size());
    m_scene.update(dt);
    m_scene.render(m_frame);
    if (!m_renderer.present(m_frame) && !m_renderer.lastError().isEmpty())
        qWarning() << m_renderer.lastError();
}

void KadiaWindow::runPostStartupChecks()
{
    if (m_closing)
        return;

    WinDSProBootstrap::offerOnce(this);

    if (!m_romScanner) {
        m_romScanCancelled = false;
        m_romScanner = new RomScanner(this);
        m_romScanDialog = new RomScanProgressDialog(this);

        connect(m_romScanner, SIGNAL(discoveryProgress(QString,int)),
                m_romScanDialog, SLOT(onDiscoveryProgress(QString,int)));
        connect(m_romScanner, SIGNAL(analysisStarted(int)),
                m_romScanDialog, SLOT(onAnalysisStarted(int)));
        connect(m_romScanner, SIGNAL(fileProgress(QString,int,int,QString,int,int)),
                m_romScanDialog, SLOT(onFileProgress(QString,int,int,QString,int,int)));
        connect(m_romScanner, SIGNAL(scanSummary(int,int,int,bool)),
                m_romScanDialog, SLOT(onScanSummary(int,int,int,bool)));

        connect(m_romScanner, SIGNAL(romDiscovered(QString,QString,QString,QString)),
                this, SLOT(onRomDiscovered(QString,QString,QString,QString)));
        connect(m_romScanner, SIGNAL(scanSummary(int,int,int,bool)),
                this, SLOT(onRomScanSummary(int,int,int,bool)));
        connect(m_romScanDialog, SIGNAL(cancelRequested()),
                m_romScanner, SLOT(requestStop()), Qt::DirectConnection);
        connect(m_romScanDialog, SIGNAL(finished(int)),
                this, SLOT(onRomScanDialogFinished(int)));

        m_romScanDialog->show();
        m_romScanDialog->raise();
        m_romScanDialog->activateWindow();
        m_romScanner->start(QThread::LowestPriority);
    }
}

void KadiaWindow::onRomDiscovered(const QString &path, const QString &hint,
                                  const QString &internalTitle, const QString &format)
{
    PendingRom candidate;
    candidate.path = path;
    candidate.hint = hint;
    candidate.internalTitle = internalTitle;
    candidate.format = format;
    m_romQueue.enqueue(candidate);

    // Do not interrupt the scan with classification prompts. The worker keeps
    // running independently and unresolved dialogs begin only after the scan
    // progress window is dismissed.
}

void KadiaWindow::onRomRecognized(const QString &path, const QString &system, const QString &title)
{
    Q_UNUSED(path);
    Q_UNUSED(system);
    Q_UNUSED(title);
    // Intentionally batched. Rebuilding/sorting the complete GUI library once
    // per recognized ROM can flood the main event loop during a large scan.
}

void KadiaWindow::onRomScanSummary(int recognizedCount, int unresolvedCount,
                                   int testedCandidates, bool cancelled)
{
    Q_UNUSED(recognizedCount);
    Q_UNUSED(unresolvedCount);
    Q_UNUSED(testedCandidates);
    m_romScanCancelled = cancelled;
    if (cancelled)
        m_romQueue.clear();
}

void KadiaWindow::onRomScanDialogFinished(int result)
{
    Q_UNUSED(result);
    if (m_romScanDialog) {
        m_romScanDialog->deleteLater();
        m_romScanDialog = 0;
    }

    activateWindow();
    setFocus();

    // Metadata is deliberately a second phase. ROM discovery/header analysis is
    // complete before any database download, hashing for identification or cover
    // download begins. All of that work runs in its own low-priority thread.
    if (!m_romScanCancelled && !m_metadataWorker) {
        m_metadataWorker = new LibretroMetadataWorker(this);
        m_metadataDialog = new LibretroMetadataProgressDialog(this);

        connect(m_metadataWorker, SIGNAL(metadataStarted(int)),
                m_metadataDialog, SLOT(onMetadataStarted(int)));
        connect(m_metadataWorker, SIGNAL(metadataProgress(QString,QString,QString,int,int,int,int)),
                m_metadataDialog, SLOT(onMetadataProgress(QString,QString,QString,int,int,int,int)));
        connect(m_metadataWorker, SIGNAL(metadataSummary(int,int,int,bool)),
                m_metadataDialog, SLOT(onMetadataSummary(int,int,int,bool)));
        connect(m_metadataDialog, SIGNAL(cancelRequested()),
                m_metadataWorker, SLOT(requestStop()), Qt::DirectConnection);
        connect(m_metadataDialog, SIGNAL(finished(int)),
                this, SLOT(onMetadataDialogFinished(int)));

        m_metadataDialog->show();
        m_metadataDialog->raise();
        m_metadataDialog->activateWindow();
        m_metadataWorker->start(QThread::LowestPriority);
        return;
    }

    refreshKadiaGameLibrary();
    setKadiaUnknownRoms(RomCatalog::pathsForClassification(QStringLiteral("Unknown")));
    if (!m_romScanCancelled && !m_romQueue.isEmpty())
        QTimer::singleShot(0, this, SLOT(showNextRomDialog()));
}

void KadiaWindow::onMetadataDialogFinished(int result)
{
    Q_UNUSED(result);
    if (m_metadataDialog) {
        m_metadataDialog->deleteLater();
        m_metadataDialog = 0;
    }

    if (m_metadataWorker && m_metadataWorker->isRunning()) {
        // The summary is emitted immediately before the worker exits. Never wait
        // here: keeping the QThread object parented to Kadia lets it finish its
        // last instructions without blocking the Windows message/render loop.
        m_metadataWorker->requestStop();
    }

    // This refresh is memory/catalog only; it never reopens ROM contents.
    refreshKadiaGameLibrary();
    setKadiaUnknownRoms(RomCatalog::pathsForClassification(QStringLiteral("Unknown")));

    activateWindow();
    setFocus();

    if (!m_romScanCancelled && !m_romQueue.isEmpty())
        QTimer::singleShot(0, this, SLOT(showNextRomDialog()));
}

void KadiaWindow::showNextRomDialog()
{
    if (m_romDialogActive || m_romQueue.isEmpty() || m_closing ||
        (m_romScanDialog && m_romScanDialog->isVisible()) ||
        (m_metadataDialog && m_metadataDialog->isVisible()))
        return;

    m_romDialogActive = true;
    const PendingRom candidate = m_romQueue.dequeue();
    RomClassificationDialog dialog(candidate.path, candidate.hint,
                                   candidate.internalTitle, candidate.format, this);
    if (dialog.exec() == QDialog::Accepted && !dialog.selectedSystem().isEmpty()) {
        RomCatalog::saveClassification(candidate.path, dialog.selectedSystem());
        setKadiaUnknownRoms(RomCatalog::pathsForClassification(QStringLiteral("Unknown")));
        if (dialog.selectedSystem().compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) != 0 &&
            dialog.selectedSystem().compare(QStringLiteral("None (ignore)"), Qt::CaseInsensitive) != 0)
            updateKadiaGameFromPath(candidate.path);
    }
    m_romDialogActive = false;

    if (!m_romQueue.isEmpty())
        QTimer::singleShot(0, this, SLOT(showNextRomDialog()));
}

void KadiaWindow::processSceneCommands()
{
    const KadiaScene::Command command = m_scene.takePendingCommand();
    if (command == KadiaScene::OpenBackgroundSettings) {
        BackgroundSettingsDialog dialog(this);
        if (dialog.exec() == QDialog::Accepted)
            BackgroundSettings::applyToScene(&m_scene, dialog.preferences());
    }
}

void KadiaWindow::ensureRenderer()
{
    if (m_renderer.isReady() || m_rendererAttempted)
        return;

    m_rendererAttempted = true;
    const WId id = winId();
    if (!m_renderer.initialize(id, qMax(1, width()), qMax(1, height()))) {
        qCritical() << "Kadia could not initialize Direct3D 9:" << m_renderer.lastError();
        setWindowTitle(QStringLiteral("Mathery Kadia! - Direct3D 9 initialization failed"));
    } else {
        qDebug() << "Renderer:" << m_renderer.description();
    }
}

void KadiaWindow::dispatch(InputManager::Action action)
{
    switch (action) {
    case InputManager::Up: m_scene.handle(KadiaScene::MoveUp); break;
    case InputManager::Down: m_scene.handle(KadiaScene::MoveDown); break;
    case InputManager::Left: m_scene.handle(KadiaScene::MoveLeft); break;
    case InputManager::Right: m_scene.handle(KadiaScene::MoveRight); break;
    case InputManager::Accept: m_scene.handle(KadiaScene::Accept); break;
    case InputManager::Back: m_scene.handle(KadiaScene::Back); break;
    default: break;
    }
}
