// OptionData.js
//
// ContentView 使用的静态选项数据：筛选器清单、播放列表选项、创意工坊选项。
// 这些常量与 UI 逻辑无关，集中在数据模块避免与视图代码混在一起。
// QML 侧通过 `import "OptionData.js" as OptionData` 使用，
// 如 `property var typeFilters: OptionData.typeFilters`。
.pragma library

/** 已安装壁纸 - 类型筛选选项 */
var typeFilters = [
    { label: "场景", key: "scene" },
    { label: "视频", key: "video" },
    { label: "网页", key: "web" },
    { label: "应用程序", key: "application" },
    { label: "预设", key: "preset" }
];

/** 已安装壁纸 - 分级筛选选项 */
var ratingFilters = [
    { label: "所有人", key: "Everyone" },
    { label: "轻度裸露", key: "Questionable" },
    { label: "成人", key: "Mature" }
];

/** 已安装壁纸 - 来源筛选选项 */
var sourceFilters = [
    { label: "创意工坊", key: "workshop" },
    { label: "我的壁纸", key: "imported" }
];

/** 已安装壁纸 - 标签筛选选项 */
var tagFilters = [
    { label: "抽象", key: "abstract" },
    { label: "动物", key: "animal" },
    { label: "动漫", key: "anime" },
    { label: "卡通", key: "cartoon" },
    { label: "CGI", key: "cgi" },
    { label: "赛博朋克", key: "cyberpunk" },
    { label: "奇幻", key: "fantasy" },
    { label: "游戏", key: "game" },
    { label: "女孩", key: "girls" },
    { label: "男孩", key: "guys" },
    { label: "风景", key: "landscape" },
    { label: "中世纪", key: "medieval" },
    { label: "表情包", key: "memes" },
    { label: "MMD", key: "mmd" },
    { label: "音乐", key: "music" },
    { label: "自然", key: "nature" },
    { label: "像素艺术", key: "pixelart" },
    { label: "治愈", key: "relaxing" },
    { label: "复古", key: "retro" },
    { label: "科幻", key: "scifi" },
    { label: "运动", key: "sports" },
    { label: "科技", key: "technology" },
    { label: "影视", key: "television" },
    { label: "载具", key: "vehicle" },
    { label: "未分类", key: "unspecified" }
];

/** 播放列表 - 顺序选项 */
var playlistOrderOptions = [
    { label: "有序", key: "sorted" },
    { label: "随机", key: "random" }
];

/** 播放列表 - 计时选项 */
var playlistTimingOptions = [
    { label: "计时器", key: "timer" },
    { label: "登录时", key: "logon" },
    { label: "当日时间", key: "daytime" },
    { label: "星期", key: "dayOfWeek" },
    { label: "从不", key: "never" }
];

/** 播放列表 - 过渡选项 */
var playlistTransitionOptions = [
    { label: "启用全部", key: "enabled" },
    { label: "禁用全部", key: "disabled" },
    { label: "随机", key: "random" }
];

/** 播放列表 - 播放暂停选项 */
var playbackOptions = [
    { label: "其他应用获得焦点时", key: "otherApplicationFocused" },
    { label: "其他应用全屏时", key: "otherApplicationFullscreen" },
    { label: "其他应用播放音频时", key: "otherApplicationPlayingAudio" },
    { label: "显示器睡眠时", key: "displayAsleep" },
    { label: "笔记本使用电池时", key: "laptopOnBattery" }
];

/** 播放列表 - 暂停时的行为选项 */
var playbackModes = [
    { label: "保持运行", key: "keepRunning" },
    { label: "静音", key: "mute" },
    { label: "暂停", key: "pause" },
    { label: "停止（释放内存）", key: "stop" }
];

/** 星期标签（1-7 对应 周一至周日），用于"当日时间/星期"排程 */
var weekdayLabels = ["星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"];

/** 已安装壁纸 - 排序选项 */
var installedSortOptions = [
    { label: "名称", key: "name" },
    { label: "评分", key: "rating" },
    { label: "文件大小", key: "size" }
];

/** 创意工坊 - 排序选项 */
var workshopSortOptions = [
    { label: "热门趋势", key: "trending" },
    { label: "最新发布", key: "recent" },
    { label: "订阅最多", key: "subscribed" },
    { label: "评分最高", key: "rated" },
    { label: "最多投票", key: "upvoted" },
    { label: "播放时长最多", key: "playtime" },
    { label: "总播放时长最多", key: "total-playtime" },
    { label: "平均播放时长最长", key: "average-playtime" },
    { label: "终身平均播放时长", key: "lifetime-average" },
    { label: "播放次数最多", key: "sessions" },
    { label: "终身播放次数最多", key: "lifetime-sessions" },
    { label: "最近更新", key: "updated" }
];

/** 创意工坊 - 类型筛选选项 */
var workshopTypeFilters = [
    { label: "全部", key: "all" },
    { label: "场景", key: "scene" },
    { label: "网页", key: "web" },
    { label: "视频", key: "video" },
    { label: "预设", key: "preset" }
];

/**
 * 分辨率过滤分组（其他/宽屏/超宽屏/双显示器/三显示器/纵向）。
 * 已安装壁纸（FilterResults）与已订阅（SubscribedWorkshopFilterSidebar）共用，
 * 对齐 macOS 的 FR*Resolution OptionSet（FilterResults 与订阅侧引用同一组定义）。
 * group 为后端 WorkshopResolutionGroup 枚举值（bit 组），maskKey 对应
 * mirage.subscriptionFilters / ContentView 中的掩码字段名（+ "Mask" 后缀）；
 * options 顺序与 bit 位一一对应（bit 0 = 第一个选项），
 * 与 C++ workshopResolutionOptions() 保持一致。
 */
var resolutionGroups = [
    { title: "其他", group: 5, maskKey: "misc", options: ["其他分辨率", "动态分辨率"] },
    { title: "宽屏", group: 0, maskKey: "widescreen", options: ["标清", "1280 x 720", "1366 x 768", "1920 x 1080 - 全高清", "2560 x 1440", "3840 x 2160 - 4K", "7680 x 4320 - 8K"] },
    { title: "超宽屏", group: 1, maskKey: "ultraWidescreen", options: ["超宽（标准）", "2560 x 1080", "3440 x 1440"] },
    { title: "双显示器", group: 2, maskKey: "dualscreen", options: ["双显示器（标准）", "3840 x 1080", "5120 x 1440", "7680 x 2160"] },
    { title: "三显示器", group: 3, maskKey: "triplescreen", options: ["三显示器（标准）", "4096 x 768", "5760 x 1080", "7680 x 1440", "11520 x 2160"] },
    { title: "纵向监视器/手机", group: 4, maskKey: "portrait", options: ["纵向（标准）", "720 x 1280", "1080 x 1920", "1440 x 2560", "2160 x 3840"] }
];

/** 创意工坊 - 标签筛选选项（key 使用 Steam 创意工坊标签原值，区分大小写） */
var workshopTagFilters = [
    { label: "动漫", key: "Anime" },
    { label: "自然", key: "Nature" },
    { label: "抽象", key: "Abstract" },
    { label: "风景", key: "Landscape" },
    { label: "科幻", key: "Sci-Fi" },
    { label: "卡通", key: "Cartoon" },
    { label: "赛博朋克", key: "Cyberpunk" },
    { label: "奇幻", key: "Fantasy" },
    { label: "女孩", key: "Girl" },
    { label: "游戏", key: "Game" },
    { label: "动物", key: "Animal" },
    { label: "音乐", key: "Music" },
    { label: "车辆", key: "Vehicle" },
    { label: "科技", key: "Technology" },
    { label: "复古", key: "Retro" },
    { label: "城市", key: "City" },
    { label: "太空", key: "Space" },
    { label: "暗黑", key: "Dark" },
    { label: "像素", key: "Pixel Art" },
    { label: "极简", key: "Minimalist" },
    { label: "水下", key: "Underwater" },
    { label: "放松", key: "Relaxing" },
    { label: "中世纪", key: "Medieval" },
    { label: "未分类", key: "Unspecified" }
];
