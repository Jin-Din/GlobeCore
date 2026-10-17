#pragma once
// 全幅栅格 sidecar 位图缓存目录：按「源绝对路径」哈希出每源一个 ASCII 子目录。
// 引擎 renderFullImage 在 sidecarDir 非空时：命中则免 GDAL 整幅 warp 秒级返回，未命中则 warp 后
// 回写 full_<WxH>.rgba(.meta)（写盘量被双帽封顶、并以源 mtime+size 校验、拒绝脏命中）。
// 无金字塔的超大投影图首开可 >4min，命中后降到读盘级。sidecar 文件名不含源标识，故必须一源一目录，
// 否则同尺寸不同源会互相覆盖（虽经校验不会错图，但缓存抖动）。目录由引擎 SidecarMkdirs 自动创建。
// ASCII 目录名（md5 十六进制）规避中文进 GDAL/文件 IO，与瓦片 presetSlug 口径一致。
#include <QCryptographicHash>
#include <QDir>
#include <QString>

inline QString hostRasterCacheDir(const QString &dataRoot, const QString &srcPath) {
    const QByteArray hex =
            QCryptographicHash::hash(srcPath.toUtf8(), QCryptographicHash::Md5).toHex().left(16);
    return QDir(dataRoot).filePath(QStringLiteral("raster/%1").arg(QString::fromLatin1(hex)));
}
