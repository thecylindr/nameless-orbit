import QtQuick

// Короткое уведомление внизу окна: show("текст") — появляется и само прячется через 3 с
Item {
    id: toast
    anchors { horizontalCenter: parent ? parent.horizontalCenter : undefined; bottom: parent ? parent.bottom : undefined; bottomMargin: 24 }
    width: card.implicitWidth
    height: card.implicitHeight
    opacity: 0
    visible: opacity > 0.01

    property string message: ""
    function show(text) { message = text; opacity = 1; hideTimer.restart(); }

    Behavior on opacity { NumberAnimation { duration: 180 } }
    Timer { id: hideTimer; interval: 3000; onTriggered: toast.opacity = 0 }

    Rectangle {
        id: card
        implicitWidth: label.implicitWidth + 32
        implicitHeight: 40
        radius: height / 2
        color: Theme.surfaceHigh
        border.width: 1; border.color: Theme.line
        Text { id: label; anchors.centerIn: parent; text: toast.message; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 12 }
    }
}
