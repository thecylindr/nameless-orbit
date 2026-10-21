import QtQuick
import QtQuick.Controls.Basic
import "common" as Common

// Главная страница: шапка (имя этого устройства, поиск, настройки), запросы на сопряжение
// и список устройств рядом / сопряжённых
Page {
    id: page
    signal openDevice(string id)
    signal openSettings

    background: null

    PairingDialog { id: pairDialog }

    header: Item {
        implicitHeight: 76
        Common.Icon { id: logo; x: 20; anchors.verticalCenter: parent.verticalCenter; name: "orbit"; size: 32; color: Theme.accent }
        Column {
            x: 64; anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text { text: "Orbit"; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 20; font.weight: Font.Bold }
            Text {
                text: Identity.deviceName + (PeerManagerModel.listening ? "" : qsTr(" · порт занят, входящие не принимаются"))
                color: PeerManagerModel.listening ? Theme.subtext : Theme.warn
                font.family: Theme.fontFamily; font.pixelSize: 12
            }
        }
        Row {
            anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
            spacing: 8
            Common.GlassButton { icon: "refresh"; text: ""; onClicked: PeerManagerModel.refresh() }
            Common.GlassButton { icon: "settings"; text: ""; onClicked: page.openSettings() }
        }
    }

    ListView {
        id: list
        anchors { top: parent.top; bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
        width: Math.min(parent.width - 32, 780)        // на большом окне содержимое не растягивается на всю ширину
        spacing: 8
        clip: true
        model: PeerManagerModel.nearbyDevices

        // блок над списком: запросы на сопряжение, поиск, подключение по адресу
        header: Column {
            width: list.width
            spacing: 10
            bottomPadding: 12

            Repeater {
                model: PeerManagerModel.pairingRequests
                PairingRequestBanner {
                    required property var modelData
                    width: list.width
                    requestData: modelData
                    onAccepted: PeerManagerModel.acceptPairing(modelData.deviceId)
                    onRejected: PeerManagerModel.rejectPairing(modelData.deviceId)
                }
            }
            Common.GlassCard {
                width: list.width; height: 64
                Common.GlassSwitch {
                    anchors.fill: parent
                    label: qsTr("Искать устройства в сети")
                    sub: qsTr("Другие устройства Orbit увидят это")
                    checked: PeerManagerModel.discoveryActive
                    onToggled: v => PeerManagerModel.discoveryActive = v
                }
            }
            Common.GlassButton { icon: "wifi"; text: qsTr("Подключиться по адресу"); onClicked: pairDialog.openFresh() }
        }

        delegate: DeviceCard {
            required property var modelData
            width: ListView.view.width
            deviceData: modelData
            onClicked: connected ? page.openDevice(modelData.deviceId) : PeerManagerModel.connectToDevice(modelData.deviceId)
            onActionClicked: connected ? page.openDevice(modelData.deviceId) : PeerManagerModel.connectToDevice(modelData.deviceId)
        }

        footer: Column {
            visible: list.count === 0
            width: list.width
            topPadding: 28
            spacing: 14
            Common.Icon { anchors.horizontalCenter: parent.horizontalCenter; name: "orbit"; size: 72; color: Theme.line }
            Text {
                width: parent.width - 48; anchors.horizontalCenter: parent.horizontalCenter
                horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                text: PeerManagerModel.discoveryActive ? qsTr("Пока никого рядом.\nОткройте Orbit на другом устройстве в этой же сети.") : qsTr("Поиск выключен — включите его выше.")
                color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 13
            }
        }
    }
}
