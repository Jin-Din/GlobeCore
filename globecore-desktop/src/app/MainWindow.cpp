#include "app/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

#include <string>

#include "app/LayerPanel.h"
#include "bridge/bridge_api.h"
#include "core/GlobeEngine.h"
#include "host/MapWidget.h"

globecore::GlobeEngine *MainWindow::engine() const {
    return map_->engine();
}

MainWindow::MainWindow(QString dataRoot, QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("GlobeCore Desktop"));

    // 初始尺寸按主屏可用工作区（扣除任务栏）比例取 72%×82% 并居中，替代硬编码 1280×800：
    // 高分屏/小屏均得到合理首启大小。逻辑像素口径，高 DPI 下由 Qt 自动放大。
    // restoreWindowState() 在构造末尾恢复上次 geometry，仅首次启动（无存档）真正使用本尺寸。
    setMinimumSize(960, 600);
    QScreen *scr = QGuiApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 800);
    const int initW = qBound(960, avail.width() * 18 / 25, 1920);
    const int initH = qBound(600, avail.height() * 41 / 50, 1080);
    resize(initW, initH);
    move(avail.center().x() - initW / 2, avail.center().y() - initH / 2);

    map_ = std::make_unique<MapWidget>(this);
    setCentralWidget(map_.get());

    layerPanel_ = new LayerPanel(map_->engine(), std::move(dataRoot), this);
    auto *dock = new QDockWidget(tr("图层"), this);
    dock->setWidget(layerPanel_);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    dock->setMinimumWidth(220);   // 防窄窗口下中文按钮被挤成竖排；逻辑像素随 DPI 缩放
    addDockWidget(Qt::LeftDockWidgetArea, dock);

    buildMenus();
    buildToolbar();
    buildStatusBar();

    connect(map_.get(), &MapWidget::cursorGeo, this, &MainWindow::onCursorGeo);
    connect(map_.get(), &MapWidget::zoomLevelChanged, this, &MainWindow::onZoomLevel);
    connect(map_.get(), &MapWidget::featurePicked, this, &MainWindow::onFeaturePicked);
    connect(layerPanel_, &LayerPanel::statusMessage, this,
            [this](const QString &t) { if (statusLabel_) statusLabel_->setText(t); });

    restoreWindowState();
}

MainWindow::~MainWindow() = default;

void MainWindow::applyFontPath(const QString &fontPath) {
    // 桌面端系统字体路径与移动端 /system/fonts 完全不同，须由宿主显式传入 CJK 字体文件，
    // 否则矢量标注文本无法渲染（引擎 FontAtlas::load 幂等、纯 CPU、线程安全）
    map_->engine()->setFontPath(fontPath.toStdString());
}

void MainWindow::buildMenus() {
    QMenu *fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    auto *aTile = fileMenu->addAction(tr("添加底图（瓦片）"), layerPanel_, &LayerPanel::addTileLayer);
    aTile->setShortcut(QKeySequence(tr("Ctrl+T")));
    auto *aRas = fileMenu->addAction(tr("添加栅格（tif/img）"), layerPanel_, &LayerPanel::addRasterLayer);
    aRas->setShortcut(QKeySequence(tr("Ctrl+R")));
    auto *aVec = fileMenu->addAction(tr("添加矢量（shp/kml/gpkg）"), layerPanel_, &LayerPanel::addVectorLayer);
    aVec->setShortcut(QKeySequence(tr("Ctrl+O")));
    fileMenu->addSeparator();
    auto *aQuit = fileMenu->addAction(tr("退出"), qApp, &QApplication::quit);
    aQuit->setShortcut(QKeySequence(tr("Ctrl+Q")));

    QMenu *viewMenu = menuBar()->addMenu(tr("视图(&V)"));
    auto *a2d = viewMenu->addAction(tr("2D 平面"), this, [this] { map_->engine()->setViewMode(0); });
    a2d->setShortcut(QKeySequence(tr("F2")));
    auto *a3d = viewMenu->addAction(tr("3D 球面"), this, [this] { map_->engine()->setViewMode(1); });
    a3d->setShortcut(QKeySequence(tr("F3")));
    viewMenu->addSeparator();
    auto *aZoomIn = viewMenu->addAction(tr("放大"), this, &MainWindow::onZoomIn);
    aZoomIn->setShortcut(QKeySequence(tr("Ctrl++")));
    auto *aZoomOut = viewMenu->addAction(tr("缩小"), this, &MainWindow::onZoomOut);
    aZoomOut->setShortcut(QKeySequence(tr("Ctrl+-")));

    QMenu *helpMenu = menuBar()->addMenu(tr("帮助(&H)"));
    helpMenu->addAction(tr("关于"), this, &MainWindow::onAbout);
}

void MainWindow::buildToolbar() {
    auto *tb = addToolBar(tr("主工具栏"));
    tb->setObjectName(QStringLiteral("MainToolbar"));
    tb->setMovable(false);

    auto *aTile = tb->addAction(tr("底图"));
    connect(aTile, &QAction::triggered, layerPanel_, &LayerPanel::addTileLayer);
    auto *aRas = tb->addAction(tr("栅格"));
    connect(aRas, &QAction::triggered, layerPanel_, &LayerPanel::addRasterLayer);
    auto *aVec = tb->addAction(tr("矢量"));
    connect(aVec, &QAction::triggered, layerPanel_, &LayerPanel::addVectorLayer);

    tb->addSeparator();

    auto *a2d = tb->addAction(tr("2D"));
    connect(a2d, &QAction::triggered, this, [this] { map_->engine()->setViewMode(0); });
    auto *a3d = tb->addAction(tr("3D"));
    connect(a3d, &QAction::triggered, this, [this] { map_->engine()->setViewMode(1); });

    tb->addSeparator();

    auto *aZoomIn = tb->addAction(tr("+"));
    connect(aZoomIn, &QAction::triggered, this, &MainWindow::onZoomIn);
    auto *aZoomOut = tb->addAction(tr("-"));
    connect(aZoomOut, &QAction::triggered, this, &MainWindow::onZoomOut);
}

void MainWindow::buildStatusBar() {
    coordLabel_ = new QLabel(QStringLiteral("0.000000, 0.000000"), this);
    levelLabel_ = new QLabel(tr("L--"), this);
    statusLabel_ = new QLabel(tr("就绪"), this);
    statusBar()->addWidget(statusLabel_, 1);
    statusBar()->addPermanentWidget(levelLabel_);
    statusBar()->addPermanentWidget(coordLabel_);

    // 引擎的加载中状态是纯查询、不触发重绘（对齐移动端「加载中」胶囊轮询口径），故用定时器轮询
    auto *poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, [this] {
        globecore::GlobeEngine *e = map_->engine();
        const bool busy = e->hasVectorLoading() || e->hasRasterImageLoading();
        statusLabel_->setText(busy ? tr("图层加载中…") : tr("就绪"));
    });
    poll->start(400);
}

void MainWindow::onCursorGeo(double lonDeg, double latDeg) {
    coordLabel_->setText(QStringLiteral("%1, %2")
                                 .arg(lonDeg, 0, 'f', 6)
                                 .arg(latDeg, 0, 'f', 6));
}

void MainWindow::onZoomLevel(int level) {
    levelLabel_->setText(QStringLiteral("L%1").arg(level));
}

void MainWindow::onFeaturePicked(int layerIndex, long long fid, double /*lon*/, double /*lat*/) {
    const QString path = layerPanel_->vectorLayerPath(layerIndex);
    if (path.isEmpty()) {
        statusLabel_->setText(tr("命中要素但图层路径未知（索引 %1）").arg(layerIndex));
        return;
    }
    // 桌面直连 gcbridge 核心（JSON 文本回传），不经 JNI/NAPI 编解码层
    const std::string json = gcbridge::getFeatureAttributes(path.toStdString(), fid);
    if (json.empty()) {
        QMessageBox::information(this, tr("要素属性"), tr("该要素无属性或未命中（FID %1）").arg(fid));
        return;
    }
    QMessageBox::information(this, tr("要素属性（FID %1）").arg(fid),
                             QString::fromStdString(json));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveWindowState();
    QMainWindow::closeEvent(event);
}

void MainWindow::saveWindowState() {
    QSettings s;
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    s.setValue(QStringLiteral("windowState"), saveState());
    // 持久化相机姿态（中心经纬度/高度/heading/tilt），下次启动恢复上次视角
    const globecore::Camera cam = map_->engine()->getCamera();
    s.beginGroup(QStringLiteral("camera"));
    s.setValue(QStringLiteral("lon"), cam.longitude);
    s.setValue(QStringLiteral("lat"), cam.latitude);
    s.setValue(QStringLiteral("alt"), cam.altitude);
    s.setValue(QStringLiteral("heading"), cam.heading);
    s.setValue(QStringLiteral("tilt"), cam.tilt);
    s.endGroup();
    s.setValue(QStringLiteral("viewMode"), map_->engine()->viewMode());
}

void MainWindow::restoreWindowState() {
    QSettings s;
    const QByteArray geom = s.value(QStringLiteral("geometry")).toByteArray();
    if (!geom.isEmpty()) {
        restoreGeometry(geom);
    }
    const QByteArray state = s.value(QStringLiteral("windowState")).toByteArray();
    if (!state.isEmpty()) {
        restoreState(state);
    }
    // 相机/视图模式须在 GL 初始化后恢复（initializeGL 会设默认相机覆盖之），
    // 故延迟到事件循环空闲时执行；首次启动无存值时引擎默认中国全域不受影响
    QTimer::singleShot(0, this, [this] {
        QSettings s2;
        s2.beginGroup(QStringLiteral("camera"));
        if (s2.contains(QStringLiteral("lon"))) {
            globecore::Camera cam;
            cam.longitude = s2.value(QStringLiteral("lon"), 116.0).toDouble();
            cam.latitude = s2.value(QStringLiteral("lat"), 35.0).toDouble();
            cam.altitude = s2.value(QStringLiteral("alt"), 3000000.0).toDouble();
            cam.heading = s2.value(QStringLiteral("heading"), 0.0).toDouble();
            cam.tilt = s2.value(QStringLiteral("tilt"), 0.0).toDouble();
            map_->engine()->setCamera(cam);
        }
        s2.endGroup();
        const int vm = s2.value(QStringLiteral("viewMode"), -1).toInt();
        if (vm == 0 || vm == 1) {
            map_->engine()->setViewMode(vm);
        }
    });
}

void MainWindow::onZoomIn() {
    map_->zoomCentered(0.5);
}

void MainWindow::onZoomOut() {
    map_->zoomCentered(2.0);
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("关于 GlobeCore Desktop"),
            tr("<h3>GlobeCore Desktop</h3>"
               "<p>C++ 地理引擎桌面宿主 —— 基于 Qt5 + OpenGL ES 2.0 (ANGLE)。</p>"
               "<p>支持：瓦片底图 / 栅格影像 / 矢量数据 / 2D 平面 / 3D 球面。</p>"
               "<p style='color:gray;'>构建日期：" __DATE__ "</p>"));
}
