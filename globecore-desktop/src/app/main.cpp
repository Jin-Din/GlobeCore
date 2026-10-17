#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QSurfaceFormat>

#include <string>
#include <vector>

#include "app/MainWindow.h"
#include "bridge/bridge_api.h"
#include "core/GlobeEngine.h"
#include "host/RasterCache.h"
#include "net/HttpClient.h"

namespace {

/// 桌面端 CJK 字体候选（引擎 FontAtlas 在移动端自探 /system/fonts，桌面须宿主给一个含中文字形的字体文件）。
/// Windows 直接命中系统雅黑/黑体；Linux/信创按「随包 fonts → 用户 XDG 字体目录 → 系统字体目录」
/// 顺序递归扫常见中文字体文件名关键字。不依赖 fc-match（其 `:lang=zh` 在本机竟回落 DejaVu 无中文），
/// 命中即用，跨发行版且覆盖 apt 安装与 ~/.local/share/fonts 手动放置两种情形。
QString detectCjkFont() {
#ifdef Q_OS_WIN
    const std::vector<std::string> winCands = {
            "C:/Windows/Fonts/msyh.ttc",      // 微软雅黑
            "C:/Windows/Fonts/msyh.ttf",
            "C:/Windows/Fonts/simhei.ttf",    // 黑体
    };
    for (const std::string &c : winCands) {
        if (QFileInfo::exists(QString::fromStdString(c))) {
            return QString::fromStdString(c);
        }
    }
#endif
    static const char *const kKeys[] = {
            "msyh", "yahei", "simhei", "simsun", "notosanscjk", "notosanssc",
            "sourcehansans", "wqy", "microhei", "uming", "ukai", "cjk", "pingfang",
    };
    auto keyHit = [](const QString &nameLower) {
        for (const char *k : kKeys) {
            if (nameLower.contains(QLatin1String(k))) {
                return true;
            }
        }
        return false;
    };
    QStringList dirs;
    dirs << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fonts"));
#ifndef Q_OS_WIN
    dirs << QDir::homePath() + QStringLiteral("/.local/share/fonts")
         << QStringLiteral("/usr/share/fonts")
         << QStringLiteral("/usr/local/share/fonts");
#else
    dirs << QStringLiteral("C:/Windows/Fonts");
#endif
    for (const QString &d : dirs) {
        if (!QFileInfo::exists(d)) {
            continue;
        }
        QDirIterator it(d, {QStringLiteral("*.ttc"), QStringLiteral("*.ttf"),
                            QStringLiteral("*.otf")},
                         QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            if (keyHit(QFileInfo(p).fileName().toLower())) {
                return p;
            }
        }
    }
    return QString();
}

/// 桌面端 HTTPS 根证书包（CA bundle）探测，供 HttpClient::setCaBundle 用。
/// 引擎 HttpClient 在未设置 CA 时会回落到 Android 系统证书目录（/system/etc/...），
/// 该路径桌面不存在：Windows 走 Schannel 后端会忽略 CAPATH 改用系统证书库（正常校验），
/// 但 Linux 若 curl 为 OpenSSL 后端则会命中假路径 → 校验失败 → 引擎静默降级为不校验（安全隐患）。
/// 故桌面须显式给一个真实存在的 bundle：优先 exe 旁随包 cacert.pem，其次各主流发行版系统 bundle。
/// 找不到时返回空串——Windows 保持不设（沿用系统库），Linux 则维持引擎既有行为（不劣化）。
QString detectCaBundle() {
    const QString besideExe = QDir(QCoreApplication::applicationDirPath())
                                      .filePath(QStringLiteral("cacert.pem"));
    const std::vector<std::string> cands = {
            besideExe.toStdString(),                    // 随包内置（跨平台，deploy 脚本可拷入）
            "/etc/ssl/certs/ca-certificates.crt",       // Debian/Ubuntu/麒麟/Kylin
            "/etc/pki/tls/certs/ca-bundle.crt",         // RHEL/Fedora/统信 UOS/openEuler
            "/etc/ssl/ca-bundle.pem",                   // openSUSE
    };
    for (const std::string &c : cands) {
        if (QFileInfo::exists(QString::fromStdString(c))) {
            return QString::fromStdString(c);
        }
    }
    return QString();
}

/// 桌面控件（菜单/按钮/状态栏）CJK 字体兜底：Windows 系统默认字体即含中文，无需干预；
/// Linux/信创若 Qt 默认族为拉丁字体，控件中文会成豆腐块，这里显式选一套系统已装的中文字族。
/// 候选按跨发行版常见度排序，命中首个即用 QApplication::setFont 全局生效；都缺则维持原状。
void applyWidgetCjkFont() {
#ifdef Q_OS_WIN
    return;
#else
    static const char *const cands[] = {
            "Noto Sans CJK SC", "Source Han Sans SC", "WenQuanYi Micro Hei",
            "Microsoft YaHei", "SimHei",
    };
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QStringList fams = QFontDatabase::families();
#else
    QFontDatabase db;
    const QStringList fams = db.families();
#endif
    for (const char *c : cands) {
        const QString fam = QString::fromLatin1(c);
        if (fams.contains(fam, Qt::CaseInsensitive)) {
            QApplication::setFont(QFont(fam));
            return;
        }
    }
#endif
}

/// 全局现代扁平 QSS（浅色，类 VS/Office 风格）：统一菜单/工具栏/停靠面板/按钮/列表/
/// 滚动条/对话框（QMessageBox/QInputDialog/QFileDialog 同源继承）观感。
/// 尺寸全部使用逻辑像素：高 DPI 缩放开启后由 Qt 按系统比例自动放大，无需为分辨率写死字号。
void applyAppStyle() {
    const QString qss = QStringLiteral(R"(
QWidget {
    color: #1F2329;
}
QMainWindow, QDialog {
    background: #F0F2F5;
}
QToolTip {
    background: #1F2329;
    color: #FFFFFF;
    border: none;
    padding: 4px 8px;
    border-radius: 4px;
}

/* ── 菜单栏 / 菜单 ── */
QMenuBar {
    background: #F5F6F8;
    border-bottom: 1px solid #E0E2E6;
    padding: 2px;
}
QMenuBar::item {
    background: transparent;
    padding: 6px 12px;
    border-radius: 4px;
}
QMenuBar::item:selected {
    background: #E3EDFB;
}
QMenu {
    background: #FFFFFF;
    border: 1px solid #D8DBE0;
    border-radius: 6px;
    padding: 4px;
}
QMenu::item {
    padding: 6px 28px 6px 16px;
    border-radius: 4px;
}
QMenu::item:selected {
    background: #E3EDFB;
}
QMenu::separator {
    height: 1px;
    background: #E8EAEE;
    margin: 4px 8px;
}

/* ── 工具栏 ── */
QToolBar {
    background: #F5F6F8;
    border: none;
    border-bottom: 1px solid #E0E2E6;
    spacing: 4px;
    padding: 3px 6px;
}
QToolBar QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 5px 12px;
    color: #1F2329;
}
QToolBar QToolButton:hover {
    background: #E3EDFB;
    border-color: #BBD3F2;
}
QToolBar QToolButton:pressed {
    background: #CFE2F9;
}
QToolBar::separator {
    width: 1px;
    background: #D8DBE0;
    margin: 4px 6px;
}

/* ── 常规按钮（含图层面板 2D/3D checkable 选中态） ── */
QPushButton {
    background: #FFFFFF;
    border: 1px solid #C9CDD4;
    border-radius: 5px;
    padding: 6px 14px;
    min-height: 20px;
}
QPushButton:hover {
    border-color: #2D7DD2;
    color: #1E5FA8;
    background: #F7FAFE;
}
QPushButton:pressed {
    background: #E3EDFB;
}
QPushButton:checked {
    background: #2D7DD2;
    border-color: #2D7DD2;
    color: #FFFFFF;
}
QPushButton:checked:hover {
    background: #1E6BB8;
    border-color: #1E6BB8;
}
QPushButton:disabled {
    color: #A8ABB2;
    background: #F5F6F8;
    border-color: #E0E2E6;
}
/* 危险操作按钮（移除图层）：仅悬停泛红 */
QPushButton#dangerBtn:hover {
    border-color: #D54941;
    color: #C2392F;
    background: #FDF6F5;
}

/* ── 图层列表 ── */
QListWidget {
    background: #FFFFFF;
    border: 1px solid #D8DBE0;
    border-radius: 6px;
    padding: 2px;
    outline: 0;
}
QListWidget::item {
    padding: 6px 8px;
    border-radius: 4px;
}
QListWidget::item:hover {
    background: #F0F5FC;
}
QListWidget::item:selected {
    background: #CFE2F9;
    color: #143D6E;
}

/* ── 停靠面板 ── */
QDockWidget {
    titlebar-close-icon: none;
    color: #1F2329;
}
QDockWidget::title {
    background: #F5F6F8;
    padding: 8px 10px;
    border-bottom: 1px solid #E0E2E6;
    font-weight: 600;
}

/* ── 状态栏 ── */
QStatusBar {
    background: #F5F6F8;
    border-top: 1px solid #E0E2E6;
}
QStatusBar::item {
    border: none;
}
QStatusBar QLabel {
    padding: 2px 8px;
}

/* ── 输入控件（QInputDialog / 未来设置面板同源） ── */
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
    background: #FFFFFF;
    border: 1px solid #C9CDD4;
    border-radius: 4px;
    padding: 4px 6px;
    selection-background-color: #CFE2F9;
    selection-color: #143D6E;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border-color: #2D7DD2;
}
QComboBox::drop-down {
    border: none;
    width: 20px;
}
QComboBox QAbstractItemView {
    background: #FFFFFF;
    border: 1px solid #D8DBE0;
    selection-background-color: #CFE2F9;
    selection-color: #143D6E;
    outline: 0;
}
QGroupBox {
    border: 1px solid #D8DBE0;
    border-radius: 6px;
    margin-top: 10px;
    padding: 8px;
    font-weight: 600;
}

/* ── 细圆角滚动条 ── */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 2px;
}
QScrollBar::handle:vertical {
    background: #C9CDD4;
    border-radius: 4px;
    min-height: 28px;
}
QScrollBar::handle:vertical:hover {
    background: #A8ABB2;
}
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 2px;
}
QScrollBar::handle:horizontal {
    background: #C9CDD4;
    border-radius: 4px;
    min-width: 28px;
}
QScrollBar::handle:horizontal:hover {
    background: #A8ABB2;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}
)");
    qApp->setStyleSheet(qss);
}

} // namespace

int main(int argc, char **argv) {
    // 高 DPI 适配：必须在 QApplication 构造前设置。Qt5 默认不开启（控件恒按 96dpi 物理像素
    // 布局），在系统缩放 125%/150%/200% 的高分屏上菜单/按钮/字体会显小且模糊；开启后 Qt 改按
    // 逻辑像素布局并随系统缩放自动放大，QSS 中的 px/padding 亦按逻辑像素等比缩放。
    // QOpenGLWidget 的物理像素换算 MapWidget 已用 devicePixelRatioF() 全面处理（视口/平移/拾取）。
    // Qt6 起这两项为默认行为，属性已移除，故仅 Qt5 显式打开。
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#endif

    // 桌面 gles2 后端下，让 Qt 经 ANGLE(Windows)/Mesa(Linux) 提供 OpenGL ES 2.0 上下文，
    // 与引擎内嵌的 GLSL ES 1.00 着色器原生匹配；否则桌面默认可能给到纯桌面 GL profile
    QSurfaceFormat fmt;
#ifdef GLOBECORE_DESKTOP_GLEW
    // glew 后端（仅 Linux 有意义）：请求桌面 GL 2.1，着色器兼容性靠 ARB_ES2_compatibility
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setMajorVersion(2);
    fmt.setMinorVersion(1);
#else
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setMajorVersion(2);
    fmt.setMinorVersion(0);
#endif
    fmt.setDepthBufferSize(16);   // 3D 球面需深度缓冲；2D 不消费
    QSurfaceFormat::setDefaultFormat(fmt);

#if defined(Q_OS_WIN) && !defined(GLOBECORE_DESKTOP_GLEW)
    // 强制 Qt 走 ANGLE（其随 Qt 部署的 libEGL/libGLESv2），得到真正的 GLES2 上下文
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("GlobeCoreDesktop"));
    QApplication::setOrganizationName(QStringLiteral("ZYS"));
    applyWidgetCjkFont();   // Linux 缺 CJK 字体时控件中文会成豆腐块；命中已装中文字族则显式指定
    applyAppStyle();        // 全局现代扁平 QSS（尺寸为逻辑像素，随系统 DPI 缩放自动等比放大）

    // PROJ 检索 proj.db：CMake POST_BUILD 已把引擎 assets/proj 拷到产物旁，作为默认数据目录
    const QString projDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("proj"));
    if (QFileInfo::exists(projDir + QStringLiteral("/proj.db"))) {
        gcbridge::initProjDataPath(projDir.toStdString());
    }

    // 桌面 HTTPS 根证书：给出真实存在的 CA bundle，规避引擎回落到 Android 系统证书目录
    // （桌面不存在该路径）。Windows 命中不到→不设置，沿用 Schannel 系统证书库；Linux 命中
    // 系统 bundle 或 exe 旁随包 cacert.pem→设置之，杜绝 OpenSSL 后端下的不校验降级。
    const QString caBundle = detectCaBundle();
    if (!caBundle.isEmpty()) {
        globecore::HttpClient::setCaBundle(caBundle.toStdString());
    }

    // 可写数据根（瓦片/栅格磁盘缓存落此）
    QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataRoot);

    MainWindow window(dataRoot);
    const QString font = detectCjkFont();
    if (!font.isEmpty()) {
        window.applyFontPath(font);   // 空则引擎回退默认探测（桌面探测为空即不渲染标注）
    }
    window.show();

    // XYZ 冒烟测试钩子（仅当设 GLOBECORE_SMOKE_BASEMAP=1 时启用，默认关闭不影响正常启动）：
    // 非交互地给引擎挂一张在线 XYZ 底图，验证 HttpClient(curl) 取瓦片 → ImageDecoder 解码 →
    // 磁盘缓存落盘 → ANGLE 纹理上传 → 着色绘制 这条桌面端到端通路。
    if (qEnvironmentVariableIsSet("GLOBECORE_SMOKE_BASEMAP")) {
        const QString cacheDir = QDir(dataRoot).filePath(QStringLiteral("tiles/smoke"));
        window.engine()->addTileLayer(
                cacheDir.toStdString(),
                "https://basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png",
                20, false);
        // 3D 冒烟：GLOBECORE_SMOKE_3D=1 时切球面模式，验证瓦片贴球 + 深度测试这条 P1 通路
        if (qEnvironmentVariableIsSet("GLOBECORE_SMOKE_3D")) {
            window.engine()->setViewMode(1);
        }
    }

    // 矢量冒烟钩子（设 GLOBECORE_SMOKE_VECTOR=<文件路径> 时启用）：非交互地经 GDAL/OGR 直读一份
    // 矢量数据，验证 GdalBootstrap→VectorReader(ogr)→proj.db 坐标变换→VectorBuilder→VBO 上传
    // 这条桌面 GDAL 通路（桌面最高集成风险处）。同时把相机移到样例区，确保要素真正落屏绘制。
    const QString smokeVec = qEnvironmentVariable("GLOBECORE_SMOKE_VECTOR");
    if (!smokeVec.isEmpty() && QFileInfo::exists(smokeVec)) {
        globecore::VectorStyle style;
        window.engine()->addVectorLayer(smokeVec.toStdString(), style);
        globecore::Camera cam;
        cam.longitude = 116.42;
        cam.latitude = 39.89;
        cam.altitude = 40000.0;
        window.engine()->setCamera(cam);
    }

    // 栅格冒烟钩子（设 GLOBECORE_SMOKE_RASTER=<栅格路径> 时启用）：非交互地经 GDAL 整幅直读一份
    // 栅格（tif/img 等），验证 GdalBootstrap→addRasterImageLayer→GDALRasterIO 读带→纹理上传→
    // 贴地绘制 这条桌面栅格通路。它与矢量共用 GDAL，但走 RasterIO 分支，独立覆盖栅格子系统。
    const QString smokeRas = qEnvironmentVariable("GLOBECORE_SMOKE_RASTER");
    if (!smokeRas.isEmpty() && QFileInfo::exists(smokeRas)) {
        // 与 LayerPanel 同口径：传每源哈希子目录，二次冒烟即验证 sidecar 命中免 warp
        window.engine()->addRasterImageLayer(
                hostRasterCacheDir(dataRoot, smokeRas).toStdString(), smokeRas.toStdString());
        globecore::Camera rcam;
        rcam.longitude = 116.3;
        rcam.latitude = 39.9;
        rcam.altitude = 60000.0;
        window.engine()->setCamera(rcam);
    }

    return QApplication::exec();
}
