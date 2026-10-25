import QtQuick
import QtQuick.Controls.Basic
import QtMultimedia
import "common" as Common

// Просмотр экрана другого устройства. Кадры приходят через image://orbit.
//   Масштаб: щипок двумя пальцами, колесо мыши, кнопки «+ / −», двойное касание. Увеличенный участок запрашивается у показывающего
//   в полном разрешении (он снимает только эту область), а не растягивается из общей картинки — текст остаётся чётким.
//   Перемещение увеличенной картинки: перетаскивание (в режиме просмотра) или двумя пальцами.
//   Ввод («Ввод» в шапке, и владелец экрана должен разрешить): касание = курсор переходит ровно в то место, куда нажали, и щёлкает;
//   долгое касание = правая кнопка; перетаскивание одним пальцем = перетаскивание с зажатой левой кнопкой. Мышь и клавиатура — как обычно
Page {
    id: page
    property string deviceId: ""
    readonly property bool isScreen: true       // Main по нему отличает эту страницу от остальных
    property bool control: false              // человек включил «Ввод»
    readonly property bool live: control && RemoteDesktop.controlState === 2     // и владелец экрана разрешил — только тогда события уходят
    // окошко «картинка в картинке» (Android): вокруг кадра не остаётся ничего лишнего
    readonly property bool pip: IsMobile && Window.window !== null && (Window.window.height < 260 || Window.window.width < 260)
    property bool inWindow: false             // страница живёт в отдельном окне, а не в стеке главного
    property bool keepViewing: false          // страница убрана из стека, но просмотр продолжается в отдельном окне
    // ---- полный экран и поворот ----
    property bool fs: false
    property bool fill: true                   // полный экран на телефоне: картинка занимает весь экран (лишнее по краю срезается); выключено — вписывается целиком
    readonly property bool cover: IsMobile && !pip && !second && fs && fill
    property int rot: 0                        // 0 — по картинке (широкая → альбом, высокая → портрет), 1 — альбом, 2 — портрет, 3 — свободно по датчику
    readonly property var rotNames: [qsTr("Авто"), qsTr("Альбом"), qsTr("Портрет"), qsTr("Датчик")]
    function applyView() {
        if (IsMobile) {
            const o = rot === 0 ? (fs ? (aspect >= 1 ? 1 : 2) : 0) : rot;
            RemoteDesktop.setFullscreenView(fs, o);
        } else if (Window.window) {
            if (fs) { Window.window.visibility = Window.FullScreen; }
            else if (Window.window.visibility === Window.FullScreen) Window.window.visibility = Window.Windowed;
        }
    }
    // «Второй экран»: страница сразу на весь экран, ввод включён (касания = мышь на мониторе другого компьютера), ориентация — как выбрана
    readonly property bool second: RemoteDesktop.secondScreen
    function enterSecond() { control = true; rot = RemoteDesktop.secondOrientation === 2 ? 2 : 1; fs = true; Qt.callLater(function () { view.forceActiveFocus(); }); }
    onSecondChanged: if (second) enterSecond()
    Component.onCompleted: if (second) enterSecond()
    onFsChanged: { applyView(); if (Window.window && Window.window.pageFullscreen !== undefined) Window.window.pageFullscreen = IsMobile && fs && !second; }
    onRotChanged: applyView()
    onAspectChanged: if (IsMobile && rot === 0) applyView()
    signal back
    signal detach                             // «в отдельное окно» (только настольные системы)

    background: null

    // ---- масштаб и сдвиг (координаты окна просмотра) ----
    property real zoom: 1
    property real panX: 0
    property real panY: 0
    // телефон, второй экран: картинка занимает всё, кроме зоны выреза камеры (её даёт система, она свободна) — в этой зоне лежат кнопки.
    // Без выреза кнопки остаются маленькими поверх угла картинки
    readonly property bool edge: second && IsMobile && !pip
    readonly property real siT: edge ? SafeArea.margins.top : 0
    readonly property real siL: edge ? SafeArea.margins.left : 0
    readonly property real siR: edge ? SafeArea.margins.right : 0
    readonly property real siB: edge ? SafeArea.margins.bottom : 0
    readonly property string btnSide: siT >= 30 ? "top" : (siL >= 30 ? "left" : (siR >= 30 ? "right" : "none"))
    readonly property real aspect: RemoteDesktop.sourceAspect > 0 ? RemoteDesktop.sourceAspect
                                   : (RemoteDesktop.frameHeight > 0 ? RemoteDesktop.frameWidth / RemoteDesktop.frameHeight : 16 / 9)

    header: Item {
        visible: !page.pip && !page.fs
        implicitHeight: (page.pip || page.fs) ? 0 : 64
        Common.GlassButton { x: 16; anchors.verticalCenter: parent.verticalCenter; icon: "back"; text: IsMobile ? "" : (page.inWindow ? qsTr("Закрыть") : qsTr("Назад")); onClicked: page.back() }
        Row {
            anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
            spacing: IsMobile ? 6 : 8
            Common.GlassButton { visible: !IsMobile && !page.inWindow; text: qsTr("В окне"); onClicked: { page.keepViewing = true; page.detach(); } }
            Common.GlassButton { visible: IsMobile; icon: "refresh"; text: ""; onClicked: page.rot = (page.rot + 1) % 4 }          // поворот: по картинке → альбом → портрет → датчик
            // звук показывающего устройства: нажатие включает и выключает, подсказка — состояние («на» — идёт, «ждём» — запрос отправлен, причина — если не вышло)
            Common.GlassButton {
                icon: "speaker"; text: IsMobile ? "" : (RemoteDesktop.audioState === "on" ? qsTr("Звук: вкл") : (RemoteDesktop.audioState === "waiting" ? qsTr("Звук: ждём…") : qsTr("Звук")))
                primary: RemoteDesktop.audioState === "on"
                onClicked: RemoteDesktop.setAudio(RemoteDesktop.audioState === "", "")
            }
            Common.GlassButton { icon: "expand"; text: IsMobile ? "" : qsTr("На весь экран"); onClicked: page.fs = true }
            Common.GlassButton { visible: IsMobile; icon: "pip"; text: ""; onClicked: RemoteDesktop.enterPip() }
            Common.GlassButton {
                primary: page.live
                text: !page.control ? (IsMobile ? qsTr("Ввод") : qsTr("Управление: выкл"))
                      : (RemoteDesktop.controlState === 1 ? qsTr("Ждём разрешения…") : (RemoteDesktop.controlState === 3 ? qsTr("Отказано") : (IsMobile ? qsTr("Ввод: вкл") : qsTr("Управление: вкл"))))
                // включение «Ввода» сразу просит разрешение у владельца экрана: отдельной кнопки не нужно
                onClicked: {
                    page.control = !page.control;
                    if (page.control && RemoteDesktop.controlState !== 2 && RemoteDesktop.controlState !== 1) RemoteDesktop.requestControl();
                }
            }
        }
    }

    // ждём согласия и первых кадров: карточка с ходом запуска (сообщения показывающего), временем ожидания и отменой.
    // Раньше здесь была одна строка серого текста — во втором экране на весь экран оставался пустой тёмный прямоугольник
    property int waitSec: 0
    Timer { interval: 1000; repeat: true; running: RemoteDesktop.frameSeq === 0 && RemoteDesktop.viewingDeviceId !== ""; onRunningChanged: if (running) page.waitSec = 0; onTriggered: page.waitSec++ }
    Rectangle {
        visible: RemoteDesktop.frameSeq === 0 && !page.pip
        z: 20
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 32)
        height: waitCol.implicitHeight + 40
        radius: Theme.radius
        color: Theme.surfaceHigh
        border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.08)
        Column {
            id: waitCol
            x: 20; y: 20
            width: parent.width - 40
            spacing: 12
            Common.Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                name: "refresh"; size: 30; color: Theme.accent
                visible: RemoteDesktop.viewingDeviceId !== ""
                RotationAnimation on rotation { running: parent.visible; loops: Animation.Infinite; from: 0; to: 360; duration: 1100 }
            }
            Text {
                width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                text: RemoteDesktop.viewingDeviceId === "" ? qsTr("Демонстрация не идёт")
                      : (page.second ? qsTr("Подключаем второй экран") : (RemoteDesktop.viewStage >= 1 ? qsTr("Показ разрешён") : qsTr("Запрос отправлен")))
                color: Theme.text; font.family: Theme.fontFamily; font.pixelSize: 17; font.weight: Font.DemiBold
            }
            Text {
                visible: RemoteDesktop.viewingDeviceId !== ""
                width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                text: RemoteDesktop.viewNote !== "" ? RemoteDesktop.viewNote
                      : (RemoteDesktop.viewStage >= 1 ? qsTr("Ждём первые кадры. На том устройстве может появиться системное окно захвата экрана — подтвердите его")
                                                      : qsTr("Ждём, пока владелец экрана разрешит показ…"))
                color: Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 13
            }
            Text {
                visible: RemoteDesktop.viewingDeviceId !== "" && page.waitSec >= 3
                width: parent.width; horizontalAlignment: Text.AlignHCenter
                text: qsTr("Ожидание: ") + page.waitSec + qsTr(" с")
                color: page.waitSec > 20 ? Theme.warn : Theme.subtext; font.family: Theme.fontFamily; font.pixelSize: 11
            }
            Common.GlassButton {
                anchors.horizontalCenter: parent.horizontalCenter
                icon: "close"; text: RemoteDesktop.viewingDeviceId === "" ? qsTr("Закрыть") : qsTr("Отменить")
                onClicked: page.back()
            }
        }
    }

    // ---- окно просмотра ----
    Item {
        id: view
        anchors { fill: parent; margins: (page.pip || page.fs) ? 0 : 8
                  topMargin: page.edge ? page.siT : ((page.pip || page.fs) ? 0 : 8); leftMargin: page.edge ? page.siL : ((page.pip || page.fs) ? 0 : 8)
                  rightMargin: page.edge ? page.siR : ((page.pip || page.fs) ? 0 : 8); bottomMargin: page.edge ? page.siB : ((page.pip || page.fs) ? 0 : 8) }
        visible: RemoteDesktop.frameSeq > 0
        clip: true
        focus: page.live
        activeFocusOnTab: true

        // размер всего экрана показывающего, вписанный в окно при масштабе 1
        // второй экран закрывает весь экран телефона без полей: монитор создан по размеру экрана телефона, поэтому при почти одинаковых пропорциях
        // картинка растягивается ровно на окно (разница в пару процентов незаметна), иначе — вписывается целиком. Участок кадра при этом всегда весь
        readonly property bool fillAll: page.second && Math.abs(page.aspect / Math.max(0.01, width / Math.max(1, height)) - 1) < 0.12
        readonly property bool coverMode: page.second || page.cover
        readonly property real fitW: fillAll ? width : (page.cover ? Math.max(width, height * page.aspect) : Math.min(width, height * page.aspect))
        readonly property real fitH: fillAll ? height : fitW / page.aspect

        function clampPan() {
            const cw = fitW * page.zoom, ch = fitH * page.zoom;
            const mid = coverMode && page.zoom <= 1.001;       // без увеличения картинка второго экрана всегда по центру
            page.panX = (cw <= width || mid) ? (width - cw) / 2 : Math.min(0, Math.max(width - cw, page.panX));
            page.panY = (ch <= height || mid) ? (height - ch) / 2 : Math.min(0, Math.max(height - ch, page.panY));
        }
        // масштаб вокруг точки: она остаётся под пальцем
        function zoomAt(cx, cy, z) {
            const nz = Math.max(1, Math.min(8, z));
            const k = nz / page.zoom;
            page.panX = cx - (cx - page.panX) * k;
            page.panY = cy - (cy - page.panY) * k;
            page.zoom = nz;
            clampPan();
            regionT.restart();
        }
        function resetZoom() { page.zoom = 1; clampPan(); regionT.restart(); }
        // точка окна → доли всего экрана показывающего (по ним и ставится курсор)
        function nx(x) { return Math.max(0, Math.min(1, (x - page.panX) / (fitW * page.zoom))); }
        function ny(y) { return Math.max(0, Math.min(1, (y - page.panY) / (fitH * page.zoom))); }

        onWidthChanged: { clampPan(); regionT.restart(); sizeT.restart(); }
        onHeightChanged: { clampPan(); regionT.restart(); }
        Component.onCompleted: { clampPan(); sizeT.restart(); }
        Connections { target: RemoteDesktop; function onFrameUpdated() { if (page.zoom === 1) view.clampPan(); } }

        // картинка кадра: лежит там, куда попала область кадра на всём экране показывающего
        Item {
            id: content
            width: view.fitW; height: view.fitH
            x: page.panX; y: page.panY
            scale: page.zoom
            transformOrigin: Item.TopLeft
            // сам рисует собранную картинку текстурой сцены и сам знает, какую часть экрана она показывает (см. FrameView)
            FrameView { anchors.fill: parent; source: RemoteDesktop; visible: !RemoteDesktop.videoOnSink }
            // видео H.264: YUV-кадр сразу на видеочип (перевод цвета — шейдером), лежит в той части экрана, которую показывает кадр
            VideoOutput {
                id: vout
                visible: RemoteDesktop.videoOnSink
                x: RemoteDesktop.frameRegion.x * parent.width; y: RemoteDesktop.frameRegion.y * parent.height
                width: RemoteDesktop.frameRegion.width * parent.width; height: RemoteDesktop.frameRegion.height * parent.height
                fillMode: VideoOutput.Stretch
                Component.onCompleted: RemoteDesktop.videoSink = vout.videoSink
                Component.onDestruction: if (RemoteDesktop.videoSink === vout.videoSink) RemoteDesktop.videoSink = null
            }
        }

        // ---------- касания ----------
        // щипок: масштаб и сдвиг двумя пальцами
        PinchHandler {
            id: pinch
            target: null
            acceptedDevices: PointerDevice.TouchScreen
            minimumPointCount: 2; maximumPointCount: 2
            property real z0: 1
            property point p0
            onActiveChanged: if (active) { z0 = page.zoom; p0 = Qt.point(page.panX, page.panY); }
            onActiveScaleChanged: if (active) {
                const nz = Math.max(1, Math.min(8, z0 * activeScale));
                const k = nz / z0;
                const c0 = centroid.pressPosition, c1 = centroid.position;
                // точка картинки под начальным центром щипка переезжает под текущий центр
                page.panX = c1.x - (c0.x - p0.x) * k;
                page.panY = c1.y - (c0.y - p0.y) * k;
                page.zoom = nz;
                view.clampPan();
                regionT.restart();
            }
            onCentroidChanged: if (active) {
                const k = page.zoom / z0;
                page.panX = centroid.position.x - (centroid.pressPosition.x - p0.x) * k;
                page.panY = centroid.position.y - (centroid.pressPosition.y - p0.y) * k;
                view.clampPan();
                regionT.restart();
            }
        }
        // просмотр: один палец двигает увеличенную картинку
        DragHandler {
            id: panDrag
            target: null
            acceptedDevices: PointerDevice.TouchScreen
            minimumPointCount: 1; maximumPointCount: 1
            enabled: !page.live
            property point p0
            onActiveChanged: if (active) p0 = Qt.point(page.panX, page.panY)
            onTranslationChanged: if (active) {
                page.panX = p0.x + translation.x;
                page.panY = p0.y + translation.y;
                view.clampPan();
                regionT.restart();
            }
        }
        // двойное касание в режиме просмотра: приблизить / вернуть
        TapHandler {
            acceptedDevices: PointerDevice.TouchScreen
            enabled: !page.live
            onDoubleTapped: (point) => { if (page.zoom > 1.05) view.resetZoom(); else view.zoomAt(point.position.x, point.position.y, 2.5); }
        }
        // ввод: касание — курсор в то место, куда нажали, и щелчок; долгое — правая кнопка
        TapHandler {
            acceptedDevices: PointerDevice.TouchScreen
            enabled: page.live
            longPressThreshold: 0.6
            onTapped: (point) => {
                RemoteDesktop.sendMoveAbsolute(view.nx(point.position.x), view.ny(point.position.y));
                RemoteDesktop.sendClick("left");
            }
            onLongPressed: {
                RemoteDesktop.sendMoveAbsolute(view.nx(point.position.x), view.ny(point.position.y));
                RemoteDesktop.sendClick("right");
            }
        }
        // ввод: перетаскивание одним пальцем — курсор идёт за пальцем с зажатой левой кнопкой (выделение, перенос окна)
        DragHandler {
            id: inputDrag
            target: null
            acceptedDevices: PointerDevice.TouchScreen
            minimumPointCount: 1; maximumPointCount: 1
            enabled: page.live
            onActiveChanged: {
                if (active) {
                    RemoteDesktop.sendMoveAbsolute(view.nx(centroid.pressPosition.x), view.ny(centroid.pressPosition.y));
                    RemoteDesktop.sendButton("left", true);
                } else {
                    RemoteDesktop.sendMoveAbsolute(view.nx(centroid.position.x), view.ny(centroid.position.y));
                    RemoteDesktop.sendButton("left", false);
                }
            }
            onCentroidChanged: if (active) { move.px = view.nx(centroid.position.x); move.py = view.ny(centroid.position.y); move.moved = true; }
        }

        // ---------- мышь ----------
        // положение мыши уходит не на каждое событие: ~30 раз в секунду, а на втором экране — ~100 (иначе курсор отстаёт от пальца и от картинки)
        QtObject { id: move; property real px: 0; property real py: 0; property bool moved: false }
        QtObject { id: exitKeys; property int n: 0; property double at: 0 }
        Timer { interval: page.second ? 10 : 33; repeat: true; running: page.live && move.moved; onTriggered: { RemoteDesktop.sendMoveAbsolute(move.px, move.py); move.moved = false; } }

        MouseArea {
            id: mouse
            anchors.fill: parent
            enabled: !IsMobile
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            // второй экран: свой курсор скрыт — виден только курсор первого устройства в картинке (раньше их было два)
            cursorShape: page.second ? Qt.BlankCursor : (page.live ? Qt.CrossCursor : (page.zoom > 1.05 ? Qt.OpenHandCursor : Qt.ArrowCursor))
            property point last
            function btn(b) { return b === Qt.RightButton ? "right" : (b === Qt.MiddleButton ? "middle" : "left"); }
            onPositionChanged: m => {
                if (page.live) { move.px = view.nx(m.x); move.py = view.ny(m.y); move.moved = true; }
                else if (pressed && page.zoom > 1.05) {                 // просмотр: перетаскивание двигает увеличенную картинку
                    page.panX += m.x - last.x; page.panY += m.y - last.y; last = Qt.point(m.x, m.y);
                    view.clampPan(); regionT.restart();
                }
            }
            onPressed: m => {
                view.forceActiveFocus();
                last = Qt.point(m.x, m.y);
                if (page.live) { RemoteDesktop.sendMoveAbsolute(view.nx(m.x), view.ny(m.y)); RemoteDesktop.sendButton(btn(m.button), true); }
            }
            onReleased: m => { if (page.live) RemoteDesktop.sendButton(btn(m.button), false); }
            onDoubleClicked: m => { if (!page.live) { if (page.zoom > 1.05) view.resetZoom(); else view.zoomAt(m.x, m.y, 2.5); } }
            onWheel: w => {
                // колесо: масштаб вокруг курсора; в режиме ввода — прокрутка у показывающего (с Ctrl — всё равно масштаб)
                if (!page.live || (w.modifiers & Qt.ControlModifier)) view.zoomAt(w.x, w.y, page.zoom * (w.angleDelta.y > 0 ? 1.18 : 1 / 1.18));
                else RemoteDesktop.sendScroll(Math.round(w.angleDelta.y / 120), Math.round(w.angleDelta.x / 120));
            }
        }

        // ---------- клавиатура ----------
        Keys.enabled: page.live || page.second
        Keys.onPressed: e => {
            if (e.key === Qt.Key_ScrollLock && page.kbPaused) { page.kbPaused = false; e.accepted = true; return; }     // снова отдать клавиатуру
            // второй экран: выход — Ctrl+C трижды подряд (за 1,5 с); первые два нажатия уходят как обычно
            if (page.second && e.key === Qt.Key_C && (e.modifiers & Qt.ControlModifier)) {
                const now = Date.now();
                exitKeys.n = now - exitKeys.at < 1500 ? exitKeys.n + 1 : 1;
                exitKeys.at = now;
                if (exitKeys.n >= 3) { exitKeys.n = 0; e.accepted = true; page.back(); return; }
            }
            if (!page.live) { e.accepted = true; return; }
            var name = page.keyName(e.key);
            var mods = [];
            if (e.modifiers & Qt.ControlModifier) mods.push("ctrl");
            if (e.modifiers & Qt.AltModifier) mods.push("alt");
            if (e.modifiers & Qt.MetaModifier) mods.push("meta");
            if (e.modifiers & Qt.ShiftModifier) mods.push("shift");
            // обычные символы уходят текстом (раскладка учтена), сочетания и служебные клавиши — по имени
            if (name === "" && e.text !== "" && !(e.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)))
                RemoteDesktop.sendType(e.text);
            else
                RemoteDesktop.sendKey(name !== "" ? name : String.fromCharCode(e.key).toLowerCase(), mods);
            e.accepted = true;
        }
    }

    function keyName(k) {
        switch (k) {
        case Qt.Key_Escape: return "escape";
        case Qt.Key_Backspace: return "backspace";
        case Qt.Key_Tab: return "tab";
        case Qt.Key_Return: case Qt.Key_Enter: return "enter";
        case Qt.Key_Space: return "space";
        case Qt.Key_Home: return "home";
        case Qt.Key_End: return "end";
        case Qt.Key_Insert: return "insert";
        case Qt.Key_Delete: return "delete";
        case Qt.Key_PageUp: return "pageup";
        case Qt.Key_PageDown: return "pagedown";
        case Qt.Key_Up: return "arrowup";
        case Qt.Key_Down: return "arrowdown";
        case Qt.Key_Left: return "arrowleft";
        case Qt.Key_Right: return "arrowright";
        default: return "";
        }
    }

    // ---- запрос увеличенного участка и размера окна: с задержкой, чтобы не слать на каждый пиксель жеста ----
    Timer {
        id: regionT
        interval: 220
        onTriggered: {
            const cw = view.fitW * page.zoom, ch = view.fitH * page.zoom;
            if (cw <= 0 || ch <= 0) return;
            if ((page.second || page.cover) && page.zoom <= 1.001) { RemoteDesktop.setViewRegion(0, 0, 1, 1); return; }
            const x0 = Math.max(0, -page.panX / cw), y0 = Math.max(0, -page.panY / ch);
            const x1 = Math.min(1, (view.width - page.panX) / cw), y1 = Math.min(1, (view.height - page.panY) / ch);
            RemoteDesktop.setViewRegion(x0, y0, x1 - x0, y1 - y0);
        }
    }
    Timer {
        id: sizeT
        interval: 500
        onTriggered: RemoteDesktop.setViewerWidth(Math.round(view.width * Screen.devicePixelRatio))
    }

    // ---- кнопки масштаба ----
    Row {
        visible: RemoteDesktop.frameSeq > 0 && !page.pip && !page.fs
        z: 5
        anchors { right: parent.right; bottom: parent.bottom; rightMargin: 16; bottomMargin: 16 }
        spacing: 8
        Common.GlassButton { visible: page.zoom > 1.05; text: Math.round(page.zoom * 100) + "%"; onClicked: view.resetZoom() }
        Common.GlassButton { icon: "minus"; text: ""; enabled: page.zoom > 1.01; onClicked: view.zoomAt(view.width / 2, view.height / 2, page.zoom / 1.5) }
        Common.GlassButton { icon: "plus"; text: ""; primary: true; enabled: page.zoom < 7.9; onClicked: view.zoomAt(view.width / 2, view.height / 2, page.zoom * 1.5) }
    }

    // связь с показывающим устройством оборвалась: последний кадр остаётся, поток восстанавливается сам
    Rectangle {
        visible: RemoteDesktop.reconnecting && !page.pip
        z: 6
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 12 }
        width: reconTxt.implicitWidth + 28; height: 32; radius: 16
        color: Qt.rgba(0, 0, 0, 0.65)
        Text { id: reconTxt; anchors.centerIn: parent; text: qsTr("Связь потеряна — восстанавливаем…"); color: Theme.warn; font.family: Theme.fontFamily; font.pixelSize: 12 }
    }

    // ---- статистика показа: коротко — строка с индикатором, по нажатию — подробная карточка с графиком частоты ----
    property bool statsOpen: false
    readonly property var st: RemoteDesktop.viewStats
    readonly property int stAge: RemoteDesktop.frameAgeMs
    // здоровье потока: зелёный — плавно, жёлтый — терпимо, красный — рвётся или кадров нет
    readonly property color stHealth: stAge > 5000 ? Theme.danger
                                      : (st.fps === undefined ? Theme.subtext
                                      : (st.fps >= 24 || (st.fps <= 2 && stAge < 2500) ? "#3ecf8e" : (st.fps >= 12 ? "#f5c451" : Theme.danger)))
    function kbitText(k) { return k >= 1000 ? (k / 1000).toFixed(1) + qsTr(" Мбит/с") : k + qsTr(" кбит/с"); }

    Rectangle {
        id: hud
        visible: RemoteDesktop.frameSeq > 0 && !page.pip && !page.second
        z: 5
        anchors { left: parent.left; bottom: parent.bottom; leftMargin: 14; bottomMargin: 14 }
        width: page.statsOpen ? Math.min(300, page.width - 28) : hudRow.implicitWidth + 24
        height: page.statsOpen ? hudCol.implicitHeight + 24 : 30
        radius: page.statsOpen ? 14 : 15
        color: Qt.rgba(0, 0, 0, page.statsOpen ? 0.72 : 0.55)
        border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.08)
        Behavior on width { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        Behavior on height { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        clip: true

        // коротко: точка здоровья · частота · кодек · поток
        Row {
            id: hudRow
            visible: !page.statsOpen
            anchors.centerIn: parent
            spacing: 7
            Rectangle { width: 8; height: 8; radius: 4; color: page.stHealth; anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: page.stAge > 5000 ? qsTr("нет кадров ") + Math.round(page.stAge / 1000) + qsTr(" с…")
                      : ((page.st.fps > 0 ? page.st.fps + qsTr(" к/с") : qsTr("экран не меняется")) + " · " + (page.st.codec || "") + (page.st.kbit > 0 ? " · " + page.kbitText(page.st.kbit) : ""))
                color: "white"; font.family: Theme.fontFamily; font.pixelSize: 11
            }
        }

        // подробно
        Column {
            id: hudCol
            visible: page.statsOpen
            x: 12; y: 12
            width: parent.width - 24
            spacing: 6
            Row {
                spacing: 8
                Rectangle { width: 9; height: 9; radius: 5; color: page.stHealth; anchors.verticalCenter: parent.verticalCenter }
                Text { text: page.st.codec || "—"; color: "white"; font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.DemiBold }
                Text { text: (page.st.w || 0) + "×" + (page.st.h || 0); color: Qt.rgba(1, 1, 1, 0.6); font.family: Theme.fontFamily; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
            }
            // график частоты прихода за последние 40 секунд
            Canvas {
                id: spark
                width: parent.width; height: 38
                property var hist: page.st.history || []
                onHistChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    const h = hist, n = h.length;
                    ctx.fillStyle = Qt.rgba(1, 1, 1, 0.05); ctx.fillRect(0, 0, width, height);
                    if (n < 2) return;
                    let mx = 30; for (let i = 0; i < n; ++i) mx = Math.max(mx, h[i]);
                    ctx.beginPath();
                    for (let i = 0; i < n; ++i) {
                        const x = width * i / 39, y = height - 3 - (height - 6) * h[i] / mx;
                        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
                    }
                    ctx.strokeStyle = page.stHealth; ctx.lineWidth = 1.6; ctx.stroke();
                    ctx.fillStyle = Qt.rgba(1, 1, 1, 0.45); ctx.font = "9px sans-serif";
                    ctx.fillText(mx + qsTr(" к/с"), 3, 10);
                }
            }
            Column {
                width: parent.width; spacing: 3
                Repeater {
                    model: [
                        [qsTr("Частота"), (page.st.fps || 0) + qsTr(" пришло · ") + (page.st.shown || 0) + qsTr(" показано, к/с")],
                        [qsTr("Поток"), page.kbitText(page.st.kbit || 0)],
                        [qsTr("Буфер"), (page.st.bufferMs || 0) + qsTr(" мс — сглаживание рывков сети")],
                        [qsTr("Свежесть"), page.stAge < 0 ? "—" : (page.stAge < 1000 ? page.stAge + qsTr(" мс назад") : Math.round(page.stAge / 1000) + qsTr(" с назад"))],
                        [qsTr("Отправитель"), page.st.steps > 0 ? qsTr("ступень ") + page.st.level + qsTr(" из ") + page.st.steps + qsTr(" · снимает ") + page.st.capFps + qsTr(" к/с") : "—"]
                    ]
                    Row {
                        required property var modelData
                        width: parent.width; spacing: 8
                        Text { width: 78; text: modelData[0]; color: Qt.rgba(1, 1, 1, 0.55); font.family: Theme.fontFamily; font.pixelSize: 11 }
                        Text { width: parent.width - 86; wrapMode: Text.WordWrap; text: modelData[1]; color: "white"; font.family: Theme.fontFamily; font.pixelSize: 11 }
                    }
                }
            }
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: page.statsOpen = !page.statsOpen }
    }

    // второй экран: экран целиком занят картинкой компьютера; единственное, что остаётся, — крестик закрытия в углу (полупрозрачный, виден всегда)
    // кнопки второго экрана: в зоне выреза камеры (сверху или сбоку), чтобы не лежать на картинке; без выреза — в углу
    Grid {
        id: secondBtns
        visible: page.second && !page.pip
        z: 30
        columns: page.btnSide === "left" || page.btnSide === "right" ? 1 : 2
        spacing: 8
        transformOrigin: Item.TopLeft
        // зона выреза бывает уже кнопки (≈48 px при кнопке 64): кнопки уменьшаются так, чтобы уложиться в свободную зону
        readonly property real zone: page.btnSide === "top" ? page.siT : (page.btnSide === "left" ? page.siL : (page.btnSide === "right" ? page.siR : 0))
        readonly property real unit: page.btnSide === "top" ? implicitHeight : implicitWidth
        scale: page.btnSide === "none" ? 0.8 : Math.min(1, Math.max(0.35, (zone - 6) / Math.max(1, unit)))
        opacity: page.btnSide === "none" ? 0.7 : 1
        x: page.btnSide === "right" ? parent.width - zone + (zone - implicitWidth * scale) / 2 : (page.btnSide === "left" ? (zone - implicitWidth * scale) / 2 : 14)
        y: page.btnSide === "top" ? (zone - implicitHeight * scale) / 2 : (page.btnSide === "none" ? 14 : Math.max(14, (parent.height - implicitHeight * scale) / 2))
        Common.GlassButton { icon: "close"; text: ""; onClicked: page.back() }
        Common.GlassButton { visible: IsMobile; icon: "pip"; text: ""; onClicked: RemoteDesktop.enterPip() }       // свернуть: окошко поверх других приложений
    }

    // в полноэкранном виде шапки нет: маленькие кнопки в углу (полупрозрачные, не мешают картинке)
    Row {
        visible: page.fs && !page.pip && !page.second
        z: 5
        anchors { top: parent.top; right: parent.right; margins: 12 }
        spacing: 8
        opacity: 0.7
        Common.GlassButton { visible: IsMobile; icon: "expand"; text: page.fill ? qsTr("Заполнить") : qsTr("Вписать"); onClicked: { page.fill = !page.fill; regionT.restart(); view.clampPan(); } }
        Common.GlassButton { visible: IsMobile; icon: "refresh"; text: page.rotNames[page.rot]; onClicked: page.rot = (page.rot + 1) % 4 }
        Common.GlassButton { icon: "close"; text: IsMobile ? "" : qsTr("Выйти из полного экрана"); onClicked: page.fs = false }
    }
    // подсказка о выходе со второго экрана: курсора своего нет, кнопок тоже — первые секунды видно, как выйти
    Rectangle {
        id: exitHint
        visible: opacity > 0
        opacity: page.second && !IsMobile && exitHintT.running ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 300 } }
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 24 }
        width: hintT.implicitWidth + 32; height: 40; radius: 20
        color: Qt.rgba(0, 0, 0, 0.7)
        z: 50
        Text { id: hintT; anchors.centerIn: parent; text: qsTr("Второй экран · выход — Ctrl+C три раза"); color: "white"; font.pixelSize: 14 }
        Timer { id: exitHintT; interval: 4500 }
        Connections { target: page; function onSecondChanged() { if (page.second) exitHintT.restart(); } }
        Component.onCompleted: if (page.second) exitHintT.restart()
    }
    Keys.onEscapePressed: page.fs = false

    // ---- второй экран на компьютере: вся клавиатура — туда (и Super, Alt+Tab, Ctrl+Alt+…), пока окно активно и управление разрешено.
    // Scroll Lock снимает захват; снова — Scroll Lock в окне или повторная активация окна
    property bool kbPaused: false
    readonly property bool kbWanted: page.second && !IsMobile && page.live && Window.window !== null && Window.window.active && !kbPaused
    onKbWantedChanged: RemoteDesktop.setKeyboardGrab(kbWanted)
    // окно — через страницу: у Connections (не Item) прикреплённого Window нет
    Connections {
        target: page.Window.window
        function onActiveChanged() { if (page.Window.window && !page.Window.window.active) page.kbPaused = false; }
    }
    Connections {
        target: RemoteDesktop
        function onKeyboardReleasedByUser() { page.kbPaused = true; kbHint.show(); }
        function onKeyboardGrabChanged() { if (RemoteDesktop.keyboardGrabbed) kbHint.show(); }
    }
    Keys.onPressed: e => { if (e.key === Qt.Key_ScrollLock && page.kbPaused) { page.kbPaused = false; e.accepted = true; } }
    Rectangle {
        id: kbHint
        function show() { opacity = 1; kbHintT.restart(); }
        visible: opacity > 0.01 && page.second && !IsMobile
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: 250 } }
        Timer { id: kbHintT; interval: 3000; onTriggered: kbHint.opacity = 0 }
        z: 40
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 18 }
        width: kbTxt.implicitWidth + 32; height: 36; radius: 18
        color: Qt.rgba(0, 0, 0, 0.72)
        Text {
            id: kbTxt
            anchors.centerIn: parent
            text: RemoteDesktop.keyboardGrabbed ? qsTr("Вся клавиатура — на том компьютере · Scroll Lock — вернуть себе")
                                               : qsTr("Клавиатура снова здесь · Scroll Lock — отдать тому компьютеру")
            color: "white"; font.family: Theme.fontFamily; font.pixelSize: 13
        }
    }

    Component.onDestruction: {
        RemoteDesktop.setKeyboardGrab(false);
        if (Window.window && Window.window.pageFullscreen !== undefined) Window.window.pageFullscreen = false;
        if (IsMobile) RemoteDesktop.setFullscreenView(false, 0);
        else if (fs && Window.window) Window.window.visibility = Window.Windowed;
        if (!keepViewing) RemoteDesktop.stopViewing();
    }
}
