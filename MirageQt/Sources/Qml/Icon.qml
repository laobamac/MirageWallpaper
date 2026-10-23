// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Shapes

Item {
    id: root
    property string name: ""
    property color color: Theme.text
    implicitWidth: 16
    implicitHeight: 16
    property var paths: ({
            search: "M10 3 A7 7 0 1 0 10 17 A7 7 0 1 0 10 3 M15 15 L21 21",
            down: "M5 9 L12 16 L19 9",
            up: "M5 15 L12 8 L19 15",
            left: "M15 5 L8 12 L15 19",
            right: "M9 5 L16 12 L9 19",
            plus: "M12 4 V20 M4 12 H20",
            minus: "M4 12 H20",
            close: "M6 6 L18 18 M18 6 L6 18",
            check: "M4 12 L9 17 L20 6",
            download: "M12 3 V15 M7 10 L12 15 L17 10 M4 15 V21 H20 V15",
            upload: "M12 16 V3 M7 8 L12 3 L17 8 M4 15 V21 H20 V15",
            folder: "M3 6 H10 L12 9 H21 V20 H3 Z",
            cloud: "M6 19 A5 5 0 0 1 5 9 A7 7 0 0 1 18 8 A5.5 5.5 0 0 1 18 19 Z",
            display: "M3 4 H21 V17 H3 Z M12 17 V21 M7 21 H17",
            phone: "M7 2 H17 V22 H7 Z M10 18 H14",
            refresh: "M20 9 A8 8 0 1 0 20 15 M20 3 V9 H14",
            heart: "M12 20 L3 11 A5 5 0 0 1 12 5 A5 5 0 0 1 21 11 Z",
            star: "M12 2 L15 9 L22 9 L17 14 L19 22 L12 17 L5 22 L7 14 L2 9 L9 9 Z",
            grid: "M3 3 H9 V9 H3 Z M15 3 H21 V9 H15 Z M3 15 H9 V21 H3 Z M15 15 H21 V21 H15 Z",
            list: "M8 5 H21 M8 12 H21 M8 19 H21 M3 5 H4 M3 12 H4 M3 19 H4",
            filter: "M10 5 H21 M10 12 H21 M10 19 H21 M2 4 L4 6 L7 2 M2 11 L4 13 L7 9 M2 18 L4 20 L7 16",
            settings: "M9 2 H15 L16 6 L20 7 L22 12 L20 17 L16 18 L15 22 H9 L8 18 L4 17 L2 12 L4 7 L8 6 Z M12 8 A4 4 0 1 0 12 16 A4 4 0 1 0 12 8",
            play: "M6 3 L21 12 L6 21 Z",
            pause: "M7 4 V20 M17 4 V20",
            stop: "M5 5 H19 V19 H5 Z",
            volume: "M3 9 H7 L12 4 V20 L7 15 H3 Z M16 8 Q20 12 16 16 M19 4 Q26 12 19 20",
            edit: "M4 16 L16 4 L20 8 L8 20 L3 21 Z M14 6 L18 10",
            trash: "M3 6 H21 M8 6 V3 H16 V6 M6 6 L7 21 H17 L18 6 M10 10 V17 M14 10 V17",
            info: "M12 2 A10 10 0 1 0 12 22 A10 10 0 1 0 12 2 M12 10 V17 M12 6 V7",
            warning: "M12 2 L23 21 H1 Z M12 8 V14 M12 17 V18",
            bubble: "M3 3 H21 V17 H10 L5 22 V17 H3 Z M12 6 V11 M12 13 V14",
            copy: "M8 8 H21 V22 H8 Z M16 8 V2 H2 V16 H8",
            person: "M12 2 A4 4 0 1 0 12 10 A4 4 0 1 0 12 2 M3 22 V19 Q3 13 12 13 Q21 13 21 19 V22",
            lock: "M5 10 H19 V22 H5 Z M8 10 V6 A4 4 0 0 1 16 6 V10 M12 14 V18",
            link: "M9 15 L15 9 M8 10 L5 13 A4 4 0 0 0 11 19 L14 16 M10 8 L13 5 A4 4 0 0 1 19 11 L16 14",
            sparkle: "M12 1 L15 9 L23 12 L15 15 L12 23 L9 15 L1 12 L9 9 Z",
            sliders: "M3 5 H7 M11 5 H21 M3 12 H14 M18 12 H21 M3 19 H5 M9 19 H21 M7 2 H11 V8 H7 Z M14 9 H18 V15 H14 Z M5 16 H9 V22 H5 Z",
            gauge: "M3 19 A10 10 0 1 1 21 19 M12 15 L17 7 M5 12 H7 M8 5 L9 7 M16 5 L15 7 M17 12 H19",
            image: "M2 3 H22 V21 H2 Z M2 17 L8 10 L14 16 L18 11 L22 16 M16 6 A2 2 0 1 0 16 10 A2 2 0 1 0 16 6",
            flame: "M13 2 Q22 12 19 19 Q12 26 5 18 Q1 12 8 6 Q7 12 11 12 Q15 10 13 2 Z",
            globe: "M12 2 A10 10 0 1 0 12 22 A10 10 0 1 0 12 2 M2 12 H22 M12 2 Q3 12 12 22 Q21 12 12 2",
            clock: "M12 2 A10 10 0 1 0 12 22 A10 10 0 1 0 12 2 M12 6 V12 L17 15"
        })
    Shape {
        width: 24
        height: 24
        scale: Math.min(root.width, root.height) / 24
        transformOrigin: Item.TopLeft
        ShapePath {
            strokeColor: root.color
            strokeWidth: 1.7
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg {
                path: root.paths[root.name] || root.paths.image
            }
        }
    }
}
