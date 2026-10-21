import QtQuick
import QtQuick.Controls.Basic
import "common" as Common

// «Подключиться по адресу» — для сопряжения не в одной локальной сети (маяк не доходит: другой Wi-Fi,
// VPN, проброс порта). Дальше — тот же путь, что и для найденных по маяку: рукопожатие, запрос на сопряжение
// (PairingRequestBanner), сверка кода
Popup {
    id: dialog
    modal: true
    focus: true
    width: 360
    height: col.implicitHeight + 32
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle { radius: Theme.radius; color: Theme.surfaceHigh; border.width: 1; border.color: Theme.line }

    function openFresh() { hostField.text = ""; portField.text = "17894"; open(); }

    Column {
        id: col
        x: 16; y: 16
        width: parent.width - 32
        spacing: 12

        Text { text: qsTr("Подключиться по адресу"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.Bold }
        Text { text: qsTr("Для устройства не в этой сети: IP-адрес и порт Orbit (по умолчанию 17894)"); color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11; wrapMode: Text.WordWrap; width: parent.width }

        Rectangle {
            width: parent.width; height: 38; radius: 10
            color: Theme.surface; border.width: 1; border.color: hostField.activeFocus ? Theme.accent : Theme.line
            TextField {
                id: hostField
                anchors.fill: parent
                leftPadding: 12; rightPadding: 12
                placeholderText: "192.168.1.42"
                color: Theme.text
                placeholderTextColor: Theme.subtext
                background: null
                selectByMouse: true
            }
        }
        Rectangle {
            width: 120; height: 38; radius: 10
            color: Theme.surface; border.width: 1; border.color: portField.activeFocus ? Theme.accent : Theme.line
            TextField {
                id: portField
                anchors.fill: parent
                leftPadding: 12; rightPadding: 12
                validator: IntValidator { bottom: 1; top: 65535 }
                color: Theme.text
                background: null
                selectByMouse: true
            }
        }
        Row {
            anchors.right: parent.right
            spacing: 10
            Common.GlassButton { text: qsTr("Отмена"); onClicked: dialog.close() }
            Common.GlassButton {
                text: qsTr("Подключиться"); primary: true
                enabled: hostField.text.trim() !== ""
                onClicked: {
                    PeerManagerModel.connectByAddress(hostField.text.trim(), parseInt(portField.text || "17894"));
                    dialog.close();
                }
            }
        }
    }
}
