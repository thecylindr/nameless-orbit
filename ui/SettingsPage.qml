import QtQuick
import QtQuick.Controls.Basic
import "common" as Common

// Настройки: это устройство (имя, тип, отпечаток), поведение, удалённый доступ и список своих команд
Page {
    id: page
    signal back
    property string codexSetupMessage: ""
    property bool notifOk: Settings.notificationsAllowed()
    // после возврата из системных настроек состояние перечитывается
    Connections { target: Qt.application; function onStateChanged() { page.notifOk = Settings.notificationsAllowed(); } }

    background: null

    CommandEditorDialog { id: editor }

    header: Item {
        implicitHeight: 64
        Common.GlassButton { x: 16; anchors.verticalCenter: parent.verticalCenter; icon: "back"; text: ""; onClicked: page.back() }
        Text { x: 72; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Настройки"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 18; font.weight: Font.Bold }
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

            Block { title: qsTr("Это устройство")
                Rectangle {
                    width: parent.width; height: 38; radius: 10
                    color: Theme.surface; border.width: 1; border.color: nameField.activeFocus ? Theme.accent : Theme.line
                    TextField {
                        id: nameField
                        anchors.fill: parent; leftPadding: 12; rightPadding: 12
                        text: Identity.deviceName
                        color: Theme.text; background: null; selectByMouse: true
                        onEditingFinished: if (text.trim() !== "") Identity.setDeviceName(text.trim())
                    }
                }
                Flow {
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [["desktop", qsTr("Компьютер")], ["laptop", qsTr("Ноутбук")], ["phone", qsTr("Телефон")], ["tablet", qsTr("Планшет")], ["server", qsTr("Сервер")]]
                        Common.GlassButton {
                            required property var modelData
                            text: modelData[1]; primary: Identity.deviceKind === modelData[0]
                            onClicked: Identity.setDeviceKind(modelData[0])
                        }
                    }
                }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Модель: ") + Identity.deviceModel + qsTr("\nАдрес в сети: ") + (Identity.localAddresses.length > 0 ? Identity.localAddresses.join(", ") : qsTr("нет сети"))
                    color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Text {
                    width: parent.width; wrapMode: Text.WrapAnywhere
                    text: qsTr("Отпечаток: ") + Identity.fingerprint + qsTr("\nКороткий код: ") + Identity.shortCode
                    color: Theme.subtext; font.family: "monospace"; font.pixelSize: 11
                }
            }

            Block { title: qsTr("Язык")
                Text { text: qsTr("Язык интерфейса"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    width: parent.width; spacing: 8
                    Common.GlassButton { text: "Русский"; primary: Settings.language !== "en"; onClicked: Settings.language = "ru" }
                    Common.GlassButton { text: "English"; primary: Settings.language === "en"; onClicked: Settings.language = "en" }
                }
            }

            // сообщения об ответе Claude: «закончил работу», «ждёт ответа»
            Block { title: qsTr("Ответы помощников · Claude и Codex")
                Common.GlassSwitch {
                    visible: !IsMobile
                    width: parent.width
                    label: qsTr("Отправлять на устройства")
                    sub: qsTr("Сообщать подключённым телефонам и компьютерам о завершении работы помощника. Claude подключается через NameLess UI, Codex — переключателем ниже.")
                    checked: Settings.sendReplies; onToggled: v => Settings.sendReplies = v
                }
                Common.GlassSwitch {
                    visible: !IsMobile
                    width: parent.width
                    label: qsTr("Сообщать о завершении ответа Codex")
                    sub: qsTr("При включении подключает Codex notify. Передаёт до 600 символов ответа; запросы и историю не отправляет. Orbit должен работать, отправка сообщений — включена.")
                    checked: Settings.sendCodexReplies
                    onToggled: v => {
                        if (!v) { Settings.sendCodexReplies = false; page.codexSetupMessage = ""; return; }
                        var error = Settings.configureCodexNotifications();
                        Settings.sendCodexReplies = error === "";
                        page.codexSetupMessage = error || qsTr("Codex подключён. Перезапустите активные сессии Codex.");
                    }
                }
                Text {
                    visible: page.codexSetupMessage !== ""
                    width: parent.width; wrapMode: Text.WordWrap
                    text: page.codexSetupMessage
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Common.GlassSwitch {
                    width: parent.width
                    label: qsTr("Принимать от других устройств")
                    sub: qsTr("Показывать такие сообщения здесь; остальные уведомления это не затрагивает")
                    checked: Settings.receiveReplies; onToggled: v => Settings.receiveReplies = v
                }
            }

            Block { title: qsTr("Уведомления"); visible: IsMobile
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: page.notifOk ? qsTr("Уведомления от других устройств приходят в шторку телефона.") : qsTr("Уведомления выключены: сообщения других устройств (Claude, файлы, «позвонить») не появятся в шторке.")
                    color: page.notifOk ? Theme.good : Theme.warn; font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Flow {
                    width: parent.width; spacing: 8
                    Common.GlassButton { visible: !page.notifOk; text: qsTr("Разрешить"); primary: true; onClicked: { Settings.requestNotificationPermission(); Settings.openNotificationSettings(); } }
                    Common.GlassButton { icon: "bell"; text: qsTr("Проверить"); onClicked: Settings.testNotification() }
                    Common.GlassButton { icon: "bolt"; text: qsTr("Фон без ограничений"); onClicked: Settings.openBatterySettings() }
                }
            }

            // формат, в котором этот телефон получает экран ПК: показываются только доступные на нём
            Block { title: qsTr("Формат показа моего экрана"); visible: RemoteDesktop.captureAvailable
                Flow {
                    width: parent.width; spacing: 8
                    Repeater {
                        model: RemoteDesktop.availableFormats()
                        Common.GlassButton {
                            required property var modelData
                            text: modelData.name; primary: Settings.streamFormat === modelData.id
                            enabled: modelData.ok
                            onClicked: Settings.streamFormat = modelData.id
                        }
                    }
                }
                Repeater {
                    model: RemoteDesktop.availableFormats()
                    Text {
                        required property var modelData
                        width: parent.width; wrapMode: Text.WordWrap
                        text: modelData.name + " — " + modelData.note
                        color: Settings.streamFormat === modelData.id ? Theme.text : Theme.subtext
                        font.family: Theme.fontFamily; font.pixelSize: 11
                    }
                }
                Text { width: parent.width; wrapMode: Text.WordWrap; text: qsTr("Так это устройство отдаёт свой экран; смотрящее устройство разжимает то, что умеет. Смена применяется сразу, даже во время показа."); color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11 }
            }

            // второй экран: что этот компьютер умеет отдавать другим устройствам
            Block { title: qsTr("Второй экран"); visible: !IsMobile
                Row {
                    spacing: 10
                    Text { text: RemoteDesktop.secondBackend !== "" ? RemoteDesktop.secondBackend : qsTr("не поддерживается"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold; anchors.verticalCenter: parent.verticalCenter }
                    // бета-режим — яркой зелёной меткой
                    Rectangle {
                        visible: RemoteDesktop.secondBeta
                        width: betaT.implicitWidth + 14; height: 20; radius: 10; anchors.verticalCenter: parent.verticalCenter
                        color: Qt.rgba(0.13, 0.88, 0.42, 0.18); border.width: 1; border.color: "#22e06b"
                        Text { id: betaT; anchors.centerIn: parent; text: qsTr("БЕТА"); color: "#22e06b"; font.family: Theme.fontFamily; font.pixelSize: 11; font.weight: Font.Bold; font.letterSpacing: 0.8 }
                    }
                }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: RemoteDesktop.secondBackend === "Hyprland" ? qsTr("Другое устройство (телефон, второй компьютер) может стать ещё одним монитором этого компьютера: создаётся настоящий виртуальный монитор нужного размера, курсор на него переходит сам, касания работают как мышь. Включается со страницы устройства («Второй экран»); каждый раз спрашивает разрешение, если устройство не в «полном доверии».")
                          : RemoteDesktop.secondBeta ? qsTr("У driftwm нет виртуальных мониторов, поэтому вторым экраном становится окно приложения: оно подгоняется под экран другого устройства, а касания управляют им. Пока бета: возможны неточности положения окна.")
                          : qsTr("Второй экран работает на Hyprland (виртуальный монитор) и driftwm (бета, окно приложения).")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
                }
                // куда ставится второй экран и как крупно рисуется интерфейс на нём (когда устройство не выбрало масштаб само)
                                                Text { visible: RemoteDesktop.secondBackend === "Hyprland"; text: qsTr("Если устройство второго экрана пропало: вернуть экран через"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    visible: RemoteDesktop.secondBackend === "Hyprland"
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [5, 10, 20, 60]
                        Common.GlassButton { required property int modelData; text: modelData + qsTr(" с"); primary: Settings.secondGraceSec === modelData; onClicked: Settings.secondGraceSec = modelData }
                    }
                }
Text { visible: RemoteDesktop.secondBackend === "Hyprland"; text: qsTr("Рабочий стол Hyprland на втором экране"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    visible: RemoteDesktop.secondBackend === "Hyprland"
                    width: parent.width; spacing: 8
                    Common.GlassButton { text: qsTr("Отдельный (рекомендуется)"); primary: Settings.secondWorkspace === ""; onClicked: Settings.secondWorkspace = "" }
                    Repeater {
                        model: 10
                        Common.GlassButton { required property int modelData; text: String(modelData + 1); primary: Settings.secondWorkspace === String(modelData + 1); onClicked: Settings.secondWorkspace = String(modelData + 1) }
                    }
                }
                Text {
                    visible: RemoteDesktop.secondBackend === "Hyprland"
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("«Отдельный» не трогает ваши столы 1–10: Super+цифра переключает только основной монитор. Номер от 1 до 10 отдаёт этот стол второму экрану — переход на него будет уводить курсор туда.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
Text { visible: RemoteDesktop.secondBackend === "Hyprland"; text: qsTr("Рядом с монитором"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    visible: RemoteDesktop.secondBackend === "Hyprland"
                    width: parent.width; spacing: 8
                    Common.GlassButton { text: qsTr("Где курсор"); primary: Settings.secondAnchor === ""; onClicked: Settings.secondAnchor = "" }
                    Repeater {
                        model: RemoteDesktop.secondMonitors
                        Common.GlassButton { required property string modelData; text: modelData; primary: Settings.secondAnchor === modelData; onClicked: Settings.secondAnchor = modelData }
                    }
                }
                Text { visible: RemoteDesktop.secondBackend === "Hyprland"; text: qsTr("Масштаб второго экрана по умолчанию"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    visible: RemoteDesktop.secondBackend === "Hyprland"
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [["auto", qsTr("Авто")], ["1", "100%"], ["1.25", "125%"], ["1.5", "150%"], ["2", "200%"]]
                        Common.GlassButton { required property var modelData; text: modelData[1]; primary: Settings.secondScale === modelData[0]; onClicked: Settings.secondScale = modelData[0] }
                    }
                }
            }

            // KDE Plasma: за каким краем экрана стоит какой компьютер (в Hyprland края задаёт NameLess UI). Край ловит скрипт KWin
            Block { title: qsTr("Переход курсора (KDE Plasma)")
                visible: KWinSession
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Курсор задержался у края экрана — мышь и клавиатура переходят на выбранный компьютер. Scroll Lock — вернуть курсор.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
                Repeater {
                    model: [["left", qsTr("Слева")], ["right", qsTr("Справа")], ["top", qsTr("Сверху")], ["bottom", qsTr("Снизу")]]
                    Column {
                        id: sideRow
                        required property var modelData
                        width: parent.width; spacing: 6
                        function pick(dev) { const m = Object.assign({}, Settings.crossLinks); if (dev === "") delete m[sideRow.modelData[0]]; else m[sideRow.modelData[0]] = dev; Settings.crossLinks = m; }
                        Text { text: sideRow.modelData[1]; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                        Flow {
                            width: parent.width; spacing: 8
                            Common.GlassButton { text: qsTr("Никого"); primary: !Settings.crossLinks[sideRow.modelData[0]]; onClicked: sideRow.pick("") }
                            Repeater {
                                model: PeerManagerModel.connectedDevices.filter(d => (d.remoteCapabilities || []).indexOf("crossing") >= 0)
                                Common.GlassButton { required property var modelData; text: modelData.name; primary: Settings.crossLinks[sideRow.modelData[0]] === modelData.deviceId; onClicked: sideRow.pick(modelData.deviceId) }
                            }
                        }
                    }
                }
            }

            // звук между устройствами: приём (на этом устройстве) и отправка (звук этого компьютера — другому устройству)
            Block { title: qsTr("Звук между устройствами")
                Text { text: qsTr("Громкость принимаемого звука"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [25, 50, 75, 100]
                        Common.GlassButton { required property int modelData; text: modelData + "%"; primary: Settings.audioVolume === modelData; onClicked: Settings.audioVolume = modelData }
                    }
                }
                Text { text: qsTr("Задержка приёма (меньше — быстрее, больше — без треска)"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [60, 120, 250, 400]
                        Common.GlassButton { required property int modelData; text: modelData + qsTr(" мс"); primary: Settings.audioLatencyMs === modelData; onClicked: Settings.audioLatencyMs = modelData }
                    }
                }
                Common.GlassSwitch { width: parent.width; label: qsTr("Включать звук вместе со вторым экраном"); checked: Settings.audioAutoSecond; onToggled: v => Settings.audioAutoSecond = v }
                Common.GlassSwitch { width: parent.width; label: qsTr("Включать звук при просмотре чужого экрана"); checked: Settings.audioAutoShare; onToggled: v => Settings.audioAutoShare = v }
                Text { visible: RemoteDesktop.audioSendAvailable; text: qsTr("Что отдавать другим устройствам"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    visible: RemoteDesktop.audioSendAvailable
                    width: parent.width; spacing: 8
                    Common.GlassButton { text: qsTr("Весь звук компьютера"); primary: Settings.audioSource === ""; onClicked: Settings.audioSource = "" }
                    Repeater {
                        model: RemoteDesktop.audioSources
                        Common.GlassButton { required property string modelData; text: modelData.length > 38 ? modelData.substring(0, 36) + "…" : modelData; primary: Settings.audioSource === modelData; onClicked: Settings.audioSource = modelData }
                    }
                }
                Text { visible: RemoteDesktop.audioSendAvailable; text: qsTr("Сжатие звука"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    visible: RemoteDesktop.audioSendAvailable
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [["auto", qsTr("Авто")], ["pcm", qsTr("Без сжатия (≈1,5 Мбит/с)")]].concat(RemoteDesktop.audioOpusAvailable ? [["opus", "Opus"]] : [])
                        Common.GlassButton { required property var modelData; text: modelData[1]; primary: Settings.audioCodec === modelData[0]; onClicked: Settings.audioCodec = modelData[0] }
                    }
                }
                Flow {
                    visible: RemoteDesktop.audioSendAvailable && RemoteDesktop.audioOpusAvailable && Settings.audioCodec !== "pcm"
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [32, 64, 128]
                        Common.GlassButton { required property int modelData; text: "Opus " + modelData + qsTr(" кбит/с"); primary: Settings.audioOpusKbps === modelData; onClicked: Settings.audioOpusKbps = modelData }
                    }
                }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: RemoteDesktop.audioSendAvailable ? qsTr("Звук отдаётся только тем устройствам, у которых на странице устройства включено «Звук». Телефон пока принимает звук, но сам его не передаёт.")
                                                          : qsTr("Это устройство принимает звук компьютера во время показа экрана; отдавать свой звук оно пока не умеет.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
                }
            }

            // что ускоряет показ экрана: живой список технологий и включены ли они прямо сейчас
            Block { title: qsTr("Что ускоряет показ экрана"); visible: RemoteDesktop.captureAvailable
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: RemoteDesktop.sharingActive || RemoteDesktop.viewingDeviceId !== ""
                          ? qsTr("Сейчас: ступень ") + RemoteDesktop.ladderLevel + qsTr(" из ") + RemoteDesktop.ladderSteps + qsTr(" · просит ") + RemoteDesktop.ladderFps + qsTr(" к/с")
                            + (RemoteDesktop.sendFormat !== "" ? " · " + RemoteDesktop.sendFormat : (RemoteDesktop.viewFormat !== "" ? " · " + RemoteDesktop.viewFormat : ""))
                          : qsTr("Показа сейчас нет — ниже, что включится и когда")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Repeater {
                    model: RemoteDesktop.technologies
                    Row {
                        required property var modelData
                        width: parent.width; spacing: 10
                        Rectangle {
                            width: 10; height: 10; radius: 5; y: 5
                            color: modelData.on ? Theme.accent : "transparent"
                            border.width: 1.5; border.color: modelData.on ? Theme.accent : Theme.subtext
                        }
                        Column {
                            width: parent.width - 20; spacing: 1
                            Text { width: parent.width; wrapMode: Text.WordWrap; text: modelData.name; color: modelData.on ? Theme.text : Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.DemiBold }
                            Text { width: parent.width; wrapMode: Text.WordWrap; text: modelData.note; color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11 }
                        }
                    }
                }
            }

            // качество показа экрана: потолки задаёт человек, «авто» опускает ступени под сеть
            Block { title: qsTr("Качество показа экрана"); visible: RemoteDesktop.captureAvailable
                Common.GlassSwitch {
                    width: parent.width
                    label: qsTr("Подстраивать под сеть")
                    sub: qsTr("Авто: если сеть не справляется, частота и качество снижаются сами, но не выше потолков ниже. Выключено — значения неизменны, лишние кадры пропускаются")
                    checked: Settings.streamAuto; onToggled: v => Settings.streamAuto = v
                }
                Flow {
                    width: parent.width; spacing: 12
                    Text { height: 40; verticalAlignment: Text.AlignVCenter; text: qsTr("Кадров в секунду (до)"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                    Common.GlassSpin { from: 5; to: 60; stepSize: 5; suffix: qsTr("к/с"); value: Settings.streamFps; onValueModified: v => Settings.streamFps = v }
                }
                Flow {
                    width: parent.width; spacing: 12
                    Text { height: 40; verticalAlignment: Text.AlignVCenter; text: qsTr("Качество картинки (до)"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                    Common.GlassSpin { from: 30; to: 95; stepSize: 5; suffix: "%"; value: Settings.streamQuality; onValueModified: v => Settings.streamQuality = v }
                }
                Text { visible: !IsMobile; text: qsTr("Кодер видео (показ этого ПК на телефон)"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    visible: !IsMobile
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [["auto", qsTr("Авто")], ["gpu", qsTr("Видеокарта")], ["cpu", qsTr("Процессор")]]
                        Common.GlassButton {
                            required property var modelData
                            text: modelData[1]; primary: Settings.streamEncoder === modelData[0]
                            onClicked: Settings.streamEncoder = modelData[0]
                        }
                    }
                }
                Text {
                    visible: !IsMobile
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Авто и «Видеокарта» — кодирует встроенная видеокарта Intel/AMD: полное разрешение почти без нагрузки на процессор; если не заработает, сам переключится на процессор. «Процессор» — x264: работает везде, но грузит ядра; размер кадра ниже берётся только для него")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
                // у телефона видеокарты в смысле ПК нет: показ его экрана кодирует аппаратный кодер H.264 самого телефона (MediaCodec) — выбирать нечего
                Text {
                    visible: IsMobile
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Кодер: аппаратный H.264 телефона (MediaCodec) — выбор не нужен. Кадры на сам телефон кодирует ПК: его кодер выбирается в настройках Orbit на компьютере.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
                Text { text: qsTr("Ширина кадра"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                Flow {
                    width: parent.width; spacing: 8
                    Repeater {
                        model: [[0, qsTr("По окну зрителя")], [1280, "1280"], [1600, "1600"], [1920, "1920"], [2560, "2560"], [3840, "3840"]]
                        Common.GlassButton {
                            required property var modelData
                            text: modelData[1]; primary: Settings.streamWidth === modelData[0]
                            onClicked: Settings.streamWidth = modelData[0]
                        }
                    }
                }
                Text {
                    visible: RemoteDesktop.sharingActive
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Сейчас идёт показ: ") + RemoteDesktop.sendActualFps + qsTr(" к/с, задержка ") + RemoteDesktop.sendRtt + qsTr(" мс (установлено: ") + RemoteDesktop.sendFps + qsTr(" к/с, качество ") + RemoteDesktop.sendQuality + " %)"
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Изменения применяются сразу, в том числе во время показа.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
            }

            // телефон: что играет — компьютеру (название, обложка, позиция); нужен доступ к уведомлениям, Orbit читает только медиасессии
            Block { title: qsTr("Показывать, что играет"); visible: IsMobile
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: Media.access ? qsTr("Включено: компьютер видит трек, обложку и позицию, и управляет плеером напрямую.")
                                       : qsTr("Чтобы компьютер видел название трека и обложку, разрешите Orbit «Доступ к уведомлениям». Сами уведомления Orbit не читает — только плеер.")
                    color: Media.access ? Theme.good : Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Common.GlassButton { visible: !Media.access; text: qsTr("Разрешить"); primary: true; onClicked: Media.openAccessSettings() }
            }

            Block { title: qsTr("Поведение")
                Common.GlassSwitch { width: parent.width; label: qsTr("Искать устройства при запуске"); checked: Settings.discoveryOnStart; onToggled: v => Settings.discoveryOnStart = v }
                Common.GlassSwitch {
                    width: parent.width
                    label: IsMobile ? qsTr("Работать в фоне") : qsTr("Работать в фоне после закрытия окна")
                    sub: IsMobile ? qsTr("«Назад» сворачивает Orbit, связь не засыпает при свёрнутом приложении") : qsTr("NameLess UI видит устройства и подключает их без окна")
                    checked: Settings.keepRunning; onToggled: v => Settings.keepRunning = v
                }
                Common.GlassSwitch {
                    visible: IsMobile && Settings.keepRunning
                    width: parent.width
                    label: qsTr("Синхронизация при выключенном экране")
                    sub: qsTr("пока есть связь, телефон не засыпает: буфер, уведомления и файлы приходят сразу (немного больше расход батареи)")
                    checked: Settings.backgroundSync; onToggled: v => Settings.backgroundSync = v
                }
                Row {
                    id: battRow
                    // результат системного вопроса виден только после возврата в приложение — перечитывается при каждом показе
                    property bool ok: Settings.batteryUnrestricted()
                    visible: IsMobile && Settings.keepRunning && !ok
                    width: parent.width; spacing: 10
                    Connections { target: Qt.application; function onStateChanged() { battRow.ok = Settings.batteryUnrestricted(); } }
                    Text {
                        width: parent.width - battBtn.width - 10; anchors.verticalCenter: parent.verticalCenter; wrapMode: Text.WordWrap
                        text: qsTr("Система ограничивает Orbit в фоне — связь может пропадать")
                        color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 12
                    }
                    Common.GlassButton { id: battBtn; text: qsTr("Снять ограничение"); primary: true; onClicked: Settings.requestBatteryUnrestricted() }
                }
                Common.GlassSwitch {
                    visible: IsMobile && Settings.keepRunning
                    width: parent.width
                    label: qsTr("Уведомление о работе в фоне")
                    sub: qsTr("постоянная строка в шторке; выключено — Orbit работает так же, но без уведомления")
                    checked: Settings.backgroundNotice; onToggled: v => Settings.backgroundNotice = v
                }
                Common.GlassButton { visible: !IsMobile; text: qsTr("Завершить Orbit"); onClicked: Qt.quit() }
                Common.GlassSwitch { visible: !IsMobile; width: parent.width; label: qsTr("Запускать свёрнутым"); checked: Settings.startMinimized; onToggled: v => Settings.startMinimized = v }
            }

            Block { title: IsMobile ? qsTr("Показ экрана") : qsTr("Удалённый доступ")
                Common.GlassSwitch {
                    width: parent.width
                    label: IsMobile ? qsTr("Разрешить показ экрана телефона") : qsTr("Разрешить демонстрацию экрана и управление")
                    sub: qsTr("Включается сам, когда вы разрешаете показ экрана устройству, и выключается, когда не осталось ни одного такого устройства. Каждый показ всё равно требует вашего подтверждения")
                    checked: Settings.remoteDesktopFeatureEnabled
                    onToggled: v => Settings.remoteDesktopFeatureEnabled = v
                }
                Flow {
                    visible: !IsMobile
                    width: parent.width; spacing: 12
                    Text { height: 40; verticalAlignment: Text.AlignVCenter; text: qsTr("Отключать управление через (мин)"); color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                    Common.GlassSpin { from: 0; to: 180; stepSize: 5; suffix: qsTr("мин"); zeroText: qsTr("без ограничения"); value: Settings.inputSessionTimeoutMin; onValueModified: v => Settings.inputSessionTimeoutMin = v }
                }
                Text {
                    visible: !IsMobile && Settings.inputSessionTimeoutMin === 0
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Без ограничения управление не погаснет само — не забудьте отключить его вручную.")
                    color: Theme.warn; font.family: Theme.fontFamily; font.pixelSize: 11
                }
                Text {
                    visible: !IsMobile && !RemoteDesktop.inputAvailable
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Управление этим компьютером с других устройств здесь недоступно (нужен доступ к /dev/uinput).")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
            }

            Block { title: qsTr("Мои команды"); visible: !IsMobile
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Доверенные устройства запускают эти команды сразу, без подтверждения: список виден им и вам при сопряжении. Другие устройства видят название, но не сам текст.")
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
                Repeater {
                    model: Commands.commandsForQml
                    Item {
                        required property var modelData
                        width: parent.width; height: 40
                        Common.Icon { id: cic; name: modelData.icon; size: 18; color: Theme.subtext; anchors.verticalCenter: parent.verticalCenter }
                        Text { anchors { left: cic.right; leftMargin: 10; right: btns.left; rightMargin: 8; verticalCenter: parent.verticalCenter } elide: Text.ElideRight; text: modelData.name; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 13 }
                        Row {
                            id: btns
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            spacing: 6
                            Common.GlassButton { icon: "edit"; text: ""; onClicked: editor.openFor(modelData.id, modelData.name, modelData.icon, modelData.confirm) }
                            Common.GlassButton { icon: "trash"; text: ""; danger: true; onClicked: Commands.removeCommand(modelData.id) }
                        }
                    }
                }
                Common.GlassButton { icon: "plus"; text: qsTr("Добавить команду"); onClicked: editor.openFor("", "", "", false) }
            }

            Text { text: "NameLess Orbit " + AppVersion; color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11; bottomPadding: 8 }
        }
    }

    // блок настроек с заголовком
    component Block: Rectangle {
        id: blk
        property string title: ""
        default property alias items: col.data
        width: parent.width
        implicitHeight: head.implicitHeight + col.implicitHeight + 42
        radius: Theme.radius; color: Theme.surface
        border.width: 1; border.color: Theme.line
        Text {
            id: head
            x: 16; y: 14
            text: blk.title; color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11; font.weight: Font.DemiBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 0.6
        }
        Column { id: col; x: 16; y: head.y + head.implicitHeight + 10; width: parent.width - 32; spacing: 10 }
    }
}
