import QtQuick

// Карточка-подложка: тёмное стекло с тонким краем, как панели NameLess UI
Rectangle {
    id: card
    default property alias content: inner.data
    property real pad: 16

    radius: Theme.radius
    color: Theme.surface
    border.width: 1
    border.color: Theme.line

    Item {
        id: inner
        anchors.fill: parent
        anchors.margins: card.pad
    }
}
