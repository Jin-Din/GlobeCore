// MapWidget —— 桌面端 GL 宿主，职责等价于 Android 的 NativeMapView(GLSurfaceView) 与鸿蒙的
// EglContext+XComponentBridge：创建/绑定 GL 上下文、把 GL 生命周期与鼠标事件转发给 GlobeEngine。
// 引擎本身不建上下文、不管窗口（见 render/Renderer.h 注释），故本类是桌面端唯一需新写的渲染胶水。
#ifndef GLOBECORE_DESKTOP_MAPWIDGET_H
#define GLOBECORE_DESKTOP_MAPWIDGET_H

#include <QMap>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <memory>

#include "core/GlobeEngine.h"

class MapWidget final : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit MapWidget(QWidget *parent = nullptr);
    ~MapWidget() override;

    /// 引擎句柄，供主窗口配置图层/字体/相机。生命周期与本控件一致，构造后即非空。
    globecore::GlobeEngine *engine() const { return engine_.get(); }

    /// 以控件中心为锚缩放（供工具栏按钮调用，与滚轮缩放同口径）
    void zoomCentered(double factor);

    // @Jin 地形 Provider 门面与 Android / Harmony 保持同名口径，数据源策略由宿主注入。
    bool configureTerrainProvider(const QString &baseUrl, const QString &cacheDir,
                                  int maxZoom = 15, int concurrency = 4, int tileRadius = 3,
                                  const QMap<QString, QString> &headers = QMap<QString, QString>());
    // @Jin 开关、刷新、元数据预取与状态查询均直接转发到共享 C++ Provider。
    bool setTerrainProviderEnabled(bool enabled);
    void refreshTerrainProvider();
    void prefetchTerrainProviderMetadata();
    QString terrainProviderStatus() const;

signals:
    /// 左键未拖动即抬起：命中矢量要素则发出（索引为 addVectorLayer 次序），未命中不发
    void featurePicked(int layerIndex, long long fid, double lonDeg, double latDeg);
    /// 鼠标移动时的光标经纬度，供状态栏坐标显示
    void cursorGeo(double lonDeg, double latDeg);
    /// 一帧绘制后的相机显示级别，供状态栏级别显示
    void zoomLevelChanged(int level);
    // @Jin 地形 Provider 统一事件（单行 JSON）；已由 GUI 线程投递后发出。
    void terrainProviderEvent(const QString &payload);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    bool event(QEvent *e) override;   // 捕获跨屏移动，按新 devicePixelRatio 更新引擎密度

    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    /// 事件坐标 → 引擎 framebuffer 像素口径（与 surfaceChanged 传入的宽高同单位）
    QPointF toFramebuffer(const QPointF &pos) const;

    std::unique_ptr<globecore::GlobeEngine> engine_;
    QPoint pressPos_;       ///< 左键按下位置，用于区分「拖动平移」与「单击拾取」
    QPoint lastPos_;
    bool dragging_ = false;
    bool inited_ = false;
};

#endif // GLOBECORE_DESKTOP_MAPWIDGET_H
