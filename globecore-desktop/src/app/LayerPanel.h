// LayerPanel —— 图层管理停靠面板（纯 Widgets，无 QML）。
// 三类图层各占一个独立索引空间（引擎口径）：瓦片层 addTileLayer / 全幅栅格 addRasterImageLayer /
// 矢量层 addVectorLayer，故列表项以 (Kind, index) 记录映射，显隐各自调对应 set*Visible。
#ifndef GLOBECORE_DESKTOP_LAYERPANEL_H
#define GLOBECORE_DESKTOP_LAYERPANEL_H

#include <QWidget>

class QListWidget;
class QPushButton;
class QButtonGroup;
class QListWidgetItem;

namespace globecore { class GlobeEngine; }

class LayerPanel final : public QWidget {
    Q_OBJECT
public:
    explicit LayerPanel(globecore::GlobeEngine *engine, QString tileCacheRoot,
                        QWidget *parent = nullptr);

    /// 矢量图层索引 → 源文件路径。引擎拾取只回 (layerIndex, fid)，属性查询需按路径打开数据源，
    /// 故本地保留映射；非矢量层索引或已移除返回空串。
    QString vectorLayerPath(int index) const;

public slots:
    // 公开为槽：主窗口菜单与面板按钮共用同一入口
    void addTileLayer();
    void addRasterLayer();
    void addVectorLayer();
    void removeCurrent();

signals:
    void statusMessage(const QString &text);

private slots:
    void onItemChanged(QListWidgetItem *item);

private:
    enum class Kind { Tile, RasterImage, Vector };

    QListWidgetItem *appendRow(Kind kind, int index, const QString &text);
    void refreshRemoveEnabled();

    globecore::GlobeEngine *engine_;
    QString tileCacheRoot_;

    // 引擎 addTileLayer 返回 void，瓦片层 index＝加入次序（0 起）；移除为墓碑(不回收槽位)，
    // 故 index 单调递增，用本地计数器复现引擎口径（Raster/Vector 有返回 index，不需此计数器）。
    int tileSeq_ = 0;

    QListWidget *list_;
    QPushButton *btnTile_;
    QPushButton *btnRaster_;
    QPushButton *btnVector_;
    QPushButton *btnRemove_;
    QButtonGroup *modeGroup_;   ///< 2D / 3D
};

#endif // GLOBECORE_DESKTOP_LAYERPANEL_H
