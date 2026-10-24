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

    property int brightness: -1
    property var media: ({})
    property real now: Date.now()
    // батарея — только у тех, у кого она бывает (у ПК старые версии присылали 0 %)
    readonly property bool hasBattery: battery >= 0 && ["phone", "tablet", "laptop"].indexOf(live ? live.kind : (trusted ? trusted.kind : "")) >= 0
    function refreshBattery() { battery = Battery.hasRemote(deviceId) ? Battery.remotePercent(deviceId) : -1; charging = Battery.remoteCharging(deviceId); brightness = Battery.remoteBrightness(deviceId); }
    function refreshMedia() { media = Media.remoteMedia(deviceId); }
    // позиция трека досчитывается от момента получения, пока играет
    Timer { interval: 1000; repeat: true; running: page.media.playing === true; onTriggered: page.now = Date.now() }
    readonly property real mediaPos: (media.pos || 0) + (media.playing ? Math.max(0, now - (media.at || now)) : 0)
    function fmtTime(ms) { var t = Math.floor(ms / 1000); return Math.floor(t / 60) + ":" + ("0" + t % 60).slice(-2); }
    Component.onCompleted: { refreshBattery(); refreshMedia(); if (confirmed) RunCommand.requestCommands(deviceId); }
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
        function onRemoteMediaChanged(id) { if (id === page.deviceId) page.refreshMedia(); }
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
                text: [page.live ? page.live.model : "", page.live ? page.live.address : ""].filter(function (x) { return x; }).join(" · ")
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

            // состояние устройства: заряд и яркость экрана полосами (заряд — только у телефона, планшета, ноутбука)
            Rectangle {
                visible: page.online && (page.hasBattery || page.brightness >= 0)
                width: parent.width; height: stateCol.implicitHeight + 28; radius: Theme.radius
                color: Theme.surface; border.width: 1; border.color: Theme.line
                Column {
                    id: stateCol
                    x: 16; y: 14; width: parent.width - 32
                    spacing: 10
                    Repeater {
                        model: [{ show: page.hasBattery, icon: "battery", t: qsTr("Заряд"), v: page.battery, extra: page.charging ? qsTr(" · заряжается") : "",
                                  c: page.battery < 20 && !page.charging ? Theme.danger : (page.charging ? Theme.good : Theme.accent) },
                                { show: page.brightness >= 0, icon: "bolt", t: qsTr("Яркость экрана"), v: page.brightness, extra: "", c: Theme.warn }]
                        Column {
                            required property var modelData
                            visible: modelData.show
                            width: stateCol.width; spacing: 6
                            Row {
                                width: parent.width; spacing: 8
                                Common.Icon { name: modelData.icon; size: 16; color: modelData.c }
                                Text { text: modelData.t; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                                Text { text: modelData.v + "%" + modelData.extra; color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.DemiBold }
                            }
                            Rectangle {
                                width: parent.width; height: 8; radius: 4; color: Theme.surfaceHigh
                                Rectangle { width: parent.width * Math.max(0, Math.min(1, modelData.v / 100)); height: parent.height; radius: 4; color: modelData.c; Behavior on width { NumberAnimation { duration: 300 } } }
                            }
                        }
                    }
                }
            }

            // сейчас играет: обложка, трек, приложение, прогресс, крупные кнопки
            Rectangle {
                visible: page.online && page.confirmed && page.can("media") && (page.media.title || "") !== ""
                width: parent.width; height: IsMobile ? 132 : 112; radius: Theme.radius
                color: Theme.surface; border.width: 1; border.color: Theme.line
                Rectangle {
                    id: artBox
                    x: 14; anchors.verticalCenter: parent.verticalCenter
                    width: parent.height - 28; height: width; radius: 12
                    color: Theme.surfaceHigh; clip: true
                    Image {
                        anchors.fill: parent; fillMode: Image.PreserveAspectCrop; asynchronous: true
                        source: page.media.art ? "data:image/jpeg;base64," + page.media.art : (page.media.artUrl || "")
                        visible: status === Image.Ready
                    }
                    Common.Icon { anchors.centerIn: parent; visible: !page.media.art && !page.media.artUrl; name: "speaker"; size: 30; color: Theme.subtext }
                }
                Column {
                    anchors { left: artBox.right; leftMargin: 14; right: parent.right; rightMargin: 14; top: artBox.top }
                    spacing: 3
                    Text { width: parent.width; elide: Text.ElideRight; text: page.media.title || ""; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.DemiBold }
                    Text { width: parent.width; elide: Text.ElideRight; text: [page.media.artist, page.media.app].filter(function (x) { return x; }).join(" · "); color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12 }
                    Item {
                        visible: (page.media.len || 0) > 0
                        width: parent.width; height: 16
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter; width: parent.width - 84; height: 4; radius: 2; color: Theme.surfaceHigh
                            Rectangle { width: parent.width * Math.min(1, page.mediaPos / Math.max(1, page.media.len)); height: parent.height; radius: 2; color: Theme.accent }
                        }
                        Text { anchors { right: parent.right; verticalCenter: parent.verticalCenter } text: page.fmtTime(Math.min(page.mediaPos, page.media.len)) + " / " + page.fmtTime(page.media.len); color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 10 }
                    }
                }
                Row {
                    anchors { left: artBox.right; leftMargin: 10; bottom: artBox.bottom }
                    spacing: 6
                    Common.GlassButton { icon: "prev"; text: ""; onClicked: Media.sendCommand(page.deviceId, "previous") }
                    Common.GlassButton { icon: page.media.playing ? "pause" : "play"; text: ""; primary: true; onClicked: Media.sendCommand(page.deviceId, "playpause") }
                    Common.GlassButton { icon: "next"; text: ""; onClicked: Media.sendCommand(page.deviceId, "next") }
                }
            }

            // действия — плитками с подписями: крупные под палец на телефоне; серая плитка объясняется строкой ниже
            Section { title: qsTr("Действия"); visible: page.online && page.confirmed
                Grid {
                    id: acts
                    width: parent.width
                    columns: IsMobile ? 3 : 4
                    spacing: 8
                    readonly property real cw: (width - spacing * (columns - 1)) / columns
                    Repeater {
                        model: [
                            { icon: "send", t: qsTr("Файл"), on: page.can("share"), f: function () { fileDialog.open(); } },
                            { icon: "clipboard", t: qsTr("Буфер"), on: page.can("clipboard"), f: function () { Clipboard.sendClipboardTo(page.deviceId); } },
                            { icon: "speaker", t: qsTr("Найти"), on: page.can("find"), f: function () { FindDevice.ring(page.deviceId); } },
                            { icon: "monitor", t: qsTr("Его экран"), on: page.can("remoteDesktop"), f: function () { RemoteDesktop.requestShare(page.deviceId); page.openScreen(page.deviceId); } },
                            { icon: "upload", t: qsTr("Мой экран"), show: RemoteDesktop.captureAvailable, on: page.can("remoteDesktop") && Settings.remoteDesktopFeatureEnabled, f: function () { RemoteDesktop.offerShare(page.deviceId); } },
                            { icon: "monitor", t: qsTr("Сделать вторым экраном"), show: RemoteDesktop.secondBackend !== "", on: page.can("remoteDesktop") && Settings.remoteDesktopFeatureEnabled, f: function () { secondDialog.openFor(page.deviceId, "offer"); } },
                            { icon: "expand", t: qsTr("Стать его 2-м экраном"), on: page.can("remoteDesktop"), f: function () { secondDialog.openFor(page.deviceId, "request"); } },
                            { icon: "bell", t: qsTr("Проверить связь"), on: page.can("notifications"), f: function () { Notifications.sendTo(page.deviceId, "Orbit", qsTr("Проверка связи с ") + Identity.deviceName); } }
                        ].filter(function (x) { return x.show !== false; })
                        Rectangle {
                            id: tile
                            required property var modelData
                            width: acts.cw; height: IsMobile ? 84 : 76; radius: 14
                            opacity: modelData.on ? 1 : 0.4
                            color: tMa.pressed ? Theme.surfaceHigh : (tMa.containsMouse ? Qt.lighter(Theme.surface, 1.3) : Theme.surfaceHigh)
                            Column {
                                anchors.centerIn: parent
                                width: parent.width - 12
                                spacing: 6
                                Common.Icon { anchors.horizontalCenter: parent.horizontalCenter; name: tile.modelData.icon; size: 22; color: Theme.accent }
                                Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; maximumLineCount: 2; text: tile.modelData.t; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 12 }
                            }
                            MouseArea { id: tMa; anchors.fill: parent; enabled: tile.modelData.on; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: tile.modelData.f() }
                        }
                    }
                }
                Common.GlassButton { visible: page.can("find"); icon: "stop"; text: qsTr("Отменить звонок"); onClicked: FindDevice.stop(page.deviceId) }
                // почему плитка серая: чего не хватает и где это включить
                Text {
                    visible: text !== ""
                    width: parent.width; wrapMode: Text.WordWrap
                    text: {
                        var h = [];
                        if (!page.can("remoteDesktop")) h.push(qsTr("Экран недоступен: на другом устройстве включите показ экрана в его настройках и разрешение «Демонстрация экрана» для этого устройства."));
                        else if (RemoteDesktop.captureAvailable && !Settings.remoteDesktopFeatureEnabled) h.push(qsTr("«Мой экран» и «Сделать вторым экраном» включаются в настройках здесь («Удалённый доступ» / «Показ экрана»)."));
                        if (!page.can("share") || !page.can("clipboard")) h.push(qsTr("Серые плитки: другое устройство не разрешило функцию — разрешения на его странице этого устройства."));
                        if (!IsMobile) h.push(qsTr("Файлы можно просто перетащить на это окно."));
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

            Section { title: qsTr("Плеер"); visible: page.online && page.confirmed && (page.media.title || "") === ""
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: !page.can("media") ? qsTr("Управление плеером выключено на другом устройстве.")
                          : qsTr("Ничего не играет. На компьютере нужен плеер с MPRIS; на телефоне — разрешение «Доступ к уведомлениям» для Orbit (Настройки Orbit на телефоне → «Показывать, что играет»).")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Row {
                    spacing: 8
                    enabled: page.can("media")
                    Common.GlassButton { icon: "prev"; text: ""; onClicked: Media.sendCommand(page.deviceId, "previous") }
                    Common.GlassButton { icon: "play"; text: ""; primary: true; onClicked: Media.sendCommand(page.deviceId, "playpause") }
                    Common.GlassButton { icon: "next"; text: ""; onClicked: Media.sendCommand(page.deviceId, "next") }
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
                        { id: "battery", t: qsTr("Заряд и яркость"), s: qsTr("видеть заряд и яркость экрана этого устройства") },
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
