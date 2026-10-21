import QtQuick

// Числовой выбор в стиле Orbit: таблетка «− значение +». Кнопки с повтором при удержании, значение можно листать колесом.
// value не привязывается к внутренним нажатиям — снаружи читается свойство и сигнал valueModified(int)
Item {
    id: spin
    property int from: 0
    property int to: 100
    property int value: 0
    property int stepSize: 1
    property string suffix: ""
    property string zeroText: ""          // подпись вместо «0» (например «без ограничения»)
    signal valueModified(int value)

    implicitWidth: 168
    implicitHeight: IsMobile ? 46 : 40

    function change(delta) {
        const v = Math.max(from, Math.min(to, value + delta * stepSize));
        if (v === value) return;
        value = v;
        valueModified(v);
    }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Theme.surface
        border.width: 1; border.color: Theme.line
    }

    component Step: Item {
        id: st
        property int delta: 1
        property string icon: "plus"
        readonly property bool blocked: delta < 0 ? spin.value <= spin.from : spin.value >= spin.to
        width: parent.height; height: parent.height
        opacity: blocked ? 0.35 : 1
        Rectangle {
            anchors.centerIn: parent
            width: parent.width - 8; height: width; radius: width / 2
            color: ma.pressed ? Theme.surfaceHigh : (ma.containsMouse ? Qt.lighter(Theme.surface, 1.4) : "transparent")
        }
        Icon { anchors.centerIn: parent; name: st.icon; size: 16; color: Theme.text }
        // удержание: первое срабатывание сразу, потом после паузы ускоренное повторение
        Timer { id: rep; interval: 120; repeat: true; onTriggered: spin.change(st.delta) }
        Timer { id: hold; interval: 420; onTriggered: rep.start() }
        MouseArea {
            id: ma
            anchors.fill: parent; hoverEnabled: true
            enabled: !st.blocked
            cursorShape: Qt.PointingHandCursor
            onPressed: { spin.change(st.delta); hold.start(); }
            onReleased: { hold.stop(); rep.stop(); }
            onCanceled: { hold.stop(); rep.stop(); }
        }
    }

    Step { id: minus; delta: -1; icon: "minus"; anchors.left: parent.left }
    Step { id: plus; delta: 1; icon: "plus"; anchors.right: parent.right }

    Text {
        anchors { left: minus.right; right: plus.left; verticalCenter: parent.verticalCenter }
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: spin.value === 0 && spin.zeroText !== "" ? spin.zeroText : spin.value + (spin.suffix !== "" ? " " + spin.suffix : "")
        color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold
    }
    WheelHandler { onWheel: event => spin.change(event.angleDelta.y > 0 ? 1 : -1) }
}
