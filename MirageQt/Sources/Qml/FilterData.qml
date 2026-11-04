// Copyright © 2026 王孝慈. All rights reserved.
pragma Singleton
import QtQuick

QtObject {
    readonly property var resolutionTags: [["Other resolution", "Dynamic resolution"], ["Standard Definition", "1280 x 720", "1366 x 768", "1920 x 1080", "2560 x 1440", "3840 x 2160", "7680 x 4320"], ["Ultrawide Standard Definition", "Ultrawide 2560 x 1080", "Ultrawide 3440 x 1440"], ["Dual Standard Definition", "Dual 3840 x 1080", "Dual 5120 x 1440", "Dual 7680 x 2160"], ["Triple Standard Definition", "Triple 4096 x 768", "Triple 5760 x 1080", "Triple 7680 x 1440", "Triple 11520 x 2160"], ["Portrait Standard Definition", "Portrait 720 x 1280", "Portrait 1080 x 1920", "Portrait 1440 x 2560", "Portrait 2160 x 3840"]]

    readonly property var englishTags: ["Abstract", "Animals", "Anime", "Cartoon", "CGI", "Cyberpunk", "Fantasy", "Games", "Girl", "Boys", "Landscape", "Medieval", "Memes", "MMD", "Music", "Nature", "Pixel art", "Relaxing", "Retro", "Sci-Fi", "Sports", "Technology", "Film & TV", "Vehicles", "Uncategorized"]
    readonly property var tags: ["抽象", "动物", "动漫", "卡通", "CGI", "赛博朋克", "奇幻", "游戏", "女孩", "男孩", "风景", "中世纪", "表情包", "MMD", "音乐", "自然", "像素艺术", "治愈", "复古", "科幻", "运动", "科技", "影视", "载具", "未分类"]
    readonly property var resolutions: [
        {
            "title": "其他",
            "options": ["其他分辨率", "动态分辨率"]
        },
        {
            "title": "宽屏",
            "options": ["标清", "1280 x 720", "1366 x 768", "1920 x 1080 - 全高清", "2560 x 1440", "3840 x 2160 - 4K", "7680 x 4320 - 8K"]
        },
        {
            "title": "超宽屏",
            "options": ["超宽（标准）", "2560 x 1080", "3440 x 1440"]
        },
        {
            "title": "双显示器",
            "options": ["双显示器（标准）", "3840 x 1080", "5120 x 1440", "7680 x 2160"]
        },
        {
            "title": "三显示器",
            "options": ["三显示器（标准）", "4096 x 768", "5760 x 1080", "7680 x 1440", "11520 x 2160"]
        },
        {
            "title": "纵向监视器/手机",
            "options": ["纵向（标准）", "720 x 1280", "1080 x 1920", "1440 x 2560", "2160 x 3840"]
        }
    ]
}
