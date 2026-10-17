#include "app/LayerPanel.h"

#include <QButtonGroup>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <string>

#include "core/GlobeEngine.h"
#include "host/RasterCache.h"
#include "vector/VectorGeometry.h"

namespace {

constexpr int kRoleKind = Qt::UserRole;        ///< Kind 的 int 值
constexpr int kRoleIndex = Qt::UserRole + 1;   ///< 该 Kind 独立索引空间内的图层号
constexpr int kRoleCacheDir = Qt::UserRole + 2;
constexpr int kRolePath = Qt::UserRole + 3;     ///< 矢量/栅格层源文件绝对路径

/// 预置瓦片图源：urlTemplate 口径同移动端（{z}/{x}/{y} 占位；overlay=注记层开 alpha 混合）
struct TilePreset {
    const char *name;
    const char *urlTemplate;
    int maxLevel;
    bool overlay;
};

const TilePreset kPresets[] = {
        {"Esri 影像",
         "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/"
         "{z}/{y}/{x}",
         19, false},
        {"Carto 浅色底图",
         "https://basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png",
         20, false},
        {"Carto 注记叠加",
         "https://basemaps.cartocdn.com/light_only_labels/{z}/{x}/{y}.png",
         20, true},
};

/// 图源名 → 缓存目录叶子名（避免中文路径进 GDAL/文件 IO）
QString presetSlug(qsizetype i) {
    return QStringLiteral("preset-%1").arg(i);
}

QString baseName(const QString &path) {
    const int slash = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return slash < 0 ? path : path.mid(slash + 1);
}

} // namespace

LayerPanel::LayerPanel(globecore::GlobeEngine *engine, QString tileCacheRoot, QWidget *parent)
        : QWidget(parent), engine_(engine), tileCacheRoot_(std::move(tileCacheRoot)) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    // ── 视图模式：2D 平面墨卡托 / 3D 球体透视（引擎切换时保持相机连续）──
    auto *modeRow = new QHBoxLayout();
    modeRow->addWidget(new QLabel(tr("视图"), this));
    auto *btn2d = new QPushButton(tr("2D"), this);
    auto *btn3d = new QPushButton(tr("3D"), this);
    btn2d->setCheckable(true);
    btn3d->setCheckable(true);
    btn2d->setChecked(true);
    modeGroup_ = new QButtonGroup(this);
    modeGroup_->addButton(btn2d, 0);
    modeGroup_->addButton(btn3d, 1);
    modeGroup_->setExclusive(true);
    modeRow->addWidget(btn2d);
    modeRow->addWidget(btn3d);
    modeRow->addStretch();
    root->addLayout(modeRow);

    // 逐按钮连接：绕开 QButtonGroup::idClicked（Qt 5.15+ 才有），兼容 Qt 5.12
    connect(btn2d, &QPushButton::clicked, this, [this] {
        engine_->setViewMode(0);
        emit statusMessage(tr("2D 墨卡托"));
    });
    connect(btn3d, &QPushButton::clicked, this, [this] {
        engine_->setViewMode(1);
        emit statusMessage(tr("3D 球体"));
    });

    // ── 图层列表：checkable，显隐映射引擎各自的 set*Visible ──
    list_ = new QListWidget(this);
    connect(list_, &QListWidget::itemChanged, this, &LayerPanel::onItemChanged);
    connect(list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { refreshRemoveEnabled(); });
    root->addWidget(list_, 1);

    auto *btnRow = new QHBoxLayout();
    btnTile_ = new QPushButton(tr("加底图"), this);
    btnRaster_ = new QPushButton(tr("加栅格"), this);
    btnVector_ = new QPushButton(tr("加矢量"), this);
    btnRow->addWidget(btnTile_);
    btnRow->addWidget(btnRaster_);
    btnRow->addWidget(btnVector_);
    root->addLayout(btnRow);

    btnRemove_ = new QPushButton(tr("移除选中图层"), this);
    btnRemove_->setObjectName(QStringLiteral("dangerBtn")); // QSS：悬停泛红提示破坏性操作
    root->addWidget(btnRemove_);

    connect(btnTile_, &QPushButton::clicked, this, &LayerPanel::addTileLayer);
    connect(btnRaster_, &QPushButton::clicked, this, &LayerPanel::addRasterLayer);
    connect(btnVector_, &QPushButton::clicked, this, &LayerPanel::addVectorLayer);
    connect(btnRemove_, &QPushButton::clicked, this, &LayerPanel::removeCurrent);
    refreshRemoveEnabled();
}

QListWidgetItem *LayerPanel::appendRow(Kind kind, int index, const QString &text) {
    auto *item = new QListWidgetItem(text, list_);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
    item->setData(kRoleKind, static_cast<int>(kind));
    item->setData(kRoleIndex, index);
    return item;
}

void LayerPanel::onItemChanged(QListWidgetItem *item) {
    const Kind kind = static_cast<Kind>(item->data(kRoleKind).toInt());
    const int index = item->data(kRoleIndex).toInt();
    const bool on = item->checkState() == Qt::Checked;
    switch (kind) {
        case Kind::Tile:
            engine_->setLayerVisible(index, on);
            break;
        case Kind::RasterImage:
            engine_->setRasterImageVisible(index, on);
            break;
        case Kind::Vector:
            engine_->setVectorLayerVisible(index, on);
            break;
    }
    // 引擎变更后经 renderCallback_ 自行驱动重绘，宿主无需显式 update()
}

void LayerPanel::refreshRemoveEnabled() {
    btnRemove_->setEnabled(list_->currentItem() != nullptr);
}

void LayerPanel::addTileLayer() {
    QStringList names;
    for (const TilePreset &p : kPresets) {
        names << QString::fromUtf8(p.name);
    }
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, tr("选择瓦片图源"), tr("图源"), names, 0,
                                               false, &ok);
    if (!ok || pick.isEmpty()) {
        return;
    }
    const qsizetype i = names.indexOf(pick);
    const TilePreset &p = kPresets[i];

    // cacheDir = <数据目录>/tiles/<叶子名>：磁盘瓦片缓存，口径同移动端 /调查宝/tiles/<图源>
    const QString cacheDir = tileCacheRoot_ + QStringLiteral("/tiles/") + presetSlug(i);
    // addTileLayer 返回 void：引擎以「加入次序」为瓦片层 index（0 起，单调、永不复用槽位，
    // 因 removeTileLayer 为墓碑移除不 erase），故宿主自持计数器 tileSeq_ 记录下一 index。
    const int index = tileSeq_++;
    engine_->addTileLayer(cacheDir.toStdString(),
                          std::string(p.urlTemplate),
                          p.maxLevel,
                          p.overlay);
    QListWidgetItem *item = appendRow(Kind::Tile, index, pick);
    item->setData(kRoleCacheDir, cacheDir);
}

void LayerPanel::addRasterLayer() {
    const QString path = QFileDialog::getOpenFileName(
            this, tr("打开栅格"), QString(),
            tr("GDAL 栅格 (*.tif *.tiff *.img *.vrt *.gpkg)"));
    if (path.isEmpty()) {
        return;
    }
    // 全幅直读：GDAL 在后台线程整幅读为一张贴图，UI 线程零 GDAL、立即返回索引。
    // 传「每源一哈希子目录」作 sidecar 缓存根：二次打开命中即免 GDAL warp 秒级返回；
    // 一源一目录避免同尺寸不同源互相覆盖，目录由引擎 SidecarMkdirs 自动建（含 ASCII 化，规避中文进文件 IO）。
    const QString cacheDir = hostRasterCacheDir(tileCacheRoot_, path);
    const int index = engine_->addRasterImageLayer(cacheDir.toStdString(), path.toStdString());
    if (index < 0) {
        const std::string err = engine_->rasterAddError();
        QMessageBox::warning(this, tr("栅格加载失败"),
                             QString::fromStdString(err.empty() ? "unknown" : err));
        return;
    }
    appendRow(Kind::RasterImage, index, tr("栅格：%1").arg(baseName(path)));
}

void LayerPanel::addVectorLayer() {
    const QString path = QFileDialog::getOpenFileName(
            this, tr("打开矢量"), QString(),
            tr("矢量数据 (*.shp *.kml *.kmz *.gpkg *.geojson *.json *.gpx)"));
    if (path.isEmpty()) {
        return;
    }
    globecore::VectorStyle style;  // 全默认：hasFillColor/hasLineColor=false → 保留源文件逐要素原色
    const int index = engine_->addVectorLayer(path.toStdString(), style);
    if (index < 0) {
        QMessageBox::warning(this, tr("矢量加载失败"), baseName(path));
        return;
    }
    QListWidgetItem *item = appendRow(Kind::Vector, index, tr("矢量：%1").arg(baseName(path)));
    item->setData(kRolePath, path);
}

QString LayerPanel::vectorLayerPath(int index) const {
    for (int r = 0; r < list_->count(); ++r) {
        QListWidgetItem *item = list_->item(r);
        if (static_cast<Kind>(item->data(kRoleKind).toInt()) == Kind::Vector &&
            item->data(kRoleIndex).toInt() == index) {
            return item->data(kRolePath).toString();
        }
    }
    return QString();
}

void LayerPanel::removeCurrent() {
    QListWidgetItem *item = list_->currentItem();
    if (!item) {
        return;
    }
    const Kind kind = static_cast<Kind>(item->data(kRoleKind).toInt());
    const int index = item->data(kRoleIndex).toInt();

    if (kind == Kind::RasterImage) {
        // 引擎只提供 setRasterImageVisible，无全幅栅格移除接口：
        // 如实降级为「隐藏 + 保留索引」，不做假删除（索引空间会因移除而错位，属引擎能力边界）
        engine_->setRasterImageVisible(index, false);
        item->setCheckState(Qt::Unchecked);
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        item->setText(tr("栅格（引擎暂无移除接口，仅隐藏）#%1").arg(index));
        emit statusMessage(tr("全幅栅格层已隐藏；引擎未提供该类型移除接口"));
        return;
    }

    // 引擎侧为墓碑移除（置 dead+hidden，不 erase），其它图层索引保持稳定，故本地行可直接删
    if (kind == Kind::Tile) {
        engine_->removeTileLayer(index);
    } else {
        engine_->removeVectorLayer(index);
    }
    // Qt5 的 takeItem(QListWidgetItem*) 在 Qt6 改为按行 takeItem(int)；这里持有 item 指针，
    // 直接 delete item 即从列表移除并释放（QListWidget 会自动摘除被删项）。
    delete item;
    refreshRemoveEnabled();
}
