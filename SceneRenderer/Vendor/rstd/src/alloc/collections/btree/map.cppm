module;
#include <rstd/macro.hpp>

export module rstd.alloc:collections.btree_map;
export import :vec;
export import :collections.btree_node;
export import rstd.core;

namespace alloc::collections
{

using ::alloc::boxed::Box;
using ::alloc::collections::btree::detail::B;
using ::alloc::collections::btree::detail::CAPACITY;
using ::alloc::collections::btree::detail::Node;
using ::alloc::vec::Vec;
using namespace rstd::prelude;

export template<typename K, typename V>
class BTreeMap;
export template<typename K, typename V>
class BTreeMapIter;
export template<typename K, typename V>
class BTreeMapIterMut;
export template<typename K, typename V>
class BTreeMapIntoIter;
export template<typename K, typename V>
class BTreeMapKeys;
export template<typename K, typename V>
class BTreeMapValues;
export template<typename K, typename V>
class BTreeMapValuesMut;

template<typename NodeType>
struct BTreeMapFrame {
    NodeType* node;
    usize     index;
};

export template<typename K, typename V>
class BTreeMapIter : public rstd::DefaultInClass<BTreeMapIter<K, V>, rstd::iter::Iterator> {
    using TreeNode = Node<K, V>;
    using Frame    = BTreeMapFrame<const TreeNode>;

    Vec<Frame> front;
    Vec<Frame> back;
    usize      remaining;

    void push_left(const TreeNode* node) {
        while (node != nullptr) {
            front.push(Frame { node, 0 });
            if (node->leaf) break;
            node = node->child(0);
        }
    }

    void push_right(const TreeNode* node) {
        while (node != nullptr) {
            back.push(Frame { node, node->len });
            if (node->leaf) break;
            node = node->child(node->len);
        }
    }

public:
    using Item = rstd::tuple<rstd::ref<K>, rstd::ref<V>>;

    BTreeMapIter(const TreeNode* root, usize len): remaining(len) {
        push_left(root);
        push_right(root);
    }

    auto next() -> Option<Item> {
        if (remaining == 0) return None();
        for (;;) {
            auto& frame = front[front.len() - 1];
            auto* node  = frame.node;
            if (node->leaf) {
                if (frame.index == node->len) {
                    front.pop();
                    continue;
                }
                usize index = frame.index++;
                --remaining;
                return Some(
                    Item(rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
                         rstd::ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
            }
            if (frame.index == node->len) {
                front.pop();
                continue;
            }
            usize index = frame.index++;
            push_left(node->child(index + 1));
            --remaining;
            return Some(Item(rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
                             rstd::ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
        }
    }

    auto next_back() -> Option<Item> {
        if (remaining == 0) return None();
        for (;;) {
            auto& frame = back[back.len() - 1];
            auto* node  = frame.node;
            if (node->leaf) {
                if (frame.index == 0) {
                    back.pop();
                    continue;
                }
                usize index = --frame.index;
                --remaining;
                return Some(
                    Item(rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
                         rstd::ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
            }
            if (frame.index == 0) {
                back.pop();
                continue;
            }
            usize index = --frame.index;
            push_right(node->child(index));
            --remaining;
            return Some(Item(rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
                             rstd::ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
        }
    }

    auto size_hint() const -> rstd::iter::SizeHint { return { remaining, Some(usize(remaining)) }; }
    auto len() const -> usize { return remaining; }
};

export template<typename K, typename V>
class BTreeMapIterMut : public rstd::DefaultInClass<BTreeMapIterMut<K, V>, rstd::iter::Iterator> {
    using TreeNode = Node<K, V>;
    using Frame    = BTreeMapFrame<TreeNode>;

    Vec<Frame> front;
    Vec<Frame> back;
    usize      remaining;

    void push_left(TreeNode* node) {
        while (node != nullptr) {
            front.push(Frame { node, 0 });
            if (node->leaf) break;
            node = node->child(0);
        }
    }

    void push_right(TreeNode* node) {
        while (node != nullptr) {
            back.push(Frame { node, node->len });
            if (node->leaf) break;
            node = node->child(node->len);
        }
    }

public:
    using Item = rstd::tuple<rstd::ref<K>, rstd::mut_ref<V>>;

    BTreeMapIterMut(TreeNode* root, usize len): remaining(len) {
        push_left(root);
        push_right(root);
    }

    auto next() -> Option<Item> {
        if (remaining == 0) return None();
        for (;;) {
            auto& frame = front[front.len() - 1];
            auto* node  = frame.node;
            if (node->leaf) {
                if (frame.index == node->len) {
                    front.pop();
                    continue;
                }
                usize index = frame.index++;
                --remaining;
                return Some(
                    Item(rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
                         rstd::mut_ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
            }
            if (frame.index == node->len) {
                front.pop();
                continue;
            }
            usize index = frame.index++;
            push_left(node->child(index + 1));
            --remaining;
            return Some(
                Item(rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
                     rstd::mut_ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
        }
    }

    auto next_back() -> Option<Item> {
        if (remaining == 0) return None();
        for (;;) {
            auto& frame = back[back.len() - 1];
            auto* node  = frame.node;
            if (node->leaf) {
                if (frame.index == 0) {
                    back.pop();
                    continue;
                }
                usize index = --frame.index;
                --remaining;
                return Some(
                    Item(rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
                         rstd::mut_ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
            }
            if (frame.index == 0) {
                back.pop();
                continue;
            }
            usize index = --frame.index;
            push_right(node->child(index));
            --remaining;
            return Some(
                Item(rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
                     rstd::mut_ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
        }
    }

    auto size_hint() const -> rstd::iter::SizeHint { return { remaining, Some(usize(remaining)) }; }
    auto len() const -> usize { return remaining; }
};

export template<typename K, typename V>
class BTreeMapKeys : public rstd::DefaultInClass<BTreeMapKeys<K, V>, rstd::iter::Iterator> {
    BTreeMapIter<K, V> inner;

public:
    using Item = rstd::ref<K>;
    explicit BTreeMapKeys(BTreeMapIter<K, V> iter): inner(rstd::move(iter)) {}
    auto next() -> Option<Item> {
        auto item = inner.next();
        if (item.is_none()) return None();
        return Some(item->template get<0>());
    }
    auto next_back() -> Option<Item> {
        auto item = inner.next_back();
        if (item.is_none()) return None();
        return Some(item->template get<0>());
    }
    auto size_hint() const -> rstd::iter::SizeHint { return inner.size_hint(); }
    auto len() const -> usize { return inner.len(); }
};

export template<typename K, typename V>
class BTreeMapValues : public rstd::DefaultInClass<BTreeMapValues<K, V>, rstd::iter::Iterator> {
    BTreeMapIter<K, V> inner;

public:
    using Item = rstd::ref<V>;
    explicit BTreeMapValues(BTreeMapIter<K, V> iter): inner(rstd::move(iter)) {}
    auto next() -> Option<Item> {
        auto item = inner.next();
        if (item.is_none()) return None();
        return Some(item->template get<1>());
    }
    auto next_back() -> Option<Item> {
        auto item = inner.next_back();
        if (item.is_none()) return None();
        return Some(item->template get<1>());
    }
    auto size_hint() const -> rstd::iter::SizeHint { return inner.size_hint(); }
    auto len() const -> usize { return inner.len(); }
};

export template<typename K, typename V>
class BTreeMapValuesMut
    : public rstd::DefaultInClass<BTreeMapValuesMut<K, V>, rstd::iter::Iterator> {
    BTreeMapIterMut<K, V> inner;

public:
    using Item = rstd::mut_ref<V>;
    explicit BTreeMapValuesMut(BTreeMapIterMut<K, V> iter): inner(rstd::move(iter)) {}
    auto next() -> Option<Item> {
        auto item = inner.next();
        if (item.is_none()) return None();
        return Some(item->template get<1>());
    }
    auto next_back() -> Option<Item> {
        auto item = inner.next_back();
        if (item.is_none()) return None();
        return Some(item->template get<1>());
    }
    auto size_hint() const -> rstd::iter::SizeHint { return inner.size_hint(); }
    auto len() const -> usize { return inner.len(); }
};

export template<typename K, typename V>
class BTreeMapIntoIter : public rstd::DefaultInClass<BTreeMapIntoIter<K, V>, rstd::iter::Iterator> {
    ::alloc::vec::VecIntoIter<rstd::tuple<K, V>> inner;

public:
    using Item = rstd::tuple<K, V>;
    explicit BTreeMapIntoIter(Vec<Item> entries): inner(rstd::move(entries)) {}
    auto next() -> Option<Item> { return inner.next(); }
    auto next_back() -> Option<Item> { return inner.next_back(); }
    auto size_hint() const -> rstd::iter::SizeHint { return inner.size_hint(); }
    auto len() const -> usize { return inner.len(); }
};

export template<typename K, typename V>
class BTreeMap {
    using TreeNode = Node<K, V>;
    using Entry    = rstd::tuple<K, V>;

    Option<Box<TreeNode>> root;
    usize                 length;

    template<typename Q>
    static bool equivalent(const K& left, const Q& right) {
        return ! (left < right) && ! (right < left);
    }

    template<typename Q>
    static auto lower_bound(const TreeNode& node, const Q& key) -> usize {
        usize index = 0;
        while (index < node.len && node.key(index) < key) ++index;
        return index;
    }

    auto root_node() noexcept -> TreeNode* { return root.is_some() ? root->get() : nullptr; }
    auto root_node() const noexcept -> const TreeNode* {
        return root.is_some() ? root->as_ptr().as_raw_ptr() : nullptr;
    }

    static void insert_edge(TreeNode& node, usize index, Box<TreeNode> edge, usize active) {
        for (usize i = active; i > index; --i) node.move_edge(i - 1, i);
        node.write_edge(index, rstd::move(edge));
    }

    static auto remove_edge(TreeNode& node, usize index, usize active) -> Box<TreeNode> {
        auto removed = node.take_edge(index);
        for (usize i = index; i + 1 < active; ++i) node.move_edge(i + 1, i);
        return removed;
    }

    static void split_child(TreeNode& parent, usize child_index) {
        auto* child   = parent.child(child_index);
        auto  sibling = Box<TreeNode>::make(child->leaf);

        for (usize i = 0; i < B - 1; ++i) {
            auto entry = child->take_entry(B + i);
            sibling->write_entry(
                i, rstd::move(entry.template get<0>()), rstd::move(entry.template get<1>()));
        }
        sibling->len = B - 1;

        if (! child->leaf) {
            for (usize i = 0; i < B; ++i) sibling->write_edge(i, child->take_edge(B + i));
        }

        auto middle = child->take_entry(B - 1);
        child->len  = B - 1;
        insert_edge(parent, child_index + 1, rstd::move(sibling), parent.len + 1);
        parent.insert_entry(child_index,
                            rstd::move(middle.template get<0>()),
                            rstd::move(middle.template get<1>()));
    }

    auto insert_non_full(TreeNode& start, K key, V value) -> Option<V> {
        TreeNode* node = rstd::addressof(start);
        for (;;) {
            usize index = lower_bound(*node, key);
            if (index < node->len && equivalent(node->key(index), key)) {
                auto stored = node->take_entry(index);
                V    old    = rstd::move(stored.template get<1>());
                node->write_entry(index, rstd::move(stored.template get<0>()), rstd::move(value));
                return Some(rstd::move(old));
            }

            if (node->leaf) {
                node->insert_entry(index, rstd::move(key), rstd::move(value));
                ++length;
                return None();
            }

            auto* child = node->child(index);
            if (child->len == CAPACITY) {
                split_child(*node, index);
                if (equivalent(node->key(index), key)) {
                    auto stored = node->take_entry(index);
                    V    old    = rstd::move(stored.template get<1>());
                    node->write_entry(
                        index, rstd::move(stored.template get<0>()), rstd::move(value));
                    return Some(rstd::move(old));
                }
                if (node->key(index) < key) ++index;
            }
            node = node->child(index);
        }
    }

    static void borrow_from_previous(TreeNode& parent, usize child_index) {
        auto* child         = parent.child(child_index);
        auto* sibling       = parent.child(child_index - 1);
        auto  parent_entry  = parent.take_entry(child_index - 1);
        auto  sibling_entry = sibling->remove_entry(sibling->len - 1);

        child->insert_entry(0,
                            rstd::move(parent_entry.template get<0>()),
                            rstd::move(parent_entry.template get<1>()));
        parent.write_entry(child_index - 1,
                           rstd::move(sibling_entry.template get<0>()),
                           rstd::move(sibling_entry.template get<1>()));

        if (! child->leaf) {
            auto edge = sibling->take_edge(sibling->len + 1);
            insert_edge(*child, 0, rstd::move(edge), child->len);
        }
    }

    static void borrow_from_next(TreeNode& parent, usize child_index) {
        auto* child         = parent.child(child_index);
        auto* sibling       = parent.child(child_index + 1);
        auto  parent_entry  = parent.take_entry(child_index);
        auto  sibling_entry = sibling->remove_entry(0);

        child->insert_entry(child->len,
                            rstd::move(parent_entry.template get<0>()),
                            rstd::move(parent_entry.template get<1>()));
        parent.write_entry(child_index,
                           rstd::move(sibling_entry.template get<0>()),
                           rstd::move(sibling_entry.template get<1>()));

        if (! child->leaf) {
            auto edge = remove_edge(*sibling, 0, sibling->len + 2);
            insert_edge(*child, child->len, rstd::move(edge), child->len);
        }
    }

    static auto merge_children(TreeNode& parent, usize left_index) -> TreeNode& {
        usize old_parent_len = parent.len;
        auto  right          = remove_edge(parent, left_index + 1, old_parent_len + 1);
        auto* left           = parent.child(left_index);
        auto  middle         = parent.remove_entry(left_index);
        usize right_len      = right->len;

        left->insert_entry(
            left->len, rstd::move(middle.template get<0>()), rstd::move(middle.template get<1>()));
        while (right->len != 0) {
            auto entry = right->remove_entry(0);
            left->insert_entry(left->len,
                               rstd::move(entry.template get<0>()),
                               rstd::move(entry.template get<1>()));
        }

        if (! left->leaf) {
            usize first_edge = left->len - right_len;
            for (usize i = 0; i <= right_len; ++i) {
                left->write_edge(first_edge + i, right->take_edge(i));
            }
            right->leaf = true;
        }
        return *left;
    }

    static auto remove_min(TreeNode& node) -> Entry {
        if (node.leaf) return node.remove_entry(0);
        auto* child = node.child(0);
        if (child->len == B - 1) {
            if (node.edge(1)->len >= B)
                borrow_from_next(node, 0);
            else
                child = rstd::addressof(merge_children(node, 0));
        }
        return remove_min(*child);
    }

    static auto remove_max(TreeNode& node) -> Entry {
        if (node.leaf) return node.remove_entry(node.len - 1);
        usize child_index = node.len;
        auto* child       = node.child(child_index);
        if (child->len == B - 1) {
            if (node.edge(child_index - 1)->len >= B)
                borrow_from_previous(node, child_index);
            else {
                child = rstd::addressof(merge_children(node, child_index - 1));
            }
        }
        return remove_max(*child);
    }

    template<typename Q>
    static auto remove_from_internal(TreeNode& node, usize index, const Q& key) -> Option<Entry> {
        auto* left  = node.child(index);
        auto* right = node.child(index + 1);
        if (left->len >= B) {
            auto removed     = node.take_entry(index);
            auto predecessor = remove_max(*left);
            node.write_entry(index,
                             rstd::move(predecessor.template get<0>()),
                             rstd::move(predecessor.template get<1>()));
            return Some(rstd::move(removed));
        }
        if (right->len >= B) {
            auto removed   = node.take_entry(index);
            auto successor = remove_min(*right);
            node.write_entry(index,
                             rstd::move(successor.template get<0>()),
                             rstd::move(successor.template get<1>()));
            return Some(rstd::move(removed));
        }
        auto& merged = merge_children(node, index);
        return remove_from_node(merged, key);
    }

    template<typename Q>
    static auto remove_from_node(TreeNode& node, const Q& key) -> Option<Entry> {
        usize index = lower_bound(node, key);
        if (index < node.len && equivalent(node.key(index), key)) {
            if (node.leaf) return Some(node.remove_entry(index));
            return remove_from_internal(node, index, key);
        }
        if (node.leaf) return None();

        auto* child = node.child(index);
        if (child->len == B - 1) {
            if (index != 0 && node.edge(index - 1)->len >= B) {
                borrow_from_previous(node, index);
            } else if (index != node.len && node.edge(index + 1)->len >= B) {
                borrow_from_next(node, index);
            } else if (index != node.len) {
                child = rstd::addressof(merge_children(node, index));
            } else {
                child = rstd::addressof(merge_children(node, index - 1));
            }
        }
        return remove_from_node(*child, key);
    }

    void normalize_root() {
        auto* current = root_node();
        if (current == nullptr || current->len != 0 || current->leaf) return;
        auto old_root  = rstd::move(*root.take());
        auto new_root  = old_root->take_edge(0);
        old_root->leaf = true;
        root           = Some(rstd::move(new_root));
    }

    static void drain_node(Box<TreeNode> node, Vec<Entry>& output) {
        if (node->leaf) {
            while (node->len != 0) output.push(node->remove_entry(0));
            return;
        }

        while (node->len != 0) {
            auto child = remove_edge(*node.get(), 0, node->len + 1);
            drain_node(rstd::move(child), output);
            output.push(node->remove_entry(0));
        }
        auto child = node->take_edge(0);
        node->leaf = true;
        drain_node(rstd::move(child), output);
    }

    static bool validate_node(const TreeNode& node,
                              bool            is_root,
                              const K*        lower,
                              const K*        upper,
                              usize           depth,
                              usize&          leaf_depth,
                              bool&           saw_leaf,
                              usize&          count) {
        if (node.len > CAPACITY || (! is_root && node.len < B - 1)) return false;
        if (! node.leaf && node.len == 0) return false;
        for (usize i = 0; i < node.len; ++i) {
            const auto& key = node.key(i);
            if (lower != nullptr && ! (*lower < key)) return false;
            if (upper != nullptr && ! (key < *upper)) return false;
            if (i != 0 && ! (node.key(i - 1) < key)) return false;
        }
        count += node.len;
        if (node.leaf) {
            if (saw_leaf && leaf_depth != depth) return false;
            leaf_depth = depth;
            saw_leaf   = true;
            return true;
        }
        for (usize i = 0; i <= node.len; ++i) {
            const K* child_lower = i == 0 ? lower : rstd::addressof(node.key(i - 1));
            const K* child_upper = i == node.len ? upper : rstd::addressof(node.key(i));
            if (! validate_node(*node.child(i),
                                false,
                                child_lower,
                                child_upper,
                                depth + 1,
                                leaf_depth,
                                saw_leaf,
                                count)) {
                return false;
            }
        }
        return true;
    }

    bool valid() const {
        if (root.is_none()) return length == 0;
        usize leaf_depth = 0;
        usize count      = 0;
        bool  saw_leaf   = false;
        return validate_node(
                   *root_node(), true, nullptr, nullptr, 0, leaf_depth, saw_leaf, count) &&
               count == length;
    }

public:
    USE_TRAIT_WIN(BTreeMap)

    BTreeMap(): root(None()), length(0) {}
    BTreeMap(const BTreeMap&)            = delete;
    BTreeMap& operator=(const BTreeMap&) = delete;
    BTreeMap(BTreeMap&& other) noexcept: root(other.root.take()), length(other.length) {
        other.length = 0;
    }
    BTreeMap& operator=(BTreeMap&& other) noexcept {
        if (this != rstd::addressof(other)) {
            clear();
            root         = other.root.take();
            length       = other.length;
            other.length = 0;
        }
        return *this;
    }

    static auto make() -> BTreeMap { return {}; }

    auto len() const noexcept -> usize { return length; }
    auto is_empty() const noexcept -> bool { return length == 0; }

    auto clone() const -> BTreeMap
        requires rstd::Impled<K, rstd::clone::Clone> && rstd::Impled<V, rstd::clone::Clone>
    {
        auto result = BTreeMap::make();
        auto source = iter();
        for (auto item = source.next(); item.is_some(); item = source.next()) {
            result.insert(rstd::as<rstd::clone::Clone>(*item->template get<0>()).clone(),
                          rstd::as<rstd::clone::Clone>(*item->template get<1>()).clone());
        }
        return result;
    }

    void clone_from(BTreeMap& source)
        requires rstd::Impled<K, rstd::clone::Clone> && rstd::Impled<V, rstd::clone::Clone>
    {
        *this = source.clone();
    }

    void clear() {
        root   = None();
        length = 0;
    }

    auto insert(K key, V value) -> Option<V> {
        if (root.is_none()) root = Some(Box<TreeNode>::make(true));
        if (root_node()->len == CAPACITY) {
            auto old_root = rstd::move(*root.take());
            auto new_root = Box<TreeNode>::make(false);
            new_root->write_edge(0, rstd::move(old_root));
            split_child(*new_root.get(), 0);
            root = Some(rstd::move(new_root));
        }
        auto old = insert_non_full(*root_node(), rstd::move(key), rstd::move(value));
        debug_assert(valid());
        return old;
    }

    template<typename Q>
    auto get(const Q& key) const -> Option<rstd::ref<V>>
        requires requires(const K& stored, const Q& borrowed) {
            stored < borrowed;
            borrowed < stored;
        }
    {
        auto* node = root_node();
        while (node != nullptr) {
            usize index = lower_bound(*node, key);
            if (index < node->len && equivalent(node->key(index), key)) {
                return Some(rstd::ref<V>::from_raw_parts(rstd::addressof(node->value(index))));
            }
            if (node->leaf) break;
            node = node->child(index);
        }
        return None();
    }

    template<typename Q>
    auto get_mut(const Q& key) -> Option<rstd::mut_ref<V>>
        requires requires(const K& stored, const Q& borrowed) {
            stored < borrowed;
            borrowed < stored;
        }
    {
        auto* node = root_node();
        while (node != nullptr) {
            usize index = lower_bound(*node, key);
            if (index < node->len && equivalent(node->key(index), key)) {
                return Some(rstd::mut_ref<V>::from_raw_parts(rstd::addressof(node->value(index))));
            }
            if (node->leaf) break;
            node = node->child(index);
        }
        return None();
    }

    template<typename Q>
    auto get_key_value(const Q& key) const -> Option<rstd::tuple<rstd::ref<K>, rstd::ref<V>>>
        requires requires(const K& stored, const Q& borrowed) {
            stored < borrowed;
            borrowed < stored;
        }
    {
        auto* node = root_node();
        while (node != nullptr) {
            usize index = lower_bound(*node, key);
            if (index < node->len && equivalent(node->key(index), key)) {
                return Some(rstd::tuple<rstd::ref<K>, rstd::ref<V>>(
                    rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
                    rstd::ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
            }
            if (node->leaf) break;
            node = node->child(index);
        }
        return None();
    }

    template<typename Q>
    auto contains_key(const Q& key) const -> bool
        requires requires(const K& stored, const Q& borrowed) {
            stored < borrowed;
            borrowed < stored;
        }
    {
        return get(key).is_some();
    }

    template<typename Q>
    auto remove_entry(const Q& key) -> Option<Entry>
        requires requires(const K& stored, const Q& borrowed) {
            stored < borrowed;
            borrowed < stored;
        }
    {
        if (root.is_none()) return None();
        auto removed = remove_from_node(*root_node(), key);
        if (removed.is_some()) --length;
        normalize_root();
        debug_assert(valid());
        return removed;
    }

    template<typename Q>
    auto remove(const Q& key) -> Option<V>
        requires requires(const K& stored, const Q& borrowed) {
            stored < borrowed;
            borrowed < stored;
        }
    {
        auto entry = remove_entry(key);
        if (entry.is_none()) return None();
        return Some(rstd::move(entry->template get<1>()));
    }

    auto first_key_value() const -> Option<rstd::tuple<rstd::ref<K>, rstd::ref<V>>> {
        auto* node = root_node();
        if (node == nullptr || length == 0) return None();
        while (! node->leaf) node = node->child(0);
        return Some(rstd::tuple<rstd::ref<K>, rstd::ref<V>>(
            rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(0))),
            rstd::ref<V>::from_raw_parts(rstd::addressof(node->value(0)))));
    }

    auto last_key_value() const -> Option<rstd::tuple<rstd::ref<K>, rstd::ref<V>>> {
        auto* node = root_node();
        if (node == nullptr || length == 0) return None();
        while (! node->leaf) node = node->child(node->len);
        usize index = node->len - 1;
        return Some(rstd::tuple<rstd::ref<K>, rstd::ref<V>>(
            rstd::ref<K>::from_raw_parts(rstd::addressof(node->key(index))),
            rstd::ref<V>::from_raw_parts(rstd::addressof(node->value(index)))));
    }

    auto pop_first() -> Option<Entry> {
        if (length == 0) return None();
        auto entry = remove_min(*root_node());
        --length;
        normalize_root();
        debug_assert(valid());
        return Some(rstd::move(entry));
    }

    auto pop_last() -> Option<Entry> {
        if (length == 0) return None();
        auto entry = remove_max(*root_node());
        --length;
        normalize_root();
        debug_assert(valid());
        return Some(rstd::move(entry));
    }

    auto iter() const -> BTreeMapIter<K, V> { return { root_node(), length }; }
    auto iter_mut() -> BTreeMapIterMut<K, V> { return { root_node(), length }; }
    auto keys() const -> BTreeMapKeys<K, V> { return BTreeMapKeys<K, V>(iter()); }
    auto values() const -> BTreeMapValues<K, V> { return BTreeMapValues<K, V>(iter()); }
    auto values_mut() -> BTreeMapValuesMut<K, V> { return BTreeMapValuesMut<K, V>(iter_mut()); }

    using IntoIter = BTreeMapIntoIter<K, V>;
    auto into_iter() -> IntoIter {
        auto entries = Vec<Entry>::with_capacity(length);
        if (root.is_some()) drain_node(rstd::move(*root.take()), entries);
        length = 0;
        return IntoIter(rstd::move(entries));
    }
};

} // namespace alloc::collections

namespace rstd
{

template<typename K, typename V>
    requires requires(const K& left_key,
                      const K& right_key,
                      const V& left_value,
                      const V& right_value) {
        left_key == right_key;
        left_value == right_value;
    }
struct Impl<cmp::PartialEq<::alloc::collections::BTreeMap<K, V>>,
            ::alloc::collections::BTreeMap<K, V>>
    : DefaultInImpl<cmp::PartialEq<::alloc::collections::BTreeMap<K, V>>,
                    ::alloc::collections::BTreeMap<K, V>> {
    auto eq(const ::alloc::collections::BTreeMap<K, V>& other) const noexcept -> bool {
        auto& self = this->self();
        if (self.len() != other.len()) return false;

        auto left  = self.iter();
        auto right = other.iter();
        for (auto lhs = left.next(), rhs = right.next(); lhs.is_some();
             lhs = left.next(), rhs = right.next()) {
            if (*lhs->template get<0>() != *rhs->template get<0>() ||
                *lhs->template get<1>() != *rhs->template get<1>()) {
                return false;
            }
        }
        return true;
    }
};

template<typename K, typename V>
struct Impl<iter::FromIterator<tuple<K, V>>, ::alloc::collections::BTreeMap<K, V>>
    : ImplBase<::alloc::collections::BTreeMap<K, V>> {
    template<typename It>
    static auto from_iter(It iter) -> ::alloc::collections::BTreeMap<K, V> {
        auto map = ::alloc::collections::BTreeMap<K, V>::make();
        for (auto item = iter.next(); item.is_some(); item = iter.next()) {
            map.insert(rstd::move(item->template get<0>()), rstd::move(item->template get<1>()));
        }
        return map;
    }
};

template<typename K, typename V>
struct Impl<iter::IntoIterator, ::alloc::collections::BTreeMap<K, V>>
    : ImplBase<::alloc::collections::BTreeMap<K, V>> {
    auto into_iter() -> ::alloc::collections::BTreeMapIntoIter<K, V> {
        return this->self().into_iter();
    }
};

} // namespace rstd
