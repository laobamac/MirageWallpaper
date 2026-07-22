module;

#include "effolkronium/random.hpp"

export module sr.core;
import rstd.cppstd;

// NoCopyMove (global scope, matches the original NoCopyMove.hpp)
export struct NoCopy {
protected:
    NoCopy()  = default;
    ~NoCopy() = default;

    NoCopy(const NoCopy&)            = delete;
    NoCopy& operator=(const NoCopy&) = delete;
};

export struct NoMove {
protected:
    NoMove()  = default;
    ~NoMove() = default;

    NoMove(NoMove&&)            = delete;
    NoMove& operator=(NoMove&&) = delete;
};

export namespace sr
{

// Literals
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using idx   = std::ptrdiff_t;
using usize = std::size_t;
using isize = std::ptrdiff_t;

inline std::intptr_t Ptr2Int(void* p) noexcept { return reinterpret_cast<std::intptr_t>(p); }

// StringHelper
constexpr bool sstart_with(std::string_view str, std::string_view start) {
    return str.size() >= start.size() && str.compare(0, start.size(), start, 0, start.size()) == 0;
}
constexpr bool send_with(std::string_view str, std::string_view end) {
    return str.size() >= end.size() &&
           str.compare(str.size() - end.size(), end.size(), end, 0, end.size()) == 0;
}
inline std::string_view sview_nullsafe(const char* const s) {
    return std::string_view(s != nullptr ? s : "");
}

// MapSet
template<class Key, class Value>
using Map = std::map<Key, Value, std::less<>>;

template<class Key>
using Set = std::set<Key, std::less<>>;

// Transparent hash + equal so a string-keyed unordered_map can be probed with a
// std::string_view (or const char*) without materialising a std::string. Used
// for hot-path lookups where std::map's O(log n) node-chasing shows up in a
// per-frame profile but iteration order is irrelevant.
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view> {}(s);
    }
    std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view> {}(s);
    }
    std::size_t operator()(const char* s) const noexcept {
        return std::hash<std::string_view> {}(std::string_view { s });
    }
};

template<class Value>
using HashMap = std::unordered_map<std::string, Value, TransparentStringHash, std::equal_to<>>;

template<class Key, class Value, class KeyLike, class Allocator>
inline bool exists(const std::map<Key, Value, std::less<>, Allocator>& m,
                   const KeyLike&                                      key) noexcept {
    auto iter = m.find(key);
    return iter != m.end();
}

template<class Value, class KeyLike, class Allocator>
inline bool exists(
    const std::unordered_map<std::string, Value, TransparentStringHash, std::equal_to<>, Allocator>&
                   m,
    const KeyLike& key) noexcept {
    auto iter = m.find(key);
    return iter != m.end();
}

template<class Key, class KeyLike, class Allocator>
inline bool exists(const std::set<Key, std::less<>, Allocator>& m, const KeyLike& key) noexcept {
    auto iter = m.find(key);
    return iter != m.end();
}

// ArrayHelper
template<typename T, typename Tarray>
std::array<T, std::tuple_size<Tarray>::value> array_cast(const Tarray& array) noexcept {
    std::array<T, std::tuple_size<Tarray>::value> res;
    std::copy(array.begin(), array.end(), res.begin());
    return res;
}

template<typename S, typename TFunc, typename TR = std::invoke_result_t<TFunc, S>>
std::vector<TR> transform(std::span<const S> src, TFunc&& func) {
    std::vector<TR> dst(std::size(src));
    std::transform(std::begin(src), std::end(src), std::begin(dst), func);
    return dst;
}

template<typename T>
class spanone {
public:
    using value_type = T;
    using size_type  = std::size_t;
    using reference  = T&;
    using pointer    = T*;

    constexpr spanone(reference value) noexcept: ptr { &value } {}
    constexpr pointer   data() const noexcept { return ptr; }
    constexpr size_type size() const noexcept { return 1; }
    constexpr reference operator[](std::size_t index) const noexcept { return ptr[index]; }
    constexpr pointer   begin() const noexcept { return ptr; }
    constexpr pointer   end() const noexcept { return ptr + 1; }
    constexpr pointer   cbegin() const noexcept { return ptr; }
    constexpr pointer   cend() const noexcept { return ptr + 1; }

private:
    pointer ptr;
};

// Visitors
namespace visitor
{

template<class... Ts>
struct overload : Ts... {
    using Ts::operator()...;
};
template<class... Ts>
overload(Ts...) -> overload<Ts...>;

struct EqualVisitor {
    using result_type = bool;

    template<typename T, typename U>
    bool operator()(const T&, const U&) const {
        return false;
    }

    template<typename T>
    bool operator()(const T& v1, const T& v2) const {
        return v1 == v2;
    }
};

} // namespace visitor

// Random
using Random = effolkronium::random_thread_local;

// BlockingQueue
template<typename T>
class BlockingQueue {
public:
    using value_type      = T;
    using size_type       = std::size_t;
    using reference       = T&;
    using const_reference = const T&;

    using lock_type = std::unique_lock<std::mutex>;

    constexpr static usize DEF_CAPACITY { 20 };

    BlockingQueue(const usize cap = DEF_CAPACITY): m_capacity(cap) {}
    ~BlockingQueue() = default;

    BlockingQueue(const BlockingQueue&)            = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;
    BlockingQueue(BlockingQueue&&)                 = delete;
    BlockingQueue& operator=(BlockingQueue&&)      = delete;

    void full() const {
        lock_type lock(m_op_mtx);
        return full_();
    }
    void empty() const {
        lock_type lock(m_op_mtx);
        return empty_();
    }
    void size() const {
        lock_type lock(m_op_mtx);
        return size_();
    }

    void push(const value_type& item) {
        lock_type lock(m_op_mtx);
        while (full_()) {
            m_cond_not_full.wait(lock);
        }
        m_queue.push(item);
        m_cond_not_empty.notify_all();
    }

    T pop() {
        lock_type lock(m_op_mtx);
        while (empty_()) {
            m_cond_not_empty.wait(lock);
        }
        T front_item { m_queue.pop() };
        m_cond_not_full.notify_all();
        return front_item;
    }

    T front() {
        lock_type lock(m_op_mtx);
        return m_queue.front();
    }
    T back() {
        lock_type lock(m_op_mtx);
        return m_queue.back();
    }

private:
    void full_() const { return m_capacity == m_queue.size(); }
    void empty_() const { return m_queue.empty(); }
    void size_() const { return m_queue.size(); }

private:
    mutable std::mutex      m_op_mtx;
    std::condition_variable m_cond_not_full;
    std::condition_variable m_cond_not_empty;
    std::queue<T>           m_queue;
    usize                   m_capacity;
};

} // namespace sr
