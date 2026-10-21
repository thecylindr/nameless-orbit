import QtQuick

// Кнопка-таблетка: акцентная (primary), стеклянная (по умолчанию) или красная (danger). Без текста — круглая кнопка
// со значком. icon — имя значка из Icon.qml. Блокируется встроенным свойством enabled
Item {
    id: btn
    property string text: ""
    property string icon: ""
    property bool primary: false
    property bool danger: false
    signal clicked

    readonly property bool round: text === ""
    implicitWidth: round ? implicitHeight : row.implicitWidth + 32
    implicitHeight: IsMobile ? 46 : 40
    opacity: enabled ? 1 : 0.4

    readonly property color baseColor: danger ? Theme.danger : (primary ? Theme.accent : Theme.surfaceHigh)

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: ma.pressed ? Qt.darker(btn.baseColor, 1.2) : (!btn.primary && !btn.danger && ma.containsMouse ? Qt.lighter(btn.baseColor, 1.3) : btn.baseColor)
        border.width: btn.primary || btn.danger ? 0 : 1
        border.color: Theme.line
        Behavior on color { ColorAnimation { duration: 120 } }
    }
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 8
        Icon { visible: btn.icon !== ""; name: btn.icon; size: 18; color: btn.primary || btn.danger ? "white" : Theme.text; anchors.verticalCenter: parent.verticalCenter }
        Text { visible: btn.text !== ""; text: btn.text; color: btn.primary || btn.danger ? "white" : Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.DemiBold; anchors.verticalCenter: parent.verticalCenter }
    }
    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        enabled: btn.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: btn.clicked()
    }
}
