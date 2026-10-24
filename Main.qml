import QtQuick
import QtQuick.Controls.Basic
import "ui"
import "ui/common" as Common

// Корневое окно: стек страниц (список → устройство → экран / настройки), полосы согласия сверху,
// запросы сопряжения, всплывающие запросы от устройств (IncomingRequests) и уведомление внизу
ApplicationWindow {
    id: win
    width: 520; height: 760
    minimumWidth: 400; minimumHeight: 520
    visible: !StartHidden
    Component.onCompleted: if (!StartHidden && Settings.startMinimized) showMinimized()
    title: "NameLess Orbit"
    color: Theme.base
    background: Rectangle {
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(Theme.base, 1.0) }
            GradientStop { position: 1; color: "#0c1030" }
        }
    }

    // кнопка «Назад» на телефоне возвращает на страницу выше, а закрывает программу только на главной
    // «Назад»: на вложенной странице — шаг назад; на главной при работе в фоне — свернуть (закрытие окна останавливало Orbit целиком)
    onClosing: close => {
        if (IsMobile && stack.depth > 1) { close.accepted = false; stack.pop(); }
        else if (IsMobile && Settings.keepRunning) { close.accepted = false; Settings.moveToBack(); }
    }

    // Android рисует под строкой состояния, вырезом и жестовой панелью — свободную область сообщает система
    Item { id: safe; anchors.fill: parent }
    property bool pageFullscreen: false           // страница просмотра открыта на весь экран: ни вырезов, ни полос системы
    readonly property bool edgeToEdge: IsMobile && (RemoteDesktop.secondScreen || pageFullscreen)
    readonly property real safeTop: safe.SafeArea.margins.top
    readonly property real safeBottom: safe.SafeArea.margins.bottom
    readonly property real safeLeft: safe.SafeArea.margins.left
    readonly property real safeRight: safe.SafeArea.margins.right

    // полоса сверху: пока идёт демонстрация своего экрана или чужое управление — забыть об этом нельзя
    Common.ConsentBanner {
        id: consent
        z: 10
        visible: text !== "" && !win.edgeToEdge
        anchors { top: parent.top; topMargin: win.safeTop; left: parent.left; right: parent.right }
        readonly property bool controlled: RemoteDesktop.inputArmedForDeviceId !== ""
        readonly property var info: RemoteDesktop.shareInfo
        // что показано в полосе: управление — важнее всего; иначе кому и что показывается, с живыми цифрами; пока идёт системное окно разрешения — так и сказано
        text: {
            if (controlled) return IsMobile ? qsTr("Ваш телефон сейчас управляется с другого устройства") : qsTr("Ваш компьютер сейчас управляется с другого устройства");
            if (!info.active) return "";
            const live = RemoteDesktop.sendActualFps > 0 ? " · " + RemoteDesktop.sendActualFps + qsTr(" к/с") : "";
            if (info.waiting) return qsTr("Показ «") + info.what + qsTr("» для «") + info.peer + qsTr("»: ждём разрешения системы…");
            let t = (info.second ? qsTr("Второй экран для «") : qsTr("Показываю ") + info.what + " → «") + info.peer + "»" + (info.second ? ": " + info.what : "") + live;
            if (!IsMobile) t += (RemoteDesktop.sendRtt > 0 ? " · " + RemoteDesktop.sendRtt + qsTr(" мс") : "") + (RemoteDesktop.sendFormat !== "" ? " · " + RemoteDesktop.sendFormat : "")
                              + qsTr(" · ступень ") + RemoteDesktop.ladderLevel + "/" + RemoteDesktop.ladderSteps + (RemoteDesktop.peerDecodeMs > 0 ? qsTr(" · у зрителя ") + RemoteDesktop.peerDecodeMs + qsTr(" мс/кадр") : "");
            return t;
        }
        danger: controlled
        actionLabel: controlled ? qsTr("Отключить управление") : (info.waiting ? qsTr("Отмена") : (info.second ? qsTr("Закрыть второй экран") : qsTr("Остановить показ")))
        onActionClicked: controlled ? RemoteDesktop.armInput(RemoteDesktop.inputArmedForDeviceId, false) : RemoteDesktop.stopSharing()
    }

    StackView {
        id: stack
        // второй экран закрывает весь телефон: ни выреза, ни полос системы, ни баннера сверху не оставляется
        anchors { top: win.edgeToEdge ? parent.top : consent.bottom; left: parent.left; right: parent.right; bottom: parent.bottom
                  leftMargin: win.edgeToEdge ? 0 : win.safeLeft; rightMargin: win.edgeToEdge ? 0 : win.safeRight; bottomMargin: win.edgeToEdge ? 0 : win.safeBottom }
        initialItem: listPage
        // просмотр чужого экрана закрывается вместе со страницей
        onCurrentItemChanged: if (RemoteDesktop.viewingDeviceId !== "" && !(currentItem && currentItem.isScreen === true) && !screenWin.visible) RemoteDesktop.stopViewing()
    }

    Component {
        id: listPage
        DeviceListPage {
            onOpenDevice: id => stack.push(detailPage, { deviceId: id })
            onOpenSettings: stack.push(settingsPage)
        }
    }
    Component {
        id: detailPage
        DeviceDetailPage {
            onBack: stack.pop()
            onOpenScreen: id => win.showScreen(id)
        }
    }
    // страница просмотра: одна на весь стек. Открывается и сама, когда просмотр начался без вопроса (доверенное устройство, предложенный второй экран),
    // иначе страница списка закрывала бы просмотр сразу после начала (см. onCurrentItemChanged у stack)
    function showScreen(id) {
        if (screenWin.visible) return;
        // второй экран на компьютере — отдельное окно на весь экран: главное окно остаётся как было и не теряет полный экран при выводе вперёд
        if (!IsMobile && RemoteDesktop.secondScreen) {
            // окно — на тот экран, по размеру которого запрошен второй экран: иначе картинка вписывалась в чужой монитор с полями
            for (let i = 0; i < Qt.application.screens.length; ++i)
                if (Qt.application.screens[i].name === RemoteDesktop.secondScreenName) screenWin.screen = Qt.application.screens[i];
            // полный экран задаётся до показа окна (showFullScreen): запрос после show() композитор иногда пропускал — окно оставалось
            // обычным с полями по краям; повтор через мгновение — на случай, если окно сначала встало плиткой
            detachScreen(id, true); fullTimer.restart(); return;
        }
        if (stack.currentItem && stack.currentItem.isScreen === true) return;
        stack.push(screenPage, { deviceId: id });
    }
    Connections {
        target: RemoteDesktop
        function onViewingChanged() { if (RemoteDesktop.viewingDeviceId !== "") win.showScreen(RemoteDesktop.viewingDeviceId); }
    }
    Component { id: screenPage; RemoteScreenPage { onBack: stack.pop(); onDetach: win.detachScreen(deviceId) } }

    // отдельное окно с демонстрацией: можно держать рядом с работой, менять размер и разворачивать на весь экран
    function detachScreen(id, full) {
        screenWin.deviceId = id;
        if (full === true) screenWin.showFullScreen(); else screenWin.show();
        screenWin.raise();
        screenWin.requestActivate();
        if (stack.currentItem && stack.currentItem.isScreen === true) stack.pop();
    }
    Timer { id: fullTimer; interval: 350; onTriggered: if (screenWin.visible && screenWin.visibility !== Window.FullScreen) screenWin.showFullScreen() }
    Window {
        id: screenWin
        property string deviceId: ""
        width: 1100; height: 680
        minimumWidth: 320; minimumHeight: 240
        visible: false
        title: qsTr("NameLess Orbit — экран устройства")
        color: Theme.base
        onClosing: RemoteDesktop.stopViewing()
        RemoteScreenPage {
            anchors.fill: parent
            deviceId: screenWin.deviceId
            inWindow: true
            onBack: screenWin.close()
        }
    }
    Component { id: settingsPage; SettingsPage { onBack: stack.pop() } }

    IncomingRequests { id: incoming; anchors.fill: parent; onNotify: text => toast.show(text); onOpenScreen: id => win.showScreen(id) }
    Common.Toast { id: toast; z: 20 }

    // устройство вернулось под другим ключом — либо переустановка, либо подмена; подключение не принято
    Connections {
        target: PeerManagerModel
        function onKeyMismatch(deviceId, name) { toast.show(qsTr("Ключ устройства «") + name + qsTr("» изменился — подключение отклонено")); }
        function onDeviceConnected(deviceId, name) { toast.show(qsTr("Подключено: ") + name); }
        function onPairingDeclined(deviceId, name) { toast.show(qsTr("Сопряжение с «") + name + qsTr("» не состоялось")); }
    }
    Connections {
        target: FindDevice
        function onDeviceFound(deviceId, name) { toast.show(name + qsTr(": устройство нашлось")); }
    }
    Connections {
        target: Share
        function onFileReceived(deviceName, fileName, savedAs) { toast.show(qsTr("Получен файл «") + fileName + "»"); }
        function onFileSent(deviceName, fileName) { toast.show(qsTr("Отправлен «") + fileName + "»"); }
        function onTransferFailed(fileName, reason) { toast.show(qsTr("Файл «") + fileName + "»: " + reason); }
    }
    Connections {
        target: Clipboard
        function onTextReceived(deviceId, fromName) { toast.show(qsTr("Буфер обмена получен от ") + fromName); }
    }
    Connections {
        target: Notifications
        function onNotificationReceived(deviceId, fromName, title, body, appName) { if ((appName === "Claude" || appName === "Codex") && !Settings.receiveReplies) return; toast.show(fromName + ": " + title); }
    }
    // просмотр начат не со страницы устройства (например, из Nameless UI по D-Bus) — страница просмотра открывается сама
    Connections {
        target: RemoteDesktop
        property bool wasViewing: false
        function onViewingChanged() {
            const now = RemoteDesktop.viewingDeviceId !== "";
            // просмотр закончился (показ остановили на любой стороне, отказ, обрыв): страница или окно просмотра закрываются, возвращаясь назад
            if (wasViewing && !now) {
                if (screenWin.visible) screenWin.close();
                else if (stack.currentItem && stack.currentItem.isScreen === true) stack.pop();
            }
            wasViewing = now;
            Qt.callLater(function () {
                if (RemoteDesktop.viewingDeviceId !== "" && !(stack.currentItem && stack.currentItem.isScreen === true) && !screenWin.visible)
                    stack.push(screenPage, { deviceId: RemoteDesktop.viewingDeviceId });
            });
        }
    }
    Connections {
        target: RemoteDesktop
        function onErrorOccurred(message) { toast.show(message); }
        function onShareDeclinedByPeer() { toast.show(qsTr("Устройство отклонило демонстрацию")); }
    }

    // первый запуск: выбор языка (потом он меняется в настройках, блок «Язык»)
    Popup {
        id: langDialog
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: parent
        width: Math.min(360, parent.width - 32)
        padding: 22
        visible: Settings.language === ""
        background: Rectangle { radius: Theme.radius; color: Theme.surfaceHigh; border.width: 1.5; border.color: Theme.accent }
        Column {
            width: parent.width
            spacing: 14
            Common.Icon { anchors.horizontalCenter: parent.horizontalCenter; name: "orbit"; size: 44; color: Theme.accent }
            Text {
                width: parent.width; horizontalAlignment: Text.AlignHCenter
                text: "Выберите язык · Choose language"
                color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 17; font.weight: Font.Bold; wrapMode: Text.WordWrap
            }
            Common.GlassButton { width: parent.width; text: "Русский"; primary: true; onClicked: Settings.language = "ru" }
            Common.GlassButton { width: parent.width; text: "English"; primary: true; onClicked: Settings.language = "en" }
            Text {
                width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                text: "Можно изменить позже в настройках · You can change this later in settings"
                color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
            }
        }
    }
}
