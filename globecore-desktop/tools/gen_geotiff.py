#!/usr/bin/env python3
# 纯 stdlib 生成最小有效 GeoTIFF（RGBA、无压缩、单 strip）。
# 用途：给桌面栅格冒烟测试 GLOBECORE_SMOKE_RASTER 造带真实地理范围的 tif，
# 走 GDAL RasterIO 分支验证；免装 gdal-bin、免 sudo、免网络。
# 两种模式：
#   默认(地理)：EPSG:4326，北京一带，左上(116.0,40.2) 右下(116.6,39.6)，32x32。
#   --projected：EPSG:32650(WGS84/UTM 50N) 米制投影 + 非地理 GTModelType，
#     令引擎命中「投影栅格 → 现场 VRT 重投影」路径（与 1.3GB 帽帽山.tif 同一条码路，但尺寸小可秒读完成）。
# 参数：<out.tif> [--size WxH] [--projected]
import struct, sys

argv = sys.argv[1:]
size = (32, 32)
projected = False
out_path = 'sample.tif'
positional = []
i = 0
while i < len(argv):
    a = argv[i]
    if a == '--projected':
        projected = True
    elif a == '--size':
        i += 1
        w, h = argv[i].lower().split('x')
        size = (int(w), int(h))
    else:
        positional.append(a)
    i += 1
if positional:
    out_path = positional[0]
W, H = size

if projected:
    # UTM 50N(117°CM) 北京附近一块 10m/px 影像：左上(400000,4470000) 米制
    ULX, ULY = 400000.0, 4470000.0
    SX = SY = 10.0                       # 每像素 10m，extent = W*10 × H*10 米
    LRX, LRY = ULX + W * SX, ULY - H * SY
else:
    ULX, ULY, LRX, LRY = 116.0, 40.2, 116.6, 39.6
    SX = (LRX - ULX) / W                 # 每像素经度跨度
    SY = (ULY - LRY) / H                 # 每像素纬度跨度（取正，GDAL 依 PixelIsArea 约定内部取负）

TYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 12: 8}  # BYTE/ASCII/SHORT/LONG/RATIONAL/DOUBLE

def field(tag, typ, count, payload):
    # payload: bytes，其长度须等于 count*TYPE_SIZE[typ]
    sz = count * TYPE_SIZE[typ]
    return (tag, typ, count, sz, payload)

# 外部 blob（>4 字节的数组）
bps  = struct.pack('<4H', 8, 8, 8, 8)                      # BitsPerSample 4×SHORT
mps  = struct.pack('<3d', SX, SY, 0.0)                     # ModelPixelScale 3×DOUBLE
mtp  = struct.pack('<6d', 0, 0, 0, ULX, ULY, 0.0)          # ModelTiepoint 6×DOUBLE
if projected:
    # GTModelType=1(投影), RasterType=1(PixelIsArea), ProjectedCSType=32650(UTM50N/WGS84)
    gkd  = [1, 1, 0, 3,
            1024, 0, 1, 1,      # GTModelTypeGeoKey = 1 (projected)
            1025, 0, 1, 1,      # GTRasterTypeGeoKey = 1 (PixelIsArea)
            3072, 0, 1, 32650]  # ProjectedCSTypeGeoKey = EPSG:32650
else:
    gkd  = [1, 1, 0, 3,
            1024, 0, 1, 2,      # GTModelTypeGeoKey = 2 (geographic)
            1025, 0, 1, 1,      # GTRasterTypeGeoKey = 1 (PixelIsArea)
            2048, 0, 1, 4326]   # GeographicTypeGeoKey = EPSG:4326
gkd  = struct.pack('<%dH' % len(gkd), *gkd)                # GeoKeyDirectory

# 像素数据：RGBA 渐变+棋盘。按行生成一次再整块平铺（纯字节乘法，C 层拷贝），
# 否则大尺寸（如 7000x7000=49M 像素）逐像素 Python 循环要数分钟。
def make_row(parity):
    row = bytearray(W * 4)
    for x in range(W):
        o = x * 4
        row[o]     = (x * 3) & 0xFF                       # R：水平渐变
        row[o + 1] = (x * 5) & 0xFF                       # G
        row[o + 2] = (x * 7) & 0xFF                       # B
        row[o + 3] = 255 if ((x // 64 + parity) % 2 == 0) else 170  # A：棋盘
    return bytes(row)
row_even = make_row(0)
row_odd  = make_row(1)
px = bytearray((row_even + row_odd) * (H // 2) + row_even * (H % 2))

entries = [
    field(256, 4, 1, struct.pack('<I', W)),   # ImageWidth
    field(257, 4, 1, struct.pack('<I', H)),   # ImageLength
    field(258, 3, 4, bps),                     # BitsPerSample (out)
    field(259, 3, 1, struct.pack('<H', 1)),    # Compression = none
    field(262, 3, 1, struct.pack('<H', 2)),    # Photometric = RGB
    field(273, 4, 1, b''),                      # StripOffsets (回填)
    field(277, 3, 1, struct.pack('<H', 4)),    # SamplesPerPixel = 4
    field(278, 3, 1, struct.pack('<H', H)),    # RowsPerStrip = H
    field(279, 4, 1, struct.pack('<I', len(px))),  # StripByteCounts
    field(338, 3, 1, struct.pack('<H', 1)),    # ExtraSamples = 1 (unassociated alpha)
    field(33550, 12, 3, mps),                   # ModelPixelScale (out)
    field(33922, 12, 6, mtp),                   # ModelTiepoint (out)
    field(34016, 3, len(gkd) // 2, gkd),        # GeoKeyDirectory (out)
]

n = len(entries)
ifd_off = 8
entries_start = ifd_off + 2
data_start = entries_start + n * 12 + 4        # + nextIFD(4)

# 分配外部数据偏移（8 字节对齐，DOUBLE 友好）
offset = data_start
alloc = {}
for k, e in enumerate(entries):
    tag, typ, count, sz, payload = e
    if sz > 4:
        offset = (offset + 7) & ~7
        alloc[k] = offset
        offset += sz

strip_off = (offset + 7) & ~7

# 组装 IFD
ifd = struct.pack('<H', n)
for k, (tag, typ, count, sz, payload) in enumerate(entries):
    if sz > 4:
        val = struct.pack('<I', alloc[k])
    elif sz == 4:
        val = payload
    else:
        val = payload + b'\x00' * (4 - sz)
    if tag == 273:
        val = struct.pack('<I', strip_off)     # StripOffsets 回填
    ifd += struct.pack('<HHI', tag, typ, count) + val
ifd += struct.pack('<I', 0)                     # nextIFD = 0

# 头部
head = b'II' + struct.pack('<HI', 42, ifd_off)

out = bytearray()
out += head
out += ifd
assert len(out) == data_start, (len(out), data_start)
# 外部数组
def put(at, blob):
    while len(out) < at:
        out.append(0)
    out[at:at + len(blob)] = blob
for k, (tag, typ, count, sz, payload) in enumerate(entries):
    if sz > 4:
        put(alloc[k], payload)
put(strip_off, bytes(px))

open(out_path, 'wb').write(bytes(out))
mode = 'projected/EPSG:32650' if projected else 'geographic/EPSG:4326'
print('wrote %s %d bytes %dx%d %s; ul=(%.2f,%.2f) lr=(%.2f,%.2f)'
      % (out_path, len(out), W, H, mode, ULX, ULY, LRX, LRY))
