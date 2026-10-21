import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import "common" as Common

// Страница одного устройства: состояние и батарея, действия (файл, буфер, поиск, экран), передачи, медиа, команды,
// затем разрешения, которые вы даёте ЭТОМУ устройству, и разрыв сопряжения
Page {
    id: page
    property string deviceId: ""
    signal back
    signal openScreen(string id)

    background: null

    // ---- данные устройства ----
    function findIn(list) {
        for (var i = 0; i < list.length; ++i) if (list[i].deviceId === deviceId) return list[i];
        return null;
    }
    readonly property var live: findIn(PeerManagerModel.connectedDevices)
    readonly property var trusted: findIn(TrustStoreModel.devices)
    readonly property bool online: live !== null
    readonly property bool confirmed: live !== null && live.confirmed === true      // второе устройство тоже приняло сопряжение
    readonly property var caps: live ? live.remoteCapabilities : []
    readonly property string devName: live ? live.name : (trusted ? trusted.name : "")
    function can(plugin) { return caps.indexOf(plugin) >= 0; }

    readonly property string kindName: ({ phone: "phone", tablet: "tablet", laptop: "laptop", server: "server" })[(live ? live.kind : (trusted ? trusted.kind : ""))] || "desktop"
    property int battery: -1
    property bool charging: false
    property string trackTitle: ""
    property string trackArtist: ""
    property bool trackPlaying: false
    property var remoteCommands: RunCommand.remoteCommands(deviceId)       // из кэша: список есть сразу при открытии страницы

    function refreshBattery() { battery = Battery.hasRemote(deviceId) ? Battery.remotePercent(deviceId) : -1; charging = Battery.remoteCharging(deviceId); }
    Component.onCompleted: { refreshBattery(); if (confirmed) RunCommand.requestCommands(deviceId); }
    onConfirmedChanged: if (confirmed) RunCommand.requestCommands(deviceId)       // сопряжение подтверждено — просим список команд

    // выбор файлов для отправки; на компьютере файлы можно и просто перетащить на страницу
    FileDialog {
        id: fileDialog
        fileMode: FileDialog.OpenFiles
        onAccepted: { for (var i = 0; i < selectedFiles.length; ++i) Share.sendFile(page.deviceId, selectedFiles[i].toString()); }
    }
    DropArea {
        anchors.fill: parent
        z: 50
        onDropped: drop => { if (drop.hasUrls) for (var i = 0; i < drop.urls.length; ++i) Share.sendFile(page.deviceId, drop.urls[i].toString()); }
    }
    function fmtSize(b) {
        if (b >= 1073741824) return (b / 1073741824).toFixed(1) + qsTr(" ГБ");
        if (b >= 1048576) return (b / 1048576).toFixed(1) + qsTr(" МБ");
        if (b >= 1024) return Math.round(b / 1024) + qsTr(" КБ");
        return b + qsTr(" Б");
    }
    readonly property var myTransfers: Share.transfers.filter(function (t) { return t.deviceId === page.deviceId; }).slice(-6)

    Connections {
        target: Battery
        function onRemoteChanged(id) { if (id === page.deviceId) page.refreshBattery(); }
    }
    Connections {
        target: Media
        function onRemoteTrackChanged(id, title, artist, playing) {
            if (id !== page.deviceId) return;
            page.trackTitle = title; page.trackArtist = artist; page.trackPlaying = playing;
        }
    }
    Connections {
        target: RunCommand
        function onRemoteCommandsUpdated(id, commands) { if (id === page.deviceId) page.remoteCommands = commands; }
    }

    header: Item {
        implicitHeight: 64
        Common.GlassButton { x: 16; anchors.verticalCenter: parent.verticalCenter; icon: "back"; text: ""; onClicked: page.back() }
        Common.Icon { id: devIcon; x: 78; anchors.verticalCenter: parent.verticalCenter; name: page.kindName; size: 26; color: Theme.accent }
        Column {
            x: 116; anchors.verticalCenter: parent.verticalCenter
            Text { text: page.devName; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 18; font.weight: Font.Bold }
            Text {
                text: [page.live ? page.live.model : "", page.live ? page.live.address : "", page.battery >= 0 ? page.battery + "%" + (page.charging ? " ⚡" : "") : ""].filter(function (x) { return x; }).join(" · ")
                color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
            }
        }
    }

    Flickable {
        anchors { top: parent.top; bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
        width: Math.min(parent.width - 32, 780)        // на большом окне содержимое не растягивается на всю ширину
        contentHeight: body.implicitHeight + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: body
            width: parent.width
            spacing: 14

            // устройство не подключено: страница остаётся, действия недоступны, подключиться можно отсюда
            Rectangle {
                visible: !page.online
                width: parent.width; height: 64; radius: Theme.radius
                color: Qt.rgba(1, 0.84, 0.04, 0.12); border.width: 1; border.color: Theme.warn
                Text { x: 16; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Не подключено"); color: Theme.warn; font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold }
                Common.GlassButton { anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter } icon: "plug"; text: qsTr("Подключить"); primary: true; onClicked: PeerManagerModel.connectToDevice(page.deviceId) }
            }

            // подключено, но второе устройство ещё не подтвердило сопряжение — функции появятся сразу после его «Сопрячь»
            Rectangle {
                visible: page.online && !page.confirmed
                width: parent.width; height: waitTxt.implicitHeight + 28; radius: Theme.radius
                color: Qt.rgba(1, 0.84, 0.04, 0.12); border.width: 1; border.color: Theme.warn
                Text { id: waitTxt; x: 16; y: 14; width: parent.width - 32; wrapMode: Text.WordWrap; text: qsTr("Ждём подтверждения на втором устройстве. Откройте там Orbit и нажмите «Коды совпадают — сопрячь»: после этого здесь появятся файлы, экран, плеер и команды."); color: Theme.warn; font.family: Theme.fontFamily; font.pixelSize: 12 }
            }

            // действия сгруппированы по смыслу, у каждой кнопки понятное название; серая кнопка объясняется строкой ниже
            Section { title: qsTr("Файлы и сообщения"); visible: page.online && page.confirmed
                Flow {
                    width: parent.width; spacing: 8
                    Common.GlassButton { enabled: page.can("share"); icon: "send"; text: qsTr("Отправить файл"); primary: true; onClicked: fileDialog.open() }
                    Common.GlassButton { enabled: page.can("clipboard"); icon: "clipboard"; text: qsTr("Отправить буфер"); onClicked: Clipboard.sendClipboardTo(page.deviceId) }
                    Common.GlassButton { enabled: page.can("notifications"); icon: "bell"; text: qsTr("Тест-уведомление"); onClicked: Notifications.sendTo(page.deviceId, "Orbit", qsTr("Проверка связи с ") + Identity.deviceName) }
                }
                Text {
                    visible: !IsMobile
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Файлы можно просто перетащить на это окно.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
            }

            Section { title: qsTr("Найти устройство"); visible: page.online && page.confirmed && page.can("find")
                Flow {
                    width: parent.width; spacing: 8
                    Common.GlassButton { icon: "speaker"; text: qsTr("Позвонить"); primary: true; onClicked: FindDevice.ring(page.deviceId) }
                    Common.GlassButton { icon: "stop"; text: qsTr("Отменить звонок"); onClicked: FindDevice.stop(page.deviceId) }
                }
            }

            Section { title: qsTr("Экран"); visible: page.online && page.confirmed
                Flow {
                    width: parent.width; spacing: 8
                    Common.GlassButton {
                        enabled: page.can("remoteDesktop"); icon: "monitor"; text: qsTr("Смотреть его экран"); primary: true
                        onClicked: { RemoteDesktop.requestShare(page.deviceId); page.openScreen(page.deviceId); }
                    }
                    Common.GlassButton {
                        visible: RemoteDesktop.captureAvailable
                        enabled: page.can("remoteDesktop") && Settings.remoteDesktopFeatureEnabled
                        icon: "upload"; text: qsTr("Показать ему мой экран")
                        onClicked: RemoteDesktop.offerShare(page.deviceId)
                    }
                    // этот компьютер сам предлагает устройству стать вторым экраном (устройство получит запрос и займёт весь экран)
                    Common.GlassButton {
                        visible: RemoteDesktop.secondBackend !== ""
                        enabled: page.can("remoteDesktop") && Settings.remoteDesktopFeatureEnabled
                        icon: "monitor"; text: qsTr("Сделать его вторым монитором")
                        onClicked: secondDialog.openFor(page.deviceId, "offer")
                    }
                    // наоборот: это устройство просит стать вторым экраном другого
                    Common.GlassButton {
                        enabled: page.can("remoteDesktop"); icon: "monitor"; text: qsTr("Стать его вторым монитором")
                        onClicked: secondDialog.openFor(page.deviceId, "request")
                    }
                }
                // почему кнопка серая: чего не хватает и где это включить
                Text {
                    visible: text !== ""
                    width: parent.width; wrapMode: Text.WordWrap
                    text: {
                        var h = [];
                        if (!page.can("remoteDesktop")) h.push(qsTr("Экран недоступен: на другом устройстве включите показ экрана в его настройках и разрешение «Демонстрация экрана» для этого устройства."));
                        else if (RemoteDesktop.captureAvailable && !Settings.remoteDesktopFeatureEnabled) h.push(qsTr("«Показать ему мой экран» и «Сделать его вторым монитором» включаются в настройках здесь («Удалённый доступ» / «Показ экрана»)."));
                        // «Стать его вторым монитором» работает, только если у другого компьютера есть где создать монитор (Hyprland, driftwm); старые версии Orbit это не сообщают
                        else if (!page.can("secondScreen")) h.push(qsTr("«Стать его вторым монитором»: у другого устройства не нашлось Hyprland или driftwm (или у него старая версия Orbit, ниже 0.2.2). Второй монитор можно создать только на компьютере с Hyprland; обратное направление — кнопка «Сделать его вторым монитором»."));
                        if (!page.can("share") || !page.can("clipboard")) h.push(qsTr("Серые кнопки: другое устройство не разрешило функцию — разрешения на его странице этого устройства."));
                        return h.join("\n");
                    }
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
            }

            Section { title: qsTr("Передачи"); visible: page.myTransfers.length > 0
                Repeater {
                    model: page.myTransfers
                    Item {
                        id: tr
                        required property var modelData
                        readonly property bool live: modelData.state === "active" || modelData.state === "offered"
                        width: parent.width; height: 52
                        Common.Icon { id: tri; anchors { left: parent.left; top: parent.top; topMargin: 4 } name: tr.modelData.outgoing ? "upload" : "download"; size: 18; color: tr.modelData.state === "failed" ? Theme.danger : Theme.accent }
                        Text { anchors { left: tri.right; leftMargin: 10; right: trb.left; rightMargin: 8; top: parent.top } elide: Text.ElideMiddle; text: tr.modelData.name; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                        Text {
                            anchors { left: tri.right; leftMargin: 10; right: trb.left; rightMargin: 8; top: parent.top; topMargin: 18 }
                            elide: Text.ElideRight
                            text: ({ offered: qsTr("ждём ответа…"), active: page.fmtSize(tr.modelData.done) + qsTr(" из ") + page.fmtSize(tr.modelData.size), done: page.fmtSize(tr.modelData.size) + (tr.modelData.path ? " · " + tr.modelData.path : qsTr(" · отправлено")),
                                     failed: qsTr("не удалось"), declined: qsTr("отклонено"), cancelled: qsTr("отменено") })[tr.modelData.state] || ""
                            color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                        }
                        Rectangle {
                            visible: tr.live
                            anchors { left: tri.right; leftMargin: 10; right: trb.left; rightMargin: 8; bottom: parent.bottom; bottomMargin: 2 }
                            height: 4; radius: 2; color: Theme.surfaceHigh
                            Rectangle { width: parent.width * (tr.modelData.size > 0 ? tr.modelData.done / tr.modelData.size : 0); height: parent.height; radius: 2; color: Theme.accent }
                        }
                        Common.GlassButton { id: trb; visible: tr.live; anchors { right: parent.right; verticalCenter: parent.verticalCenter } icon: "close"; text: ""; onClicked: Share.cancel(tr.modelData.id) }
                    }
                }
                Common.GlassButton { text: qsTr("Очистить список"); onClicked: Share.clearFinished() }
            }

            Section { title: qsTr("Медиа"); visible: page.online && page.confirmed
                Text {
                    width: parent.width; elide: Text.ElideRight
                    text: page.trackTitle !== "" ? page.trackTitle + (page.trackArtist !== "" ? " — " + page.trackArtist : "") : qsTr("Ничего не играет")
                    color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13
                }
                Row {
                    spacing: 8
                    enabled: page.can("media")
                    Common.GlassButton { icon: "prev"; text: ""; onClicked: Media.sendCommand(page.deviceId, "previous") }
                    Common.GlassButton { icon: page.trackPlaying ? "pause" : "play"; text: ""; primary: true; onClicked: Media.sendCommand(page.deviceId, "playpause") }
                    Common.GlassButton { icon: "next"; text: ""; onClicked: Media.sendCommand(page.deviceId, "next") }
                }
                Text {
                    visible: !page.can("media") || (page.trackTitle === "" && !page.trackPlaying)
                    width: parent.width; wrapMode: Text.WordWrap
                    text: !page.can("media") ? qsTr("Управление плеером выключено на другом устройстве.") : qsTr("Запустите плеер (с MPRIS) на другом устройстве — здесь появится трек.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
            }

            Section { title: qsTr("Быстрые команды"); visible: page.online && page.confirmed
                Flow {
                    width: parent.width; spacing: 8
                    Repeater {
                        model: page.remoteCommands
                        Common.GlassButton {
                            required property var modelData
                            icon: modelData.icon; text: modelData.name
                            onClicked: RunCommand.request(page.deviceId, modelData.id)
                        }
                    }
                }
                Text {
                    visible: page.remoteCommands.length === 0
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Здесь появятся быстрые команды другого устройства (их список ведёт оно само: «Настройки → Мои команды»). ") +
                          qsTr("Если пусто, на ТОМ устройстве на странице этого устройства проверьте разрешение «Запуск команд».")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
            }

            // доверие: как часто это устройство должно спрашивать вашего согласия
            Section { title: qsTr("Доверие"); visible: page.confirmed
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: (page.trusted && page.trusted.trust === 2) ? qsTr("Полное доверие: файлы, показ экрана и управление мышью и клавиатурой — без вопросов. Красная полоса при управлении остаётся.")
                          : (page.trusted && page.trusted.trust === 1) ? qsTr("Доверенное устройство: файлы и показ экрана — без вопросов. Управление мышью и клавиатурой по-прежнему спрашивает.")
                          : qsTr("Каждый файл, показ экрана и управление — только с вашего согласия.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Flow {
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [{ l: 0, t: qsTr("Спрашивать") }, { l: 1, t: qsTr("Доверенное") }, { l: 2, t: qsTr("Полное доверие") }]
                        Common.GlassButton {
                            required property var modelData
                            text: modelData.t
                            primary: (page.trusted ? page.trusted.trust : 0) === modelData.l
                            onClicked: TrustStoreModel.setTrustLevel(page.deviceId, modelData.l)
                        }
                    }
                }
            }

            Section { title: qsTr("Что разрешено этому устройству у вас")
                Repeater {
                    model: [
                        { id: "share", t: qsTr("Файлы"), s: qsTr("присылать вам файлы (каждый — с вашего согласия)") },
                        { id: "clipboard", t: qsTr("Буфер обмена"), s: qsTr("принимать текст в ваш буфер") },
                        { id: "notifications", t: qsTr("Уведомления"), s: qsTr("показывать его уведомления у вас") },
                        { id: "battery", t: qsTr("Заряд батареи"), s: qsTr("видеть заряд этого компьютера") },
                        { id: "find", t: qsTr("Найти устройство"), s: qsTr("заставить этот компьютер сигналить") },
                        { id: "media", t: qsTr("Управление плеером"), s: qsTr("пауза, вперёд, назад") },
                        { id: "runcommand", t: qsTr("Запуск команд"), s: qsTr("только из вашего списка в настройках") },
                        { id: "remoteDesktop", t: qsTr("Демонстрация экрана"), s: qsTr("каждый раз с вашего подтверждения") },
                        { id: "audio", t: qsTr("Звук"), s: qsTr("отдавать этому устройству звук этого компьютера при показе экрана") }
                    ]
                    Common.GlassSwitch {
                        required property var modelData
                        width: parent.width
                        label: modelData.t; sub: modelData.s
                        checked: page.trusted ? page.trusted.plugins.indexOf(modelData.id) >= 0 : false
                        onToggled: v => TrustStoreModel.setPluginAllowed(page.deviceId, modelData.id, v)
                    }
                }
            }

            Flow {
                width: parent.width; spacing: 8
                Common.GlassButton { text: qsTr("Отключиться"); onClicked: PeerManagerModel.disconnectDevice(page.deviceId) }
                Common.GlassButton { text: qsTr("Забыть устройство"); danger: true; onClicked: { PeerManagerModel.forgetDevice(page.deviceId); page.back(); } }
            }
        }
    }

    SecondScreenDialog { id: secondDialog; onStarted: id => page.openScreen(id) }

    // блок с заголовком; содержимое — в колонку внутри карточки
    component Section: Rectangle {
        id: sec
        property string title: ""
        default property alias items: col.data
        width: parent.width
        implicitHeight: head.implicitHeight + col.implicitHeight + 42
        radius: Theme.radius; color: Theme.surface
        border.width: 1; border.color: Theme.line
        Text {
            id: head
            x: 16; y: 14
            text: sec.title; color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11; font.weight: Font.DemiBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 0.6
        }
        Column { id: col; x: 16; y: head.y + head.implicitHeight + 10; width: parent.width - 32; spacing: 8 }
    }
}
