import QtQuick
import QtQuick.Controls.Basic
import "common" as Common

// «Второй экран»: это устройство становится ещё одним монитором другого компьютера. Здесь — как именно: ориентация, разрешение, частота и
// где стоит новый монитор относительно основного (курсор на него переходит сам). Выбор запоминается до следующего раза
Popup {
    id: dlg
    property string deviceId: ""
    property string orientation: "auto"
    property string res: "native"
    property int fps: 60
    property string side: "right"
    property string smode: "extend"      // extend — отдельный монитор Hyprland (расширение рабочего пространства); mirror — повтор существующего монитора
    property string workspace: ""        // стол Hyprland на новом мониторе: пусто — как в настройках компьютера, own — отдельный, 1–10 — этот стол
    property string audioMode: "auto"    // звук компьютера на этом устройстве: auto — как в настройках, on — включить, off — нет
    property string audioCodec: "auto"   // auto | pcm | opus
    property string uiScale: "auto"        // масштаб интерфейса на новом мониторе: auto — по размеру экрана (или настройке компьютера), либо число
    property string mode: "request"      // request — это устройство само становится вторым экраном; offer — этот компьютер предлагает другому устройству стать вторым экраном
    property var windows: []              // окна driftwm (бета) для режима offer
    property string windowChoice: ""
    readonly property bool offer: mode === "offer"
    signal started(string deviceId)

    modal: true
    focus: true
    width: Math.min(440, (parent ? parent.width : 440) - 32)
    height: Math.min(col.implicitHeight + 36, (parent ? parent.height : 700) - 32)
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { radius: Theme.radius; color: Theme.surfaceHigh; border.width: 1.5; border.color: Theme.accent }

    function openFor(id, m) {
        deviceId = id; mode = m || "request";
        windows = (offer && RemoteDesktop.secondBeta) ? RemoteDesktop.driftwmWindows() : [];
        windowChoice = windows.length > 0 ? String(windows[0].id) : "";
        open();
    }

    // ряд вариантов: подпись и кнопки-«таблетки»
    component Choice: Column {
        id: ch
        property string label
        property var options: []
        property var value
        signal picked(var v)
        width: parent.width
        spacing: 6
        Text { text: ch.label; color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11 }
        Flow {
            width: parent.width; spacing: 6
            Repeater {
                model: ch.options
                Common.GlassButton {
                    required property var modelData
                    text: modelData.t
                    primary: ch.value === modelData.v
                    onClicked: ch.picked(modelData.v)
                }
            }
        }
    }

    Flickable {
        anchors { fill: parent; margins: 18 }
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Column {
            id: col
            width: parent.width
            spacing: 14

            Row {
                spacing: 10
                Common.Icon { name: "monitor"; size: 22; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                Text { text: dlg.offer ? qsTr("Использовать как второй экран") : qsTr("Второй экран"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 16; font.weight: Font.Bold; anchors.verticalCenter: parent.verticalCenter }
            }
            Text {
                width: parent.width; wrapMode: Text.WordWrap
                text: dlg.offer ? qsTr("Устройство получит запрос; после согласия (или сразу, если оно «доверенное») его экран целиком станет ещё одним монитором этого компьютера: можно перетащить окна и перевести мышь, касания работают как мышь.")
                             : qsTr("Это устройство станет ещё одним монитором другого компьютера: на него можно перетащить окна и перевести мышь, касания работают как мышь.")
                color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
            }
            Choice {
                label: qsTr("Ориентация")
                options: [{ v: "auto", t: qsTr("Как сейчас") }, { v: "landscape", t: qsTr("Альбомная") }, { v: "portrait", t: qsTr("Портретная") }]
                value: dlg.orientation; onPicked: v => dlg.orientation = v
            }
            Choice {
                label: qsTr("Разрешение")
                options: [{ v: "native", t: qsTr("Родное") }, { v: "1080", t: "1080p" }, { v: "720", t: "720p" }]
                value: dlg.res; onPicked: v => dlg.res = v
            }
            Choice {
                label: qsTr("Частота кадров")
                options: [{ v: 30, t: qsTr("30 к/с") }, { v: 60, t: qsTr("60 к/с") }, { v: 90, t: qsTr("90 к/с") }]
                value: dlg.fps; onPicked: v => dlg.fps = v
            }
            Choice {
                label: qsTr("Что показывать")
                options: [{ v: "extend", t: qsTr("Отдельный монитор (расширение)") }, { v: "mirror", t: qsTr("Повтор монитора компьютера") }]
                value: dlg.smode; onPicked: v => dlg.smode = v
            }
            Choice {
                visible: dlg.smode === "extend"
                label: qsTr("Какой рабочий стол Hyprland (1–10) показывает этот экран")
                options: [{ v: "", t: qsTr("Как в настройках ПК") }, { v: "own", t: qsTr("Свой, отдельный") }].concat([1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map(n => ({ v: String(n), t: String(n) })))
                value: dlg.workspace; onPicked: v => dlg.workspace = v
            }
            Choice {
                visible: !dlg.offer
                label: qsTr("Звук компьютера на этом устройстве")
                options: [{ v: "auto", t: qsTr("Как в настройках") }, { v: "on", t: qsTr("Включить") }, { v: "off", t: qsTr("Выключить") }]
                value: dlg.audioMode; onPicked: v => dlg.audioMode = v
            }
            Choice {
                visible: !dlg.offer && dlg.audioMode !== "off"
                label: qsTr("Передача звука")
                options: [{ v: "auto", t: qsTr("Авто") }, { v: "pcm", t: qsTr("Без сжатия") }].concat(RemoteDesktop.audioOpusAvailable ? [{ v: "opus", t: qsTr("Opus (меньше трафика)") }] : [])
                value: dlg.audioCodec; onPicked: v => dlg.audioCodec = v
            }
            Choice {
                label: qsTr("Масштаб (крупность интерфейса на мониторе)")
                options: [{ v: "auto", t: qsTr("Авто") }, { v: "1", t: "100%" }, { v: "1.25", t: "125%" }, { v: "1.5", t: "150%" }, { v: "2", t: "200%" }]
                value: dlg.uiScale; onPicked: v => dlg.uiScale = v
            }
            Choice {
                label: qsTr("Где стоит новый монитор")
                options: [{ v: "left", t: qsTr("Слева") }, { v: "right", t: qsTr("Справа") }, { v: "top", t: qsTr("Сверху") }, { v: "bottom", t: qsTr("Снизу") }]
                value: dlg.side; onPicked: v => dlg.side = v
            }
            Choice {
                visible: dlg.offer && RemoteDesktop.secondBeta
                label: qsTr("Окно приложения (driftwm, бета)")
                options: dlg.windows.map(w => ({ v: String(w.id), t: w.label }))
                value: dlg.windowChoice; onPicked: v => dlg.windowChoice = v
            }
            // что нужно на другом компьютере
            Column {
                width: parent.width; spacing: 4
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: dlg.offer ? qsTr("Монитор создаётся на этом компьютере (Hyprland — настоящий виртуальный; driftwm — окно приложения, бета).")
                                    : qsTr("Нужен компьютер с Hyprland: монитор создаётся настоящий, виртуальный. Другой компьютер сначала спросит разрешение.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
                Row {
                    spacing: 8
                    // метка «бета» — ярким зелёным: так же она видна и в настройках, и в запросе на другом компьютере
                    Rectangle {
                        width: betaText.implicitWidth + 14; height: 20; radius: 10
                        color: Qt.rgba(0.13, 0.88, 0.42, 0.18); border.width: 1; border.color: "#22e06b"
                        Text { id: betaText; anchors.centerIn: parent; text: qsTr("БЕТА"); color: "#22e06b"; font.family: Theme.fontFamily; font.pixelSize: 11; font.weight: Font.Bold; font.letterSpacing: 0.8 }
                    }
                    Text { text: qsTr("driftwm: вторым экраном станет окно приложения"); color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
                }
            }
            Row {
                anchors.right: parent.right
                spacing: 10
                Common.GlassButton { text: qsTr("Отмена"); onClicked: dlg.close() }
                Common.GlassButton {
                    text: dlg.offer ? qsTr("Предложить") : qsTr("Включить"); primary: true
                    onClicked: {
                        const spec = { orientation: dlg.orientation, res: dlg.res, fps: dlg.fps, side: dlg.side, scale: dlg.uiScale, mode: dlg.smode, workspace: dlg.workspace, audio: dlg.audioMode, acodec: dlg.audioCodec };
                        if (dlg.offer) RemoteDesktop.offerSecondScreen(dlg.deviceId, spec, dlg.windowChoice);
                        else { RemoteDesktop.requestSecondScreen(dlg.deviceId, spec); dlg.started(dlg.deviceId); }
                        dlg.close();
                    }
                }
            }
        }
    }
}
