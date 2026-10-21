import QtQuick
import "common" as Common

// Запрос на сопряжение — код сравнивается на обоих экранах (как номер в Bluetooth), отвечает здесь только
// человек: сообщение само ничего не принимает. requestData — элемент из PeerManagerModel.pairingRequests
Rectangle {
    id: banner
    property var requestData: ({})
    signal accepted
    signal rejected

    implicitHeight: col.implicitHeight + 28
    radius: Theme.radius
    color: Theme.surfaceHigh
    border.width: 1.5; border.color: Theme.accent

    Column {
        id: col
        x: 18; y: 14
        width: parent.width - 36
        spacing: 10

        Row {
            spacing: 10
            Common.Icon { name: "shield"; size: 24; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                Text { text: (banner.requestData.outgoing ? qsTr("Сопряжение с ") : qsTr("Хочет сопряжение: ")) + (banner.requestData.name || ""); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold }
                Text { text: [banner.requestData.model, banner.requestData.address].filter(function (x) { return x; }).join(" · "); visible: text !== ""; color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11 }
                Text { text: qsTr("Сверьте код на обоих экранах"); color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11 }
            }
        }
        Text {
            text: banner.requestData.code || ""
            color: Theme.accent
            font.family: "monospace"
            font.pixelSize: 26
            font.letterSpacing: 4
            font.weight: Font.Bold
        }
        // доверие действиям: что сможет запускать у вас второе устройство и что сможете запускать вы
        Column {
            width: parent.width
            spacing: 6
            visible: banner.requestData.localAccepted !== true
            Text {
                visible: Commands.commandsForQml.length > 0
                width: parent.width; wrapMode: Text.WordWrap
                text: qsTr("Оно сможет запускать у вас без подтверждения:")
                color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
            }
            Flow {
                width: parent.width; spacing: 6
                Repeater {
                    model: Commands.commandsForQml
                    Rectangle {
                        required property var modelData
                        width: cn.implicitWidth + 28; height: 26; radius: 13
                        color: Theme.surface; border.width: 1; border.color: Theme.line
                        Row { anchors.centerIn: parent; spacing: 5
                            Common.Icon { name: parent.parent.modelData.icon; size: 13; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                            Text { id: cn; text: parent.parent.modelData.name; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
                        }
                    }
                }
            }
            Common.GlassSwitch {
                visible: Commands.commandsForQml.length > 0
                width: parent.width
                label: qsTr("Разрешить запуск этих команд")
                sub: qsTr("можно изменить позже на странице устройства")
                checked: banner.requestData.allowCommands !== false
                onToggled: v => PeerManagerModel.setPairingCommandsAllowed(banner.requestData.deviceId, v)
            }
            Text {
                visible: (banner.requestData.remoteCommands || []).length > 0
                width: parent.width; wrapMode: Text.WordWrap
                text: qsTr("Его быстрые команды (вы сможете их запускать):")
                color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
            }
            Flow {
                width: parent.width; spacing: 6
                Repeater {
                    model: banner.requestData.remoteCommands || []
                    Rectangle {
                        required property var modelData
                        width: rn.implicitWidth + 28; height: 26; radius: 13
                        color: Theme.surface; border.width: 1; border.color: Theme.line
                        Row { anchors.centerIn: parent; spacing: 5
                            Common.Icon { name: parent.parent.modelData.icon; size: 13; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                            Text { id: rn; text: parent.parent.modelData.name; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
                        }
                    }
                }
            }
        }
        Text {
            visible: banner.requestData.localAccepted === true
            width: parent.width; wrapMode: Text.WordWrap
            text: qsTr("Вы согласились. Ждём подтверждения на втором устройстве…")
            color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
        }
        Text {
            visible: banner.requestData.remoteAccepted === true && banner.requestData.localAccepted !== true
            width: parent.width; wrapMode: Text.WordWrap
            text: qsTr("Второе устройство уже согласно — сверьте код и подтвердите.")
            color: Theme.good; font.family: Theme.fontFamily; font.pixelSize: 12
        }
        Flow {
            width: parent.width; spacing: 10
            GlassButtonLocal { text: banner.requestData.localAccepted === true ? qsTr("Отмена") : qsTr("Отклонить"); onClicked: banner.rejected() }
            GlassButtonLocal { visible: banner.requestData.localAccepted !== true; text: qsTr("Коды совпадают — сопрячь"); primary: true; onClicked: banner.accepted() }
        }
    }

    // маленькая кнопка без отдельного файла — тот же стиль, что у common/GlassButton, но для локального использования в баннере
    component GlassButtonLocal: Item {
        property string text: ""
        property bool primary: false
        signal clicked
        implicitWidth: t.implicitWidth + 26; implicitHeight: 34
        Rectangle { anchors.fill: parent; radius: height / 2; color: primary ? Theme.accent : "transparent"; border.width: primary ? 0 : 1; border.color: Theme.line }
        Text { id: t; anchors.centerIn: parent; text: parent.text; color: primary ? "white" : Theme.text; font.family: Theme.fontFamily; font.pixelSize: 12; font.weight: Font.DemiBold }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked() }
    }
}
