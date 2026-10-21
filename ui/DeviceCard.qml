import QtQuick
import "common" as Common

// Строка устройства в списке: значок по виду (kind) в круге, имя, состояние (рядом / сопряжено / подключено)
// и справа кнопка по состоянию. deviceData — элемент из PeerManagerModel.nearbyDevices
Rectangle {
    id: card
    property var deviceData: ({})
    signal clicked
    signal actionClicked

    readonly property bool connected: deviceData.connected === true
    readonly property bool paired: deviceData.paired === true
    readonly property bool connecting: deviceData.connecting === true

    implicitHeight: IsMobile ? 88 : 80
    radius: Theme.radius
    color: ma.pressed ? Theme.surfaceHigh : (ma.containsMouse ? Qt.lighter(Theme.surface, 1.25) : Theme.surface)
    border.width: 1; border.color: connected ? Qt.rgba(0.04, 0.52, 1, 0.55) : Theme.line
    Behavior on color { ColorAnimation { duration: 120 } }

    Rectangle {
        id: badge
        x: 14; anchors.verticalCenter: parent.verticalCenter
        width: 44; height: 44; radius: 22
        color: card.connected ? Theme.accent : Theme.surfaceHigh
        Common.Icon { anchors.centerIn: parent; name: kindName; size: 22; color: card.connected ? "white" : Theme.subtext }
        Rectangle {
            visible: card.connected
            width: 12; height: 12; radius: 6
            x: parent.width - 11; y: parent.height - 11
            color: Theme.good
            border.width: 2; border.color: card.color
        }
    }
    readonly property string kindName: ({ phone: "phone", tablet: "tablet", laptop: "laptop", server: "server" })[deviceData.kind] || "desktop"

    Column {
        anchors { left: badge.right; leftMargin: 14; right: actionBtn.left; rightMargin: 10; verticalCenter: parent.verticalCenter }
        spacing: 3
        Text { text: card.deviceData.name || ""; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.DemiBold; width: parent.width; elide: Text.ElideRight }
        // модель устройства и его адрес в локальной сети
        Text {
            width: parent.width; elide: Text.ElideRight
            text: [card.deviceData.model, card.deviceData.address].filter(function (x) { return x; }).join(" · ")
            color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
        }
        Text {
            text: card.connecting ? qsTr("подключение…") : (card.connected ? (card.deviceData.confirmed === true ? qsTr("подключено") : qsTr("ждём подтверждения на втором устройстве")) : (card.paired ? qsTr("сопряжено · не подключено") : qsTr("рядом · не сопряжено")))
            color: card.connected ? (card.deviceData.confirmed === true ? Theme.good : Theme.warn) : Theme.subtext
            font.family: Theme.fontFamily; font.pixelSize: 12
        }
    }
    Common.GlassButton {
        id: actionBtn
        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
        icon: card.connected ? "chevron" : "plug"
        visible: !card.connecting
        onClicked: card.actionClicked()
    }
    MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; z: -1; cursorShape: Qt.PointingHandCursor; onClicked: card.clicked() }
}
