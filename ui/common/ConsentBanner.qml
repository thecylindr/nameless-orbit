import QtQuick
import QtQuick.Layouts

// Постоянная полоса наверху, пока идёт демонстрация экрана или управление им — чтобы об этом нельзя было забыть.
// danger — красный (идёт управление курсором/клавиатурой), иначе — акцентный (просто демонстрация экрана)
Rectangle {
    id: banner
    property string text: ""
    property string actionLabel: qsTr("Отключить")
    property bool danger: false
    signal actionClicked

    visible: text !== ""
    height: visible ? Math.max(44, row.implicitHeight + 16) : 0
    color: danger ? Theme.danger : Theme.accent

    Behavior on height { NumberAnimation { duration: 160 } }

    // подпись занимает всё свободное место и переносится на вторую строку: на узком экране телефона кнопка не вытесняет текст
    RowLayout {
        id: row
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 14; rightMargin: 14 }
        spacing: 12
        Icon { name: banner.danger ? "cursor" : "monitor"; size: 18; color: "white"; Layout.alignment: Qt.AlignVCenter }
        Text {
            Layout.fillWidth: true; Layout.minimumWidth: 60; Layout.preferredWidth: 200
            text: banner.text
            wrapMode: Text.Wrap
            color: "white"; font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.DemiBold
        }
        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: actionText.implicitWidth + 24; implicitHeight: 28; radius: 14
            color: Qt.rgba(0, 0, 0, actionMa.containsMouse ? 0.28 : 0.18)
            Text { id: actionText; anchors.centerIn: parent; text: banner.actionLabel; color: "white"; font.family: Theme.fontFamily; font.pixelSize: 12; font.weight: Font.Bold }
            MouseArea { id: actionMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: banner.actionClicked() }
        }
    }
}
