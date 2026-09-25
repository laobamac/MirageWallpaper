import QtQuick

QtObject {
    id: controller

    property var window

    function open() {
        if (!window)
            return;
        window.show();
        window.raise();
        window.requestActivate();
    }

}
