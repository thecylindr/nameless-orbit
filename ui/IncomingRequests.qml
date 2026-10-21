import QtQuick
import QtQuick.Controls.Basic
import "common" as Common

// Всё, что приходит от других устройств и требует ответа человека: просьба показать экран, просьба об управлении,
// «точно выполнить команду?» (5 с) и «Найти устройство». Сам ничего не принимает — только показывает и передаёт ответ
Item {
    id: root
    signal notify(string text)
    signal openScreen(string id)       // человек согласился смотреть экран — Main открывает страницу просмотра

    function fmtSize(b) {
        if (b >= 1073741824) return (b / 1073741824).toFixed(1) + qsTr(" ГБ");
        if (b >= 1048576) return (b / 1048576).toFixed(1) + qsTr(" МБ");
        if (b >= 1024) return Math.round(b / 1024) + qsTr(" КБ");
        return b + qsTr(" Б");
    }

    // очередь вопросов: диалог показывает по одному. Одинаковый вопрос (тот же key) второй раз не добавляется — ни в очередь,
    // ни поверх уже показанного: повторные запросы от одного устройства не плодят окна
    property var queue: []
    property var current: null
    property string pick: ""            // выбранный вариант (экран для показа)
    function ask(item) {
        if (item.key) {
            if (current !== null && current.key === item.key) return;
            for (let i = 0; i < queue.length; ++i) if (queue[i].key === item.key) return;
        }
        queue = queue.concat([item]);
        if (current === null) next();
    }
    // запрос отозван (устройство отключилось, отменило) — убрать его из очереди и закрыть окно, если оно сейчас о нём
    function withdraw(key) {
        queue = queue.filter(q => q.key !== key);
        if (current !== null && current.key === key) { timeout.running = false; next(); }
    }
    function next() {
        if (queue.length === 0) { current = null; dlg.close(); return; }
        current = queue[0]; queue = queue.slice(1);
        const ch = current.choices || [];
        pick = ch.length > 0 ? ch[0].value : "";
        timeout.stop();
        if (current.timeout === true) { bar.value = 1; timeout.start(); }
        dlg.open();
    }
    function answer(yes) {
        if (current === null) return;
        timeout.stop();
        const c = current, p = pick;
        current = null;                 // сначала сбросить: обработчик может сразу задать следующий вопрос
        if (yes) c.yes(p); else c.no();
        next();
    }

    Connections {
        target: RemoteDesktop
        function onShareRequested(id, name) {
            const scr = RemoteDesktop.availableScreens();
            const ch = scr.map(s => ({ value: s.name, label: s.label }));
            root.ask({ key: "share:" + id, icon: "monitor", title: qsTr("Показать экран?"), device: name,
                       text: qsTr("просит смотреть ваш экран. Выберите, что показывать."), choices: ch.length > 1 ? ch : [],
                       yesText: qsTr("Показать"), noText: qsTr("Отклонить"),
                       yes: function (screen) { RemoteDesktop.acceptShareRequest(id, screen); }, no: function () { RemoteDesktop.declineShareRequest(id); } });
        }
        // человек сам решил показать экран этому устройству: сначала что показывать (экран или окно программы), потом предложение уходит
        function onSourceChoiceNeeded(id, name) {
            const ch = RemoteDesktop.availableScreens().map(s => ({ value: s.name, label: s.label }));
            root.ask({ key: "source:" + id, icon: "monitor", title: qsTr("Что показать?"), device: name,
                       text: qsTr("Выберите экран или окно программы. Устройство увидит только это."), choices: ch,
                       yesText: qsTr("Показать"), noText: qsTr("Отмена"),
                       yes: function (src) { RemoteDesktop.offerShare(id, src); }, no: function () { } });
        }
        // другое устройство хочет стать вторым экраном этого компьютера (Hyprland — монитор создаётся сам; driftwm, бета — выбирается окно)
        function onSecondScreenRequested(id, name, desc, windows) {
            const beta = RemoteDesktop.secondBeta;
            // Hyprland: выбирается монитор, рядом с которым встанет новый (первым — тот, что задан в настройках); driftwm: окно приложения
            const hypr = RemoteDesktop.secondBackend === "Hyprland";
            let ch = windows.map(w => ({ value: String(w.id), label: w.label }));
            if (hypr) {
                const mons = RemoteDesktop.secondMonitors.slice();
                const pref = Settings.secondAnchor;
                if (pref !== "" && mons.indexOf(pref) > 0) { mons.splice(mons.indexOf(pref), 1); mons.unshift(pref); }
                ch = mons.map(n => ({ value: n, label: qsTr("Рядом с монитором ") + n }));
            }
            root.ask({ key: "second:" + id, icon: "monitor", title: qsTr("Второй экран?"), device: name, beta: beta,
                       text: qsTr("хочет использовать этот компьютер как источник второго экрана: ") + desc + qsTr(". Пока он включён, касания и клавиатура того устройства управляют этим компьютером.") + (hypr && ch.length > 0 ? qsTr(" Выберите, где поставить монитор:") : (beta && ch.length > 0 ? qsTr(" Выберите окно:") : "")),
                       choices: ch, yesText: qsTr("Разрешить"), noText: qsTr("Отклонить"),
                       yes: function (win) { RemoteDesktop.acceptSecondScreen(id, win); },
                       no: function () { RemoteDesktop.declineSecondScreen(id, qsTr("отклонено владельцем компьютера")); } });
        }
        // компьютер предлагает этому устройству стать его вторым экраном: после согласия устройство целиком отдаётся под экран (на весь экран, со стрелкой выхода)
        function onSecondScreenOffered(id, name, desc) {
            root.ask({ key: "secondoffer:" + id, icon: "monitor", title: qsTr("Стать вторым экраном?"), device: name,
                       text: qsTr("хочет использовать это устройство как второй монитор: ") + desc + qsTr(". Экран целиком займёт изображение компьютера; касания будут управлять им. Выйти можно стрелкой в углу."),
                       yesText: qsTr("Стать экраном"), noText: qsTr("Отклонить"),
                       yes: function () { RemoteDesktop.acceptSecondScreenOffer(id); }, no: function () { RemoteDesktop.declineSecondScreenOffer(id); } });
        }
        function onShareOffered(id, name) {
            root.ask({ key: "offer:" + id, icon: "monitor", title: qsTr("Посмотреть экран?"), device: name, text: qsTr("хочет показать вам свой экран."), yesText: qsTr("Смотреть"), noText: qsTr("Отклонить"),
                       yes: function () { RemoteDesktop.requestShare(id); root.openScreen(id); }, no: function () { RemoteDesktop.declineShareRequest(id); } });
        }
        function onControlRequested(id, name) {
            root.ask({ key: "control:" + id, icon: "cursor", title: qsTr("Разрешить управление?"), device: name,
                       text: qsTr("просит управлять мышью и клавиатурой этого компьютера. Пока управление включено, сверху видна красная полоса."), yesText: qsTr("Разрешить"), noText: qsTr("Отклонить"),
                       yes: function () { RemoteDesktop.armInput(id, true); }, no: function () { RemoteDesktop.armInput(id, false); } });
        }
    }
    Connections {
        target: RunCommand
        function onConfirmationNeeded(requestId, name) {
            root.ask({ key: "cmd:" + requestId, icon: "bolt", title: qsTr("Выполнить команду?"), device: "",
                       text: qsTr("Другое устройство просит запустить «") + name + qsTr("». Без ответа через 5 секунд запрос отменится."), yesText: qsTr("Выполнить"), noText: qsTr("Отменить"), timeout: true,
                       yes: function () { RunCommand.confirmPending(requestId, true); }, no: function () { RunCommand.confirmPending(requestId, false); } });
        }
    }
    Connections {
        target: Share
        function onOfferReceived(id, deviceName, fileName, size) {
            root.ask({ key: "file:" + id, icon: "download", title: qsTr("Принять файл?"), device: deviceName,
                       text: qsTr("отправляет «") + fileName + "» (" + root.fmtSize(size) + ").", yesText: qsTr("Принять"), noText: qsTr("Отклонить"),
                       yes: function () { Share.accept(id); }, no: function () { Share.decline(id); } });
        }
    }
    Timer { id: timeout; interval: 5000; onTriggered: root.answer(false) }

    Popup {
        id: dlg
        parent: Overlay.overlay
        modal: true
        anchors.centerIn: parent
        width: Math.min(400, root.width - 32)
        padding: 22                     // отступ от рамки до содержимого со всех сторон, кнопки не прижимаются к краю
        closePolicy: Popup.NoAutoClose
        focus: true
        background: Rectangle { radius: Theme.radius; color: Theme.surfaceHigh; border.width: 1.5; border.color: Theme.accent }
        // Popup — не Item, Keys на нём не работают: горячие клавиши через Shortcut, пока диалог открыт
        Shortcut { sequence: "Return"; enabled: dlg.opened; onActivated: root.answer(true) }
        Shortcut { sequence: "Enter"; enabled: dlg.opened; onActivated: root.answer(true) }
        Shortcut { sequence: "Escape"; enabled: dlg.opened; onActivated: root.answer(false) }

        contentItem: Column {
            id: col
            spacing: 16

            Row {
                spacing: 12
                Rectangle {
                    width: 40; height: 40; radius: 20
                    color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.22)
                    Common.Icon { anchors.centerIn: parent; name: root.current && root.current.icon ? root.current.icon : "bolt"; size: 20; color: Theme.accent }
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text { text: root.current ? root.current.title : ""; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 16; font.weight: Font.Bold }
                    Row {
                        spacing: 8
                        visible: root.current !== null && root.current.beta === true
                        // бета-режим: яркая зелёная метка
                        Rectangle {
                            width: betaTxt.implicitWidth + 14; height: 18; radius: 9
                            color: Qt.rgba(0.13, 0.88, 0.42, 0.18); border.width: 1; border.color: "#22e06b"
                            Text { id: betaTxt; anchors.centerIn: parent; text: qsTr("БЕТА"); color: "#22e06b"; font.family: Theme.fontFamily; font.pixelSize: 10; font.weight: Font.Bold; font.letterSpacing: 0.8 }
                        }
                        Text { text: "driftwm"; color: "#22e06b"; font.family: Theme.fontFamily; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
                    }
                    Text {
                        visible: root.current !== null && root.current.device !== ""
                        text: root.current ? root.current.device : ""
                        color: Theme.accent; font.family: Theme.fontFamily; font.pixelSize: 12; font.weight: Font.Medium
                    }
                }
            }
            Text { width: parent.width; wrapMode: Text.WordWrap; text: root.current ? root.current.text : ""; color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 13 }

            // выбор варианта (экран или окно): при длинном списке прокручивается
            Flickable {
                id: choiceView
                visible: root.current !== null && (root.current.choices || []).length > 0
                width: parent.width
                height: Math.min(choiceCol.implicitHeight, 228)
                contentHeight: choiceCol.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Column {
                    id: choiceCol
                    width: parent.width
                    spacing: 6
                    Repeater {
                        model: root.current ? (root.current.choices || []) : []
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool on: root.pick === modelData.value
                            width: choiceCol.width; height: 38; radius: 10
                            color: on ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.20) : Qt.rgba(1, 1, 1, 0.05)
                            border.width: 1; border.color: on ? Theme.accent : Theme.line
                            Row {
                                anchors { left: parent.left; leftMargin: 12; right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                                spacing: 10
                                Rectangle {
                                    width: 14; height: 14; radius: 7; anchors.verticalCenter: parent.verticalCenter
                                    color: "transparent"; border.width: 1.5; border.color: on ? Theme.accent : Theme.subtext
                                    Rectangle { anchors.centerIn: parent; width: 8; height: 8; radius: 4; color: Theme.accent; visible: on }
                                }
                                Text { width: parent.width - 24; elide: Text.ElideRight; text: modelData.label; color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 12 }
                            }
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.pick = modelData.value }
                        }
                    }
                }
            }

            // сколько осталось до автоотмены
            Rectangle {
                id: bar
                property real value: 1
                visible: root.current !== null && root.current.timeout === true
                width: parent.width; height: 3; radius: 2; color: Theme.line
                Rectangle { width: parent.width * bar.value; height: parent.height; radius: 2; color: Theme.accent }
                NumberAnimation on value { running: timeout.running; from: 1; to: 0; duration: timeout.interval }
            }

            Row {
                width: parent.width
                spacing: 10
                Text {
                    visible: root.queue.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - btns.width - 10
                    text: qsTr("Ещё запросов: ") + root.queue.length
                    color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
                }
                Item { visible: root.queue.length === 0; width: parent.width - btns.width - 10; height: 1 }
                Row {
                    id: btns
                    spacing: 10
                    Common.GlassButton { text: root.current ? root.current.noText : ""; onClicked: root.answer(false) }
                    Common.GlassButton { text: root.current ? root.current.yesText : ""; primary: true; onClicked: root.answer(true) }
                }
            }
        }
    }

    // «Найти устройство»: заметная вспышка на всё окно, пока не нажмут «Я здесь»
    property string ringFrom: ""
    Connections {
        target: FindDevice
        function onRingRequested(id, name) { root.ringFrom = name; }
        function onRingStopped() { root.ringFrom = ""; }
    }
    Rectangle {
        anchors.fill: parent
        visible: FindDevice.ringing
        color: pulse ? Theme.accent : Theme.accent2
        property bool pulse: true
        Timer { interval: 500; repeat: true; running: parent.visible; onTriggered: parent.pulse = !parent.pulse }
        Column {
            anchors.centerIn: parent
            spacing: 16
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("Я здесь!"); color: "white"; font.family: Theme.fontFamily; font.pixelSize: 40; font.weight: Font.Bold }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: root.ringFrom + qsTr(" ищет это устройство"); color: "white"; font.family: Theme.fontFamily; font.pixelSize: 14 }
            Common.GlassButton { anchors.horizontalCenter: parent.horizontalCenter; icon: "check"; text: qsTr("Нашёл"); primary: true; onClicked: FindDevice.dismiss() }
        }
        MouseArea { anchors.fill: parent; z: -1 }
    }
}
