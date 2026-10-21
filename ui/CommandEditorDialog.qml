import QtQuick
import QtQuick.Controls.Basic
import "common" as Common

// Добавление и правка своей команды. Текст команды хранится только здесь, на этом компьютере:
// другие устройства видят название и значок, но не саму команду
Popup {
    id: dialog
    property string commandId: ""       // пусто — новая команда
    modal: true
    focus: true
    width: Math.min(420, (parent ? parent.width : 420) - 32)
    height: col.implicitHeight + 32
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle { radius: Theme.radius; color: Theme.surfaceHigh; border.width: 1; border.color: Theme.line }

    function openFor(id, name, icon, confirm) {
        commandId = id;
        nameField.text = id === "" ? "" : name;
        iconField.text = id === "" ? "" : icon;
        cmdField.text = id === "" ? "" : Commands.commandText(id);
        open();
    }
    readonly property bool valid: nameField.text.trim() !== "" && cmdField.text.trim() !== ""

    Column {
        id: col
        x: 16; y: 16
        width: parent.width - 32
        spacing: 10

        Text { text: dialog.commandId === "" ? qsTr("Новая команда") : qsTr("Правка команды"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.Bold }
        Field { id: nameField; placeholder: qsTr("Название, например «Заблокировать экран»") }
        Field { id: iconField; placeholder: qsTr("Значок: bolt, monitor, bell, plug, find… (необязательно)") }
        Field { id: cmdField; placeholder: qsTr("Команда, например loginctl lock-session") }
        Row {
            anchors.right: parent.right
            spacing: 10
            Common.GlassButton { text: qsTr("Отмена"); onClicked: dialog.close() }
            Common.GlassButton {
                text: qsTr("Сохранить"); primary: true; enabled: dialog.valid
                onClicked: {
                    if (dialog.commandId === "") Commands.addCommand(nameField.text.trim(), iconField.text.trim(), cmdField.text.trim(), false);
                    else Commands.updateCommand(dialog.commandId, nameField.text.trim(), iconField.text.trim(), cmdField.text.trim(), false);
                    dialog.close();
                }
            }
        }
    }

    component Field: Rectangle {
        property alias text: tf.text
        property string placeholder: ""
        width: parent.width; height: 38; radius: 10
        color: Theme.surface; border.width: 1; border.color: tf.activeFocus ? Theme.accent : Theme.line
        TextField {
            id: tf
            anchors.fill: parent
            leftPadding: 12; rightPadding: 12
            placeholderText: parent.placeholder
            color: Theme.text; placeholderTextColor: Theme.subtext
            background: null
            selectByMouse: true
        }
    }
}
