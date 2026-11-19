package com.zys.globecore.android

import android.os.Bundle
import com.zys.globecore.Camera
import com.zys.globecore.FeatureGeometry
import com.zys.globecore.NativeMapView
import com.zys.globecore.VectorStyle

/**
 * 演示 03 · 矢量图层与拾取（对应 tutorials/03「矢量图层与拾取」）：
 *  - @Jin [NativeMapView.addGeoJSONLayer]：专用 GeoJSON 入口，url 传在线省级地址（native 下载缓存后复用读取管线）；
 *  - @Jin 同接口 data 传 GeoJSON 字符串：native 校验后落 inline 缓存再建层，无需宿主写文件；
 *  - @Jin [VectorStyle] 承载面填充/描边/标注样式，异步读取 + 三角剖分，就绪自动重绘；
 *  - @Jin [NativeMapView.pickVector]：屏幕点命中 → [layerIndex, fid]；
 *  - @Jin [NativeMapView.featureGeometry] + 叠加层：选中要素高亮（noPick 退出拾取竞争）。
 */
class VectorLayerActivity : BaseMapActivity() {

    override val demoTitle get() = getString(R.string.demo_vector_title)

    private var vectorIdx = -1
    private var inlineIdx = -1
    private var highlightIdx = -1

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // @Jin 省级 GeoJSON 改为直接测试在线 Source；首次下载，后续由 native vector-cache 命中本地缓存。
        // @Jin 在线高德影像底图；本演示默认 2D 平面，初始给全国视角便于观察省级边界。
        map.setViewMode(NativeMapView.ViewMode.TWO_D)
        addBasemap(Camera(latitude = 35.2, longitude = 105.5, altitude = 9_000_000.0))

        addDemoAction(getString(R.string.action_add_vector)) { addVector() }
        // @Jin 新增内联 GeoJSON 字符串入口：验证 addGeoJSONLayer 的 data 通路（落 inline 缓存、同内容去重）。
        addDemoAction("内联GeoJSON") { addInlineGeoJson() }
        addDemoAction("放大") { zoomIn() }
        addDemoAction("缩小") { zoomOut() }
    }

    private fun addVector() {
        if (vectorIdx >= 0) {
            toast("矢量层已加载（index=$vectorIdx）")
            return
        }
        // @Jin URL 数据源走 addGeoJSONLayer 专用入口（data 传空串）；旧 addVectorLayer 的 URL 用法已弃用。
        // @Jin labelField="name"：逐要素取 name 属性作文字标注（native 自动探测系统 CJK 字体）
        vectorIdx = map.addGeoJSONLayer(
            url = PROVINCE_GEOJSON_URL,
            data = "",
            VectorStyle(
                fillColor = 0x664A8FE3.toInt(),
                outlineColor = 0xFFFFFFFF.toInt(),
                labelField = "name",
                labelOutline = true,
            ),
        )
        map.setCamera(Camera(latitude = 35.2, longitude = 105.5, altitude = 9_000_000.0))
        toast("省级在线 GeoJSON 已加入 index=$vectorIdx（首次下载，就绪自动显示）")

        // 单击拾取：命中要素画高亮叠加层（选中态口径），高亮层不参与拾取竞争
        map.setOnTapListener { x, y ->
            val hit = map.pickVector(x, y)
            if (hit == null) {
                clearHighlight()
            } else {
                val layer = hit[0].toInt()
                val fid = hit[1]
                val geo = map.featureGeometry(layer, fid)
                if (geo == null) {
                    toast("命中 layer=$layer fid=$fid（无 CPU 几何）")
                } else {
                    toast("命中 layer=$layer fid=$fid 类型=${geo.typeDesc()}")
                    highlight(geo, fid)
                }
            }
        }
    }

    // @Jin data 通路：宿主直接给 FeatureCollection 字符串，native 在 filesDir/globecore/vector-cache/inline 落盘后建层。
    private fun addInlineGeoJson() {
        if (inlineIdx >= 0) {
            toast("内联 GeoJSON 层已加载（index=$inlineIdx）")
            return
        }
        val geoJsonText = """
            {"type":"FeatureCollection","features":[
              {"type":"Feature","properties":{"name":"内联点"},"geometry":{"type":"Point","coordinates":[116.40,39.90]}},
              {"type":"Feature","properties":{"name":"内联三角面"},"geometry":{"type":"Polygon","coordinates":[[
                [115.90,39.40],[116.90,39.40],[116.40,40.10],[115.90,39.40]]]}}
            ]}
        """.trimIndent()
        inlineIdx = map.addGeoJSONLayer(
            url = "",
            data = geoJsonText,
            VectorStyle(
                fillColor = 0x66FF8800.toInt(),
                outlineColor = 0xFFFF8800.toInt(),
                lineColor = 0xFFFF8800.toInt(),
                pointColor = 0xFFFF8800.toInt(),
                labelField = "name",
                labelOutline = true,
            ),
        )
        map.setCamera(Camera(latitude = 39.75, longitude = 116.40, altitude = 700_000.0))
        toast("内联 GeoJSON 已加入 index=$inlineIdx（data 通路）")
    }

    private fun FeatureGeometry.typeDesc() = when (type) {
        0 -> "点"
        1 -> "线"
        else -> "面"
    }

    /** 选中高亮：金黄样式叠加层，几何直接从 featureGeometry 摊平数组透传（同构无需换序） */
    private fun highlight(geo: FeatureGeometry, fid: Long) {
        if (highlightIdx < 0) {
            highlightIdx = map.addOverlayLayer(
                VectorStyle(
                    fillColor = 0x66FFD700.toInt(),
                    outlineColor = 0xFFFFD700.toInt(),
                    outlineWidth = 3f,
                    lineColor = 0xFFFFD700.toInt(),
                    lineWidth = 4f,
                    pointColor = 0xFFFFD700.toInt(),
                )
            )
            map.setOverlayNoPick(highlightIdx, true)  // 瞬态高亮层不参与拾取
        }
        when (geo.type) {
            0 -> map.updateOverlayPoints(highlightIdx, geo.lonlat, LongArray(geo.lonlat.size / 2) { fid })
            1 -> map.updateOverlayLines(highlightIdx, geo.lonlat, geo.ringCounts, LongArray(geo.ringCounts.size) { fid })
            else -> map.updateOverlayPolygons(
                highlightIdx, geo.lonlat, geo.ringCounts, geo.ringsPerFeature,
                LongArray(geo.ringsPerFeature.size) { fid },
            )
        }
    }

    private fun clearHighlight() {
        if (highlightIdx >= 0) {
            map.removeOverlayLayer(highlightIdx)
            highlightIdx = -1
        }
    }

    override fun onDestroy() {
        clearHighlight()
        super.onDestroy()
    }
    companion object {
        // @Jin P0 真机验证地址：公开 HTTP 内网服务，SDK 下载到 filesDir/globecore/vector-cache 后交给 OGR。
        private const val PROVINCE_GEOJSON_URL =
            "http://210.74.129.84:8520/app-pub/web-data/geojson/district/province.json"
    }
}
