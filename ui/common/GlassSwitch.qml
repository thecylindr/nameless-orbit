import QtQuick

// Переключатель с подписью и необязательным пояснением — для списков разрешений по плагинам
Item {
    id: sw
    property string label: ""
    property string sub: ""
    property bool checked: false
    signal toggled(bool value)

    implicitHeight: Math.max(40, col.implicitHeight)
    opacity: enabled ? 1 : 0.4

    Column {
        id: col
        anchors { left: parent.left; right: track.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
        spacing: 2
        Text { text: sw.label; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.Medium; width: parent.width; elide: Text.ElideRight }
        Text { visible: sw.sub !== ""; text: sw.sub; color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11; width: parent.width; wrapMode: Text.WordWrap }
    }
    Rectangle {
        id: track
        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
        width: 44; height: 26; radius: 13
        color: sw.checked ? Theme.accent : Theme.surfaceHigh
        border.width: 1; border.color: Theme.line
        Behavior on color { ColorAnimation { duration: 140 } }
        Rectangle {
            width: 20; height: 20; radius: 10
            y: 3; x: sw.checked ? 21 : 3
            color: "white"
            Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        }
    }
    MouseArea {
        anchors.fill: parent
        enabled: sw.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: { sw.checked = !sw.checked; sw.toggled(sw.checked); }
    }
}
