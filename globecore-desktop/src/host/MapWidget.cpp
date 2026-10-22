#include "host/MapWidget.h"

#include <cmath>

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QScreen>
#include <QSurfaceFormat>
#include <QWheelEvent>

#ifdef GLOBECORE_DESKTOP_GLEW
#include <GL/glew.h>
#endif

namespace {

// Qt5/Qt6 鼠标·滚轮坐标取法不同（Qt6 统一 position()），收敛到一个口子
inline QPointF pointOf(const QMouseEvent *e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->localPos();
#endif
}

inline QPointF pointOf(const QWheelEvent *e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->posF();
#endif
}

} // namespace

MapWidget::MapWidget(QWidget *parent) : QOpenGLWidget(parent) {
    setMouseTracking(true);
    // 无 GL 上下文依赖：构造只做对象图搭建，GL 相关一律留到 initializeGL
    engine_ = std::make_unique<globecore::GlobeEngine>();
    engine_->setViewMode(0); // 2D 墨卡托起步，与移动端默认口径一致
    // @Jin Provider 回调可能来自调度/worker 线程，统一排队到 MapWidget 所属 GUI 线程后发 signal。
    engine_->setTerrainProviderEventCallback([this](const std::string &eventJson) {
        const QString payload = QString::fromStdString(eventJson);
        QMetaObject::invokeMethod(
                this, [this, payload] { emit terrainProviderEvent(payload); }, Qt::QueuedConnection);
    });
}

MapWidget::~MapWidget() {
    if (!engine_) {
        return;
    }
    // @Jin 先断地形事件与渲染回调，再释放 GL，避免析构期间 worker 回投。
    engine_->setTerrainProviderEventCallback(nullptr);
    engine_->setRenderCallback(nullptr);
    // GL 资源（VBO/纹理/着色器）须在上下文销毁前释放；QOpenGLWidget 的上下文归属本控件
    makeCurrent();
    engine_->releaseGl();
    doneCurrent();
}

QPointF MapWidget::toFramebuffer(const QPointF &pos) const {
    const qreal dpr = devicePixelRatioF();
    return QPointF(pos.x() * dpr, pos.y() * dpr);
}

void MapWidget::initializeGL() {
#ifdef GLOBECORE_DESKTOP_GLEW
    // glewInit 须在当前上下文已绑定时执行，且全进程一次
    static bool glewDone = false;
    if (!glewDone) {
        glewExperimental = GL_TRUE; // 桌面 GL 核心配置下 glGetProcAddress 兜底需要
        if (glewInit() != GLEW_OK) {
            qWarning("MapWidget: glewInit 失败，GLES2 兼容层不可用");
            return;
        }
        glewDone = true;
    }
#endif

    // 引擎内嵌 GLSL ES 1.00 着色器，走 GLES2 后端时此处上下文即原生 ES2，无需额外处理
    engine_->surfaceCreated();

    // 对应 Android 注入 requestRender / 鸿蒙注入 MapRenderHost 唤醒：
    // 回调可能在 worker 或 GL 线程触发，须跨线程安全 → 一律排队到 GUI 线程调 update()
    engine_->setRenderCallback([this] {
        QMetaObject::invokeMethod(
                this, [this] { update(); }, Qt::QueuedConnection);
    });

    const qreal dpr = devicePixelRatioF();
    engine_->setDisplayDensity(dpr);

    // 默认视角：中国全域。altitude 驱动 zoom 口径（Navigator 的 altitude↔zoom 换算）
    globecore::Camera cam;
    cam.longitude = 104.0;
    cam.latitude = 35.0;
    cam.altitude = 9000000.0;
    engine_->setCamera(cam);

    inited_ = true;
}

void MapWidget::resizeGL(int w, int h) {
    if (!engine_) {
        return;
    }
    // QOpenGLWidget 的 resizeGL 收逻辑像素，而引擎的 viewportHeight()/panByPixels()
    // 同单位为 framebuffer 像素（对齐移动端 setupViewport 用物理像素的口径），此处换算一次
    const qreal dpr = devicePixelRatioF();
    engine_->surfaceChanged(qRound(w * dpr), qRound(h * dpr));
}

void MapWidget::paintGL() {
    if (!engine_ || !inited_) {
        return;
    }
    engine_->drawFrame();
    emit zoomLevelChanged(engine_->currentZoomLevel());
}

bool MapWidget::event(QEvent *e) {
    // 窗口跨显示器拖动（缩放比例不同）时：系统 DPI 改变 → devicePixelRatioF 变化，
    // 图标/标注的像素密度须同步更新；GL 视口由随后触发的 resizeGL（已乘 dpr）负责。
    // 用 ScreenChangeInternal 而非 QWidget::screenChanged 信号（后者 Qt 5.14 才有，需兼容 5.12）。
    if (e->type() == QEvent::ScreenChangeInternal && engine_ && inited_) {
        engine_->setDisplayDensity(devicePixelRatioF());
    }
    return QOpenGLWidget::event(e);
}

// @Jin 注入数据源、缓存目录、并发与请求头，Provider 实例和下载线程均由 GlobeCore 管理。
bool MapWidget::configureTerrainProvider(const QString &baseUrl, const QString &cacheDir,
                                         int maxZoom, int concurrency, int tileRadius,
                                         const QMap<QString, QString> &headers) {
    if (!engine_) return false;
    std::vector<std::pair<std::string, std::string>> nativeHeaders;
    nativeHeaders.reserve(static_cast<size_t>(headers.size()));
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        nativeHeaders.emplace_back(it.key().toStdString(), it.value().toStdString());
    }
    return engine_->configureTerrainProvider(baseUrl.toStdString(), cacheDir.toStdString(),
                                             maxZoom, concurrency, tileRadius, nativeHeaders);
}

// @Jin 开关、刷新、预取和状态查询只做薄转发，不在 Qt 层复制 Provider 逻辑。
bool MapWidget::setTerrainProviderEnabled(bool enabled) {
    return engine_ && engine_->setTerrainProviderEnabled(enabled);
}

void MapWidget::refreshTerrainProvider() {
    if (engine_) engine_->refreshTerrainProvider();
}

void MapWidget::prefetchTerrainProviderMetadata() {
    if (engine_) engine_->prefetchTerrainProviderMetadata();
}

QString MapWidget::terrainProviderStatus() const {
    return engine_ ? QString::fromStdString(engine_->terrainProviderStatsJson()) : QStringLiteral("{}");
}

void MapWidget::zoomCentered(double factor) {
    if (!engine_ || !inited_) return;
    const QPointF fb = toFramebuffer(QPointF(width() / 2.0, height() / 2.0));
    engine_->zoomBy(factor, fb.x(), fb.y());
    update();
}

void MapWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        pressPos_ = pointOf(e).toPoint();
        lastPos_ = pressPos_;
        dragging_ = false;
        e->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(e);
}

void MapWidget::mouseMoveEvent(QMouseEvent *e) {
    const QPointF fb = toFramebuffer(pointOf(e));
    double lon = 0.0;
    double lat = 0.0;
    if (engine_ && engine_->screenToGeo(fb.x(), fb.y(), lon, lat)) {
        emit cursorGeo(lon, lat);
    }

    if (dragging_ || (e->buttons() & Qt::LeftButton)) {
        const QPoint cur = pointOf(e).toPoint();
        const QPoint d = cur - lastPos_;
        if (!dragging_ && (cur - pressPos_).manhattanLength() < 4) {
            return; // 未达拖动阈值，仍视作潜在单击
        }
        dragging_ = true;
        const qreal dpr = devicePixelRatioF();
        engine_->panByPixels(d.x() * dpr, d.y() * dpr);
        lastPos_ = cur;
        update();
        e->accept();
        return;
    }
    QOpenGLWidget::mouseMoveEvent(e);
}

void MapWidget::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        QOpenGLWidget::mouseReleaseEvent(e);
        return;
    }
    if (dragging_) {
        dragging_ = false;
        e->accept();
        return;
    }
    // 单击未拖 → 拾取：屏幕点先转 framebuffer 口径，再取地理坐标供属性面板定位
    const QPointF fb = toFramebuffer(pointOf(e));
    int layerIndex = -1;
    long long fid = -1;
    if (engine_ && engine_->pickVector(fb.x(), fb.y(), layerIndex, fid)) {
        double lon = 0.0;
        double lat = 0.0;
        engine_->screenToGeo(fb.x(), fb.y(), lon, lat);
        emit featurePicked(layerIndex, fid, lon, lat);
    }
    e->accept();
}

void MapWidget::wheelEvent(QWheelEvent *e) {
    const int steps = e->angleDelta().y() / 120;
    if (steps == 0) {
        QOpenGLWidget::wheelEvent(e);
        return;
    }
    // factor>1 放大，以光标为锚（对齐移动端 ScaleGestureDetector 的焦点缩放口径）
    const double factor = std::pow(1.15, steps);
    const QPointF fb = toFramebuffer(pointOf(e));
    engine_->zoomBy(factor, fb.x(), fb.y());
    update();
    e->accept();
}
