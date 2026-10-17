// MainWindow —— 桌面端主窗口（纯 Widgets：QMainWindow + DockWidget + 状态栏）。
// 中央嵌 MapWidget（GL 宿主），左侧图层停靠面板，底部状态栏显示光标经纬度/级别/加载态。
// 拾取命中要素后经 gcbridge::getFeatureAttributes 直读属性弹窗——桌面直连 C++，无 JNI/NAPI 编解码层。
#ifndef GLOBECORE_DESKTOP_MAINWINDOW_H
#define GLOBECORE_DESKTOP_MAINWINDOW_H

#include <QMainWindow>
#include <memory>

class MapWidget;
class LayerPanel;
class QLabel;
class QAction;
namespace globecore { class GlobeEngine; }

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    /// dataRoot 为可写数据目录（瓦片/栅格磁盘缓存根），由 main 依 QStandardPaths 解析后传入
    explicit MainWindow(QString dataRoot, QWidget *parent = nullptr);
    ~MainWindow() override;   // 内联析构需要 MapWidget 完整类型，故外联到 .cpp

    /// 中央 GL 宿主持有的渲染引擎句柄，供构造后非交互地配置图层/相机（如 XYZ 冒烟测试）。
    globecore::GlobeEngine *engine() const;

    /// 设定标注字体文件并加载字形图集；path 为空则沿用引擎默认探测
    void applyFontPath(const QString &fontPath);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onCursorGeo(double lonDeg, double latDeg);
    void onZoomLevel(int level);
    void onFeaturePicked(int layerIndex, long long fid, double lonDeg, double latDeg);
    void onZoomIn();
    void onZoomOut();
    void onAbout();

private:
    void buildMenus();
    void buildToolbar();
    void buildStatusBar();
    void saveWindowState();
    void restoreWindowState();

    std::unique_ptr<MapWidget> map_;
    LayerPanel *layerPanel_ = nullptr;
    QLabel *coordLabel_ = nullptr;
    QLabel *levelLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
};

#endif // GLOBECORE_DESKTOP_MAINWINDOW_H
