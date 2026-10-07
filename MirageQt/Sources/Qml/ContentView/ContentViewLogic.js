// ContentViewLogic.js
//
// ContentView 的纯逻辑层：壁纸筛选、发现页过滤、分页、属性编辑器桥接、
// 元数据解析。所有依赖窗口 / mirage 的状态一律通过参数传入，不读取任何
// 外部可变状态，因此可独立测试。QML 侧通过
// `import "ContentViewLogic.js" as ContentViewLogic` 使用。
//
// 命名约定：所有函数带 JSDoc 类型注释，参数为入参、返回新值，不修改入参。
//
// 注意：本模块依赖 PropertyLocalization.js，但不能用 `import ... as ...`
// 语法——qmlcachegen 编译裸脚本时不接受 JS 内的 import 语句（含
// `.pragma library` 时亦然）。改用 Qt.include 运行时加载该脚本，
// 其 displayText/propertyText 函数直接进入本模块作用域。

Qt.include("../PropertyLocalization.js");

// Qt.include 只把被包含文件的顶层函数注入本模块作用域，不会创建
// `PropertyLocalization` 命名空间；这里显式聚合，供 propertyLabel/
// propertyOptionItems 调用（等价于旧版 `import ... as PropertyLocalization`）。
var PropertyLocalization = {
    propertyText: propertyText,
    displayText: displayText
};

/**
 * 标签规范化：转小写并去掉空格与连字符，用于标签匹配。
 * @param {string} tag - 原始标签
 * @returns {string} 规范化后的标签
 */
function normalizeTag(tag) {
    return String(tag == null ? "" : tag).toLowerCase().replace(/[ -]/g, "");
}

/**
 * 分辨率 tag 归一化：小写并仅保留字母与数字。
 * 对齐 C++ WorkshopModels.cpp 的 normalizeResolutionTag（isLetterOrNumber，
 * 去掉空格/连字符/点号等），保证与订阅侧 workshopResolutionMatches 一致。
 * @param {string} tag - 原始分辨率 tag
 * @returns {string} 归一化后的 tag
 */
function normalizeResolutionTag(tag) {
    return String(tag == null ? "" : tag).toLowerCase().replace(/[^a-z0-9]/g, "");
}

// 六组分辨率选项数量（决定"全选"掩码的位数），与 C++ resolutionOptionCount
// 及 OptionData.resolutionGroups 的 options.length 一一对应。
var __resolutionOptionCounts = [7, 3, 4, 5, 5, 2];

/**
 * 归一化后的分辨率 tag → (group, bit)。映射表严格对齐 C++ resolutionKindForTag。
 * @param {string} normalized - normalizeResolutionTag 的输出
 * @returns {number[]|null} [group, bit]，未命中返回 null
 */
function resolutionKindForTag(normalized) {
    if (normalized === "standarddefinition") return [0, 0];
    if (normalized === "1280x720") return [0, 1];
    if (normalized === "1366x768") return [0, 2];
    if (normalized.startsWith("1920x1080")) return [0, 3];
    if (normalized === "2560x1440") return [0, 4];
    if (normalized.startsWith("3840x2160")) return [0, 5];
    if (normalized.startsWith("7680x4320")) return [0, 6];
    if (normalized === "ultrawidestandard" || normalized === "ultrawidestandarddefinition") return [1, 0];
    if (normalized === "ultrawide2560x1080" || normalized === "2560x1080") return [1, 1];
    if (normalized === "ultrawide3440x1440" || normalized === "3440x1440") return [1, 2];
    if (normalized === "dualstandard" || normalized === "dualstandarddefinition") return [2, 0];
    if (normalized === "dual3840x1080" || normalized === "3840x1080") return [2, 1];
    if (normalized === "dual5120x1440" || normalized === "5120x1440") return [2, 2];
    if (normalized === "dual7680x2160" || normalized === "7680x2160") return [2, 3];
    if (normalized === "triplestandard" || normalized === "triplestandarddefinition") return [3, 0];
    if (normalized === "triple4096x768" || normalized === "4096x768") return [3, 1];
    if (normalized === "triple5760x1080" || normalized === "5760x1080") return [3, 2];
    if (normalized === "triple7680x1440" || normalized === "7680x1440") return [3, 3];
    if (normalized === "triple11520x2160" || normalized === "11520x2160") return [3, 4];
    if (normalized === "portraitstandard" || normalized === "portraitstandarddefinition" || normalized === "potraitstandard") return [4, 0];
    if (normalized === "portrait720x1280" || normalized === "720x1280") return [4, 1];
    if (normalized === "portrait1080x1920" || normalized === "1080x1920") return [4, 2];
    if (normalized === "portrait1440x2560" || normalized === "1440x2560") return [4, 3];
    if (normalized === "portrait2160x3840" || normalized === "2160x3840") return [4, 4];
    if (normalized === "otherresolution") return [5, 0];
    if (normalized === "dynamicresolution") return [5, 1];
    return null;
}

/**
 * 判定 tags 是否命中给定的分辨率过滤（六组掩码，bit 0 为各组第一个选项）。
 * 语义对齐 C++ workshopResolutionMatches / macOS FRResolutionFilter.matches(tags:)：
 * 六组全选 → true；tags 中无任何分辨率 tag → 等价于仅选中 misc.otherResolution。
 * @param {string[]} tags - 壁纸原始 tag 列表（内部再做分辨率归一化）
 * @param {number[]} masks - [宽屏, 超宽屏, 双, 三, 纵向, 其他] 六组掩码
 * @returns {boolean} true 表示通过分辨率过滤
 */
function resolutionMatches(tags, masks) {
    var allSelected = true;
    for (var group = 0; group < __resolutionOptionCounts.length; ++group) {
        var allMask = (1 << __resolutionOptionCounts[group]) - 1;
        if (masks[group] !== allMask) {
            allSelected = false;
            break;
        }
    }
    if (allSelected) return true;

    var anyKind = false;
    for (var index = 0; index < tags.length; ++index) {
        var kind = resolutionKindForTag(normalizeResolutionTag(tags[index]));
        if (!kind) continue;
        anyKind = true;
        if ((masks[kind[0]] & (1 << kind[1])) !== 0) return true;
    }
    // 无任何分辨率 tag 时，等价于仅选中 misc.otherResolution（bit 0）。
    if (!anyKind) return (masks[5] & 1) !== 0;
    return false;
}

/**
 * 判断 key 是否已存在于 filter 数组。
 * @param {string[]} filters - 已启用的 key 列表
 * @param {string} key - 要查询的 key
 * @returns {boolean} true 表示已启用
 */
function isEnabled(filters, key) {
    return filters.indexOf(key) !== -1;
}

/**
 * 生成启用/停用 key 后的新数组（不修改入参）。
 * @param {string[]} list - 当前 key 列表
 * @param {string} key - 要增删的 key
 * @param {boolean} enabled - true 加入，false 移除
 * @returns {string[]} 新的 key 列表
 */
function setEnabled(list, key, enabled) {
    var next = list.slice();
    var index = next.indexOf(key);
    if (enabled && index === -1)
        next.push(key);
    if (!enabled && index !== -1)
        next.splice(index, 1);
    return next;
}

/**
 * 在选项中按字段查找值对应的下标，找不到返回 0（ComboBox 默认项）。
 * @param {Array<Object>} options - 选项数组，每项含 field 字段
 * @param {*} value - 要查找的值
 * @param {string} [field="key"] - 参与比较的字段名
 * @returns {number} 匹配下标（0 起步），无匹配返回 0
 */
function findOptionIndex(options, value, field) {
    field = field || "key";
    for (var index = 0; index < options.length; ++index) {
        if (String(options[index][field]) === String(value))
            return index;
    }
    return 0;
}

/**
 * 元数据标签解析：按分隔符拆分、去重、排序。
 * @param {string} text - 用户输入的标签文本（以 ; , 或换行分隔）
 * @returns {string[]} 去重并排序后的标签列表
 */
function metadataTagList(text) {
    var result = [];
    var seen = {};
    var values = String(text == null ? "" : text).split(/[;,\n]/);
    for (var index = 0; index < values.length; ++index) {
        var tag = values[index].trim();
        var normalized = tag.toLowerCase();
        if (tag.length === 0 || seen[normalized])
            continue;
        seen[normalized] = true;
        result.push(tag);
    }
    result.sort(function (left, right) {
        return left.localeCompare(right);
    });
    return result;
}

/**
 * 把任意类型的属性值归一化为可比较的 JS 值。
 * @param {*} value - 属性原始值
 * @param {string} type - WE 属性类型（"bool" 等）
 * @returns {boolean|number|string} 归一化后的值
 */
function propertyConditionValue(value, type) {
    if (typeof value === "boolean" || typeof value === "number")
        return value;
    var text = String(value == null ? "" : value).trim();
    if (type === "bool") {
        var normalized = text.toLowerCase();
        return normalized === "true" || normalized === "1" || normalized === "yes" || normalized === "on";
    }
    if (/^-?(?:\d+\.?\d*|\.\d+)$/.test(text))
        return Number(text);
    if (text === "true")
        return true;
    if (text === "false")
        return false;
    return text;
}

/**
 * 解析 WE 条件表达式（如 "hasChild && gender == 'male'"），决定属性是否可见。
 * 条件来自创意工坊项目文件，语法非法时降级为可见并输出警告，避免阻塞 UI。
 *
 * 性能：与 macOS WEConditionEvaluator.cache 同构——同一属性集周期内，
 * names/values 只构建一次（__buildPropertyConditionRuntime），每个 condition
 * 的判定结果缓存到 verdicts，QML 侧每个属性 delegate 的 visible 绑定与每个
 * combo 的 optionItems 绑定在重求值时不再各自 Function 构造（JS 编译）。
 * 属性集变化（选中/属性编辑）时由 ContentView 调用
 * invalidatePropertyConditionCache() 使缓存失效。本模块被 Qt.include 到
 * ContentView.qml 的实例与 ContentViewModel.qml 的实例各自独立，缓存只影响
 * 走 ContentView.host 的求值路径；filterWallpapers 等纯函数不依赖此缓存。
 * @param {string} condition - 条件表达式，空串视为无条件（可见）
 * @param {Array<Object>} properties - 当前选中壁纸的属性列表（mirage.selectedProperties）
 * @returns {boolean} true 表示可见
 */
var __propertyConditionRuntime = null; // {names, values, verdicts}

/**
 * 使属性条件判定缓存失效。必须在属性集变化（选中不同壁纸、编辑属性值）
 * 后、下一次求值前调用一次；缓存假定两次失效之间 properties 引用不变。
 */
function invalidatePropertyConditionCache() {
    __propertyConditionRuntime = null;
}

/**
 * 构建条件求值运行时：把属性列表压平为 JS 变量名与值数组（一次 O(N) 遍历），
 * 供后续多个 condition 复用，避免每个 condition 各自重复遍历属性列表。
 * @param {Array<Object>} properties - 属性列表
 * @returns {{names: string[], values: Array<{value: *}>, verdicts: Object}} 运行时
 */
function __buildPropertyConditionRuntime(properties) {
    var names = [];
    var values = [];
    var source = properties || [];
    for (var index = 0; index < source.length; ++index) {
        var property = source[index];
        if (!/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(property.key))
            continue;
        names.push(property.key);
        values.push({
            value: propertyConditionValue(property.value, property.type)
        });
    }
    return {
        names: names,
        values: values,
        verdicts: {}
    };
}

function propertyConditionVisible(condition, properties) {
    if (!condition || String(condition).trim().length === 0)
        return true;
    try {
        if (!__propertyConditionRuntime)
            __propertyConditionRuntime = __buildPropertyConditionRuntime(properties);
        var runtime = __propertyConditionRuntime;
        if (runtime.verdicts[condition] !== undefined)
            return runtime.verdicts[condition];
        var evaluate = Function.apply(null, runtime.names.concat(["return !!(" + condition + ");"]));
        var verdict = evaluate.apply(null, runtime.values);
        runtime.verdicts[condition] = verdict;
        return verdict;
    } catch (error) {
        console.warn("propertyConditionVisible: invalid condition:", condition, error);
        return true;
    }
}

/**
 * 属性编辑器标签：优先取项目文本，经本地化表翻译。
 * @param {Object} propertyData - 属性数据 {key, text}
 * @returns {string} 显示文本
 */
function propertyLabel(propertyData) {
    return PropertyLocalization.propertyText(propertyData.key, propertyData.text);
}

/**
 * 属性选项：过滤不可见项并生成 ComboBox 数据模型。
 * @param {Array<Object>} options - 原始选项列表
 * @param {Array<Object>} properties - 当前选中壁纸的属性列表
 * @returns {Array<{label: string, value: *}>} 可见选项的 {label, value} 列表
 */
function propertyOptionItems(options, properties) {
    var result = [];
    for (var index = 0; index < (options || []).length; ++index) {
        if (propertyConditionVisible(options[index].condition, properties)) {
            result.push({
                label: PropertyLocalization.displayText(options[index].label || options[index].value),
                value: options[index].value
            });
        }
    }
    return result;
}

/**
 * 属性布尔值归一化："true"/"1"/"yes"/"on" 视为真。
 * @param {*} value - 属性原始值
 * @returns {boolean} 归一化后的布尔值
 */
function propertyBoolValue(value) {
    if (typeof value === "boolean")
        return value;
    var normalized = String(value == null ? "" : value).toLowerCase();
    return normalized === "true" || normalized === "1" || normalized === "yes" || normalized === "on";
}

/**
 * 属性颜色解析：接受 "r g b" 文本（0-1），否则返回原值。
 * @param {*} value - 属性颜色值
 * @returns {color} Qt 颜色对象；无法解析时返回原值
 */
function propertyColor(value) {
    var text = String(value == null ? "" : value).trim();
    var components = text.split(/\s+/);
    if (components.length >= 3
            && !isNaN(Number(components[0]))
            && !isNaN(Number(components[1]))
            && !isNaN(Number(components[2]))) {
        return Qt.rgba(Number(components[0]), Number(components[1]), Number(components[2]), 1);
    }
    return value == null ? "white" : value;
}

/**
 * 把 Qt 颜色对象序列化为 "r g b" 文本（三位小数）。
 * @param {color} color - Qt 颜色对象
 * @returns {string} "r g b" 文本
 */
function propertyColorValue(color) {
    return Number(color.r).toFixed(3) + " " + Number(color.g).toFixed(3) + " " + Number(color.b).toFixed(3);
}

/**
 * 属性路径标签：取路径最后一段；空路径显示"未选择"。
 * @param {string} value - 文件或目录路径
 * @returns {string} 显示文本
 */
function propertyPathLabel(value) {
    var path = String(value == null ? "" : value);
    if (path.length === 0)
        return "未选择";
    var separator = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
    return separator >= 0 ? path.substring(separator + 1) : path;
}

/**
 * file:// URL 解码为本地路径。
 * @param {string} url - 文件对话框返回的 URL
 * @returns {string} 解码后的路径
 */
function selectedFilePath(url) {
    return decodeURIComponent(String(url).replace(/^file:\/\//, ""));
}

/**
 * 分页按钮数字序列：总页数 <= 7 时全部列出，否则用 0 表示省略号。
 * @param {number} page - 当前页
 * @param {number} count - 总页数
 * @returns {number[]} 页码序列（0 表示省略号）
 */
function wallpaperPageItems(page, count) {
    if (count <= 7) {
        var all = [];
        for (var index = 1; index <= count; ++index)
            all.push(index);
        return all;
    }
    if (page <= 4)
        return [1, 2, 3, 4, 5, 0, count];
    if (page >= count - 3)
        return [1, 0, count - 4, count - 3, count - 2, count - 1, count];
    return [1, 0, page - 1, page, page + 1, 0, count];
}

/**
 * 显示器标签列表："显示器 1"、"显示器 2"……
 * @param {number} screenCount - 显示器数量（mirage.screenCount）
 * @returns {string[]} 标签列表
 */
function screenLabels(screenCount) {
    var result = [];
    for (var index = 0; index < screenCount; ++index) {
        result.push("显示器 " + (index + 1));
    }
    return result;
}

/**
 * 已安装壁纸筛选 + 排序：按类型/分级/来源/标签/搜索词过滤，按名称/评分/大小排序。
 * @param {Array<Object>} source - 全量壁纸列表（mirage.wallpapers）
 * @param {Object} state - 筛选与排序状态
 * @param {string} state.searchText - 搜索词
 * @param {boolean} state.approvedOnly - 仅显示已批准
 * @param {boolean} state.favoritesOnly - 仅显示收藏
 * @param {boolean} state.mobileOnly - 仅显示移动端标签
 * @param {boolean} state.audioOnly - 仅显示音频响应标签
 * @param {boolean} state.customizableOnly - 仅显示可自定义
 * @param {string[]} state.enabledTypes - 启用的类型 key
 * @param {string[]} state.enabledRatings - 启用的分级 key
 * @param {string[]} state.enabledSources - 启用的来源 key
 * @param {string[]} state.enabledTags - 启用的标签 key
 * @param {Array<Object>} state.tagFilters - 全部标签选项（判断是否处于"筛选标签"状态）
 * @param {number} state.widescreenMask - 宽屏分辨率掩码（bit 0 = 第一个选项）
 * @param {number} state.ultraWidescreenMask - 超宽屏分辨率掩码
 * @param {number} state.dualscreenMask - 双显示器分辨率掩码
 * @param {number} state.triplescreenMask - 三显示器分辨率掩码
 * @param {number} state.portraitMask - 纵向分辨率掩码
 * @param {number} state.miscMask - 其他分辨率掩码
 * @param {string} state.sortMode - 排序模式："name"/"rating"/"size"
 * @param {boolean} state.sortDescending - 是否降序
 * @returns {Array<Object>} 筛选排序后的壁纸列表
 */
function filterWallpapers(source, state) {
    var query = String(state.searchText || "").trim().toLowerCase();
    var result = source.slice();
    result = result.filter(function (wallpaper) {
        var tags = (wallpaper.tags || []).map(normalizeTag);
        if (state.approvedOnly && !wallpaper.approved)
            return false;
        if (state.favoritesOnly && !wallpaper.favorite)
            return false;
        if (state.customizableOnly && !wallpaper.customizable)
            return false;
        if (state.mobileOnly && tags.indexOf("mobile") === -1)
            return false;
        if (state.audioOnly && tags.indexOf("audioresponsive") === -1 && tags.indexOf("audio") === -1)
            return false;
        if (!isEnabled(state.enabledTypes, wallpaper.type))
            return false;
        if (!isEnabled(state.enabledRatings, wallpaper.rating))
            return false;
        if (!isEnabled(state.enabledSources, wallpaper.source))
            return false;
        // 分辨率过滤：基于壁纸 project.json 的 tags 匹配六组掩码
        // （对齐 macOS FRResolutionFilter.matches(tags:)，与订阅侧
        // C++ workshopResolutionMatches 语义一致）。
        if (!resolutionMatches(wallpaper.tags || [], [
            state.widescreenMask,
            state.ultraWidescreenMask,
            state.dualscreenMask,
            state.triplescreenMask,
            state.portraitMask,
            state.miscMask
        ]))
            return false;

        if (state.enabledTags.length < state.tagFilters.length) {
            if (tags.length === 0) {
                if (!isEnabled(state.enabledTags, "unspecified"))
                    return false;
            } else {
                var tagMatches = false;
                for (var index = 0; index < tags.length; ++index) {
                    if (isEnabled(state.enabledTags, tags[index])) {
                        tagMatches = true;
                        break;
                    }
                }
                if (!tagMatches)
                    return false;
            }
        }
        return query.length === 0
            || String(wallpaper.searchText || "").toLowerCase().indexOf(query) !== -1;
    });
    result.sort(function (left, right) {
        var comparison = 0;
        if (state.sortMode === "rating") {
            comparison = String(left.rating || "").localeCompare(String(right.rating || ""));
        } else if (state.sortMode === "size") {
            comparison = Number(left.size || 0) - Number(right.size || 0);
        }
        if (comparison === 0) {
            comparison = String(left.title || "").localeCompare(String(right.title || ""));
        }
        return state.sortDescending ? -comparison : comparison;
    });
    return result;
}

/**
 * 发现页条目过滤：按创意工坊类型/分级/标签筛选。
 * @param {Array<Object>} items - 发现页条目列表
 * @param {Object} state - 筛选状态
 * @param {string} state.workshopType - 当前类型（"all" 表示不限）
 * @param {string[]} state.workshopRatings - 启用的分级 key
 * @param {string[]} state.workshopSelectedTags - 已选标签（需全部命中）
 * @returns {Array<Object>} 过滤后的条目
 */
function filterDiscoverItems(items, state) {
    return items.filter(function (item) {
        if (state.workshopType !== "all" && String(item.type || "") !== state.workshopType)
            return false;
        if (state.workshopRatings.length > 0
                && !isEnabled(state.workshopRatings, String(item.rating || "Everyone")))
            return false;
        if (state.workshopSelectedTags.length > 0) {
            var itemTags = item.tags || [];
            for (var index = 0; index < state.workshopSelectedTags.length; ++index) {
                if (itemTags.indexOf(state.workshopSelectedTags[index]) === -1)
                    return false;
            }
        }
        return true;
    });
}

/**
 * 自适应网格列宽（对齐 macOS 的 GridItem(.adaptive(minimum:maximum:))）：
 * 列数按最小列宽（iconSize + 间隔）确定，实际列宽在 [iconSize, 2×iconSize]
 * 区间内尽量拉满容器；容器窄到均分宽不足 iconSize 时退回 iconSize，
 * 宽到均分超上限时封顶 2×iconSize（右侧留白）。
 * 已安装/创意工坊/订阅网格共用，保证 4 分区尺寸规则一致。
 * @param {number} width - 网格可用宽度（GridView.width）
 * @param {number} iconSize - 图标尺寸基准（explorerIconSize，140/170/200）
 * @param {number} spacing - 卡片间隔（项目统一 14）
 * @returns {number} cellWidth（整数，不超过 width）
 */
function adaptiveGridCellWidth(width, iconSize, spacing) {
    var minimum = Number(iconSize);
    var gap = Number(spacing);
    if (minimum <= 0 || width <= 0)
        return Math.max(1, minimum);
    var cols = Math.max(1, Math.floor((width + gap) / (minimum + gap)));
    var avg = (width - gap * (cols - 1)) / cols;
    var cell = Math.max(minimum, Math.min(2 * minimum, Math.floor(avg)));
    return Math.max(1, Math.min(width, cell));
}
