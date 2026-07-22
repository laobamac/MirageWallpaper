module;
#include <rstd/macro.hpp>

export module rstd.alloc:collections.hash_map;
export import :vec;
export import :hash.random;
export import :collections.hash_table;
export import rstd.core;

namespace alloc::collections
{

using ::alloc::collections::hash::detail::BucketState;
using ::alloc::collections::hash::detail::RawTable;
using ::alloc::vec::Vec;
using namespace rstd::prelude;

export template<typename K>
struct DefaultHashEqual {
    auto operator()(const K& left, const K& right) const noexcept -> bool { return left == right; }
};

export template<typename K,
                typename V,
                typename S  = rstd::hash::RandomState,
                typename Eq = DefaultHashEqual<K>>
class HashMap;
export template<typename K, typename V>
class HashMapIter;
export template<typename K, typename V>
class HashMapIterMut;
export template<typename K, typename V>
class HashMapIntoIter;
export template<typename K, typename V>
class HashMapKeys;
export template<typename K, typename V>
class HashMapValues;
export template<typename K, typename V>
class HashMapValuesMut;

export template<typename K, typename V>
class HashMapIter : public rstd::DefaultInClass<HashMapIter<K, V>, rstd::iter::Iterator> {
    const RawTable<K, V>* table;
    usize                 index;
    usize                 remaining;

public:
    using Item = rstd::tuple<rstd::ref<K>, rstd::ref<V>>;
    HashMapIter(const RawTable<K, V>* source, usize len): table(source), index(0), remaining(len) {}

    auto next() -> Option<Item> {
        while (remaining != 0 && index < table->bucket_count()) {
            const auto& bucket = table->bucket(index++);
            if (bucket.state != BucketState::Full) continue;
            --remaining;
            return Some(Item(rstd::ref<K>::from_raw_parts(rstd::addressof(bucket.key())),
                             rstd::ref<V>::from_raw_parts(rstd::addressof(bucket.value()))));
        }
        return None();
    }
    auto size_hint() const -> rstd::iter::SizeHint { return { remaining, Some(usize(remaining)) }; }
    auto len() const noexcept -> usize { return remaining; }
};

export template<typename K, typename V>
class HashMapIterMut : public rstd::DefaultInClass<HashMapIterMut<K, V>, rstd::iter::Iterator> {
    RawTable<K, V>* table;
    usize           index;
    usize           remaining;

public:
    using Item = rstd::tuple<rstd::ref<K>, rstd::mut_ref<V>>;
    HashMapIterMut(RawTable<K, V>* source, usize len): table(source), index(0), remaining(len) {}

    auto next() -> Option<Item> {
        while (remaining != 0 && index < table->bucket_count()) {
            auto& bucket = table->bucket(index++);
            if (bucket.state != BucketState::Full) continue;
            --remaining;
            return Some(Item(rstd::ref<K>::from_raw_parts(rstd::addressof(bucket.key())),
                             rstd::mut_ref<V>::from_raw_parts(rstd::addressof(bucket.value()))));
        }
        return None();
    }
    auto size_hint() const -> rstd::iter::SizeHint { return { remaining, Some(usize(remaining)) }; }
    auto len() const noexcept -> usize { return remaining; }
};

export template<typename K, typename V>
class HashMapKeys : public rstd::DefaultInClass<HashMapKeys<K, V>, rstd::iter::Iterator> {
    HashMapIter<K, V> inner;

public:
    using Item = rstd::ref<K>;
    explicit HashMapKeys(HashMapIter<K, V> iter): inner(rstd::move(iter)) {}
    auto next() -> Option<Item> {
        auto item = inner.next();
        return item.is_some() ? Some(item->template get<0>()) : None();
    }
    auto size_hint() const -> rstd::iter::SizeHint { return inner.size_hint(); }
    auto len() const noexcept -> usize { return inner.len(); }
};

export template<typename K, typename V>
class HashMapValues : public rstd::DefaultInClass<HashMapValues<K, V>, rstd::iter::Iterator> {
    HashMapIter<K, V> inner;

public:
    using Item = rstd::ref<V>;
    explicit HashMapValues(HashMapIter<K, V> iter): inner(rstd::move(iter)) {}
    auto next() -> Option<Item> {
        auto item = inner.next();
        return item.is_some() ? Some(item->template get<1>()) : None();
    }
    auto size_hint() const -> rstd::iter::SizeHint { return inner.size_hint(); }
    auto len() const noexcept -> usize { return inner.len(); }
};

export template<typename K, typename V>
class HashMapValuesMut : public rstd::DefaultInClass<HashMapValuesMut<K, V>, rstd::iter::Iterator> {
    HashMapIterMut<K, V> inner;

public:
    using Item = rstd::mut_ref<V>;
    explicit HashMapValuesMut(HashMapIterMut<K, V> iter): inner(rstd::move(iter)) {}
    auto next() -> Option<Item> {
        auto item = inner.next();
        return item.is_some() ? Some(item->template get<1>()) : None();
    }
    auto size_hint() const -> rstd::iter::SizeHint { return inner.size_hint(); }
    auto len() const noexcept -> usize { return inner.len(); }
};

export template<typename K, typename V>
class HashMapIntoIter : public rstd::DefaultInClass<HashMapIntoIter<K, V>, rstd::iter::Iterator> {
    RawTable<K, V> table;
    usize          index;

public:
    using Item = rstd::tuple<K, V>;
    explicit HashMapIntoIter(RawTable<K, V> source): table(rstd::move(source)), index(0) {}
    auto next() -> Option<Item> {
        while (index < table.bucket_count()) {
            usize current = index++;
            if (table.bucket(current).state == BucketState::Full) {
                return Some(table.remove(current));
            }
        }
        return None();
    }
    auto size_hint() const -> rstd::iter::SizeHint {
        return { table.len(), Some(usize(table.len())) };
    }
    auto len() const noexcept -> usize { return table.len(); }
};

export template<typename K, typename V, typename S, typename Eq>
class HashMap {
    using Entry = rstd::tuple<K, V>;

    RawTable<K, V> table;
    S              hash_builder;
    Eq             equal;

    auto hash_key(const K& key) const noexcept -> u64 {
        return static_cast<u64>(hash_builder(key));
    }

    auto find_index(const K& key) const -> Option<usize> {
        u64 hash = hash_key(key);
        return table.find(hash, [&](const K& stored) {
            return equal(stored, key);
        });
    }

public:
    USE_TRAIT_WIN(HashMap)
    using IntoIter = HashMapIntoIter<K, V>;

    HashMap(): table(), hash_builder(), equal() {}
    HashMap(const HashMap&)                = delete;
    HashMap& operator=(const HashMap&)     = delete;
    HashMap(HashMap&&) noexcept            = default;
    HashMap& operator=(HashMap&&) noexcept = default;

    static auto make() -> HashMap { return {}; }
    static auto with_capacity(usize capacity) -> HashMap { return HashMap(capacity, S {}, Eq {}); }
    static auto with_hasher(S hasher) -> HashMap { return HashMap(0, rstd::move(hasher), Eq {}); }
    static auto with_capacity_and_hasher(usize capacity, S hasher) -> HashMap {
        return HashMap(capacity, rstd::move(hasher), Eq {});
    }

    HashMap(usize capacity, S hasher, Eq equality)
        : table(capacity), hash_builder(rstd::move(hasher)), equal(rstd::move(equality)) {}

    auto len() const noexcept -> usize { return table.len(); }
    auto is_empty() const noexcept -> bool { return table.len() == 0; }
    auto capacity() const noexcept -> usize { return table.capacity(); }
    auto hasher() const noexcept -> const S& { return hash_builder; }

    void reserve(usize additional) { table.reserve(additional); }
    void shrink_to_fit() { table.shrink_to(0); }
    void shrink_to(usize minimum) { table.shrink_to(minimum); }
    void clear() noexcept { table.clear(); }

    auto insert(K key, V value) -> Option<V> {
        u64  hash  = hash_key(key);
        auto found = table.find(hash, [&](const K& stored) {
            return equal(stored, key);
        });
        if (found.is_some()) {
            return Some(table.bucket(*found).replace_value(rstd::move(value)));
        }
        table.insert(hash, rstd::move(key), rstd::move(value));
        return None();
    }

    auto get(const K& key) const -> Option<rstd::ref<V>> {
        auto found = find_index(key);
        if (found.is_none()) return None();
        return Some(rstd::ref<V>::from_raw_parts(rstd::addressof(table.bucket(*found).value())));
    }

    auto get_mut(const K& key) -> Option<rstd::mut_ref<V>> {
        auto found = find_index(key);
        if (found.is_none()) return None();
        return Some(
            rstd::mut_ref<V>::from_raw_parts(rstd::addressof(table.bucket(*found).value())));
    }

    auto get_key_value(const K& key) const -> Option<rstd::tuple<rstd::ref<K>, rstd::ref<V>>> {
        auto found = find_index(key);
        if (found.is_none()) return None();
        const auto& bucket = table.bucket(*found);
        return Some(rstd::tuple<rstd::ref<K>, rstd::ref<V>>(
            rstd::ref<K>::from_raw_parts(rstd::addressof(bucket.key())),
            rstd::ref<V>::from_raw_parts(rstd::addressof(bucket.value()))));
    }

    auto contains_key(const K& key) const -> bool { return find_index(key).is_some(); }

    auto remove_entry(const K& key) -> Option<Entry> {
        auto found = find_index(key);
        return found.is_some() ? Some(table.remove(*found)) : None();
    }

    auto remove(const K& key) -> Option<V> {
        auto entry = remove_entry(key);
        return entry.is_some() ? Some(rstd::move(entry->template get<1>())) : None();
    }

    template<typename F>
    void retain(F predicate) {
        for (usize i = 0; i < table.bucket_count(); ++i) {
            auto& bucket = table.bucket(i);
            if (bucket.state == BucketState::Full && ! predicate(bucket.key(), bucket.value())) {
                (void)table.remove(i);
            }
        }
    }

    auto iter() const -> HashMapIter<K, V> { return { rstd::addressof(table), table.len() }; }
    auto iter_mut() -> HashMapIterMut<K, V> { return { rstd::addressof(table), table.len() }; }
    auto keys() const -> HashMapKeys<K, V> { return HashMapKeys<K, V>(iter()); }
    auto values() const -> HashMapValues<K, V> { return HashMapValues<K, V>(iter()); }
    auto values_mut() -> HashMapValuesMut<K, V> { return HashMapValuesMut<K, V>(iter_mut()); }
    auto into_iter() -> IntoIter { return IntoIter(rstd::move(table)); }
};

} // namespace alloc::collections

namespace rstd
{

template<typename K, typename V, typename S, typename Eq>
struct Impl<iter::FromIterator<tuple<K, V>>, ::alloc::collections::HashMap<K, V, S, Eq>>
    : ImplBase<::alloc::collections::HashMap<K, V, S, Eq>> {
    template<typename It>
    static auto from_iter(It iter) -> ::alloc::collections::HashMap<K, V, S, Eq> {
        auto map = ::alloc::collections::HashMap<K, V, S, Eq>::make();
        for (auto item = iter.next(); item.is_some(); item = iter.next()) {
            map.insert(rstd::move(item->template get<0>()), rstd::move(item->template get<1>()));
        }
        return map;
    }
};

template<typename K, typename V, typename S, typename Eq>
struct Impl<iter::IntoIterator, ::alloc::collections::HashMap<K, V, S, Eq>>
    : ImplBase<::alloc::collections::HashMap<K, V, S, Eq>> {
    auto into_iter() -> ::alloc::collections::HashMapIntoIter<K, V> {
        return this->self().into_iter();
    }
};

} // namespace rstd
