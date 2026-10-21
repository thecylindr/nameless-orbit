import QtQuick
import QtQuick.Shapes

// Значок из SVG-путей (сетка 24×24, линия 2, скруглённые концы) — рисуется Shape, поэтому не зависит от шрифтов
// и плагина qsvg и красится любым цветом. Имя — ключ в paths; неизвестное имя рисует пустоту
Item {
    id: ic
    property string name: ""
    property color color: Theme.text
    property real size: 20

    implicitWidth: size
    implicitHeight: size

    readonly property var paths: ({
        "back": "M15 5l-7 7 7 7",
        "chevron": "M9 5l7 7-7 7",
        "close": "M6 6l12 12M18 6l-12 12",
        "file": "M6 2.5h8l5 5v13a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1v-17a1 1 0 0 1 1-1zM14 2.5v5h5",
        "send": "M21 3L3 10.5l7 2.5 2.5 7zM10 13L21 3",
        "download": "M12 4v11M7 11l5 5 5-5M5 20h14",
        "upload": "M12 16V5M7 9l5-5 5 5M5 20h14",
        "speaker": "M4 9v6h4l5 4V5L8 9zM16.5 8.5a5 5 0 0 1 0 7",
        "stop": "M7 7h10v10H7z",
        "link": "M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1",
        "check": "M5 12.5l4.5 4.5L19 7.5",
        "plus": "M12 5v14M5 12h14",
        "pip": "M3 5h18v14H3zM12 12h7v5h-7z",
        "expand": "M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5",
        "minus": "M5 12h14",
        "settings": "M4 7h9M17 7h3M4 17h3M11 17h9M15 4v6M9 14v6",
        "refresh": "M20 12a8 8 0 1 1-2.34-5.66M20 4v5h-5",
        "wifi": "M2.5 9.5a14 14 0 0 1 19 0M5.5 13a9.5 9.5 0 0 1 13 0M8.8 16.4a4.8 4.8 0 0 1 6.4 0M12 19.8h.01",
        "plug": "M9 2v6M15 2v6M6 8h12v4a6 6 0 0 1-12 0zM12 18v4",
        "find": "M8 12a4 4 0 1 0 8 0a4 4 0 1 0-8 0M12 2v3M12 19v3M2 12h3M19 12h3",
        "clipboard": "M9 3h6v3H9zM8 4.5H6.5A1.5 1.5 0 0 0 5 6v14a1.5 1.5 0 0 0 1.5 1.5h11A1.5 1.5 0 0 0 19 20V6a1.5 1.5 0 0 0-1.5-1.5H16M9 12h6M9 16h4",
        "bell": "M18 16v-5a6 6 0 0 0-12 0v5l-2 3h16zM10 21a2 2 0 0 0 4 0",
        "monitor": "M4 4.5h16a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1v-10a1 1 0 0 1 1-1zM8 20.5h8M12 16.5v4",
        "play": "M8 5l11 7-11 7z",
        "pause": "M8 5v14M16 5v14",
        "prev": "M19 5l-10 7 10 7zM5.5 5v14",
        "next": "M5 5l10 7-10 7zM18.5 5v14",
        "trash": "M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13M10 11v5M14 11v5",
        "edit": "M4 20l1-5L16 4l4 4L9 19zM14 6l4 4",
        "bolt": "M13 2L4 14h7l-1 8 9-12h-7z",
        "battery": "M3.5 8h14a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1h-14a1 1 0 0 1-1-1V9a1 1 0 0 1 1-1zM21 11v2",
        "shield": "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z",
        "cursor": "M5 3l14 7-6 2.2L10.8 19z",
        "phone": "M8 2.5h8a2 2 0 0 1 2 2v15a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2v-15a2 2 0 0 1 2-2zM11 18.5h2",
        "tablet": "M6.5 2.5h11a2 2 0 0 1 2 2v15a2 2 0 0 1-2 2h-11a2 2 0 0 1-2-2v-15a2 2 0 0 1 2-2zM11 18.5h2",
        "laptop": "M5 5h14a1 1 0 0 1 1 1v9H4V6a1 1 0 0 1 1-1zM2 19h20",
        "desktop": "M4 4.5h16a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1v-10a1 1 0 0 1 1-1zM8 20.5h8M12 16.5v4",
        "server": "M4 4h16a1 1 0 0 1 1 1v4a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1zM4 14h16a1 1 0 0 1 1 1v4a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1v-4a1 1 0 0 1 1-1zM7 7h.01M7 17h.01",
        "orbit": "M9.5 12a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0-5 0M10.14 17.51A10 4.4 -28 0 1 6.40 10.46M19.99 6.46A10 4.4 -28 0 1 17.60 13.54M14.99 6.01a1.1 1.1 0 1 0 2.2 0a1.1 1.1 0 1 0-2.2 0"
    })

    // значок по виду устройства
    function kind(k) { return ({ phone: "phone", tablet: "tablet", laptop: "laptop", server: "server" })[k] || "desktop"; }

    Shape {
        width: 24; height: 24
        anchors.centerIn: parent
        scale: ic.size / 24
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: ic.color
            strokeWidth: 2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: ic.paths[ic.name] || "" }
        }
    }
}
