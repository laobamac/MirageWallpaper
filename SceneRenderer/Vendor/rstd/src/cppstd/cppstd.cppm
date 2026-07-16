module;
// c
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// c++11
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <queue>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// c++17
#include <charconv>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <variant>

// c++20
#include <bit>
#include <coroutine>
#include <format>
#include <ranges>
#include <source_location>
#include <span>

#define ALWAYS_INLINE [[gnu::always_inline]] inline

export module cppstd;

export using ::operator new;
export using ::operator delete;

// ============================================================================
// All re-exports live in `std::` namespace. Consumers `import cppstd;` and
// then use names as `std::vector`, `std::string`, etc. — same as if they had
// `#include <vector>` etc. but going through one shared module BMI.
//
// Plus: a small set of C-attached typedefs that <cstdint>/<cstddef> normally
// leak to the global namespace via the platform's C-compat layer. Re-exporting
// these at global scope keeps existing "unprefixed" usages (`int32_t`, `size_t`)
// working across the migration without per-call-site `std::` qualification.
// ============================================================================

export using ::int8_t;
export using ::int16_t;
export using ::int32_t;
export using ::int64_t;
export using ::uint8_t;
export using ::uint16_t;
export using ::uint32_t;
export using ::uint64_t;
export using ::intptr_t;
export using ::uintptr_t;
export using ::size_t;
export using ::ptrdiff_t;
export using ::FILE;
export using ::va_list;
export using ::time_t;
export using ::clock_t;

namespace std
{

// ==== c ====

// <cstdint>
export using std::int8_t;
export using std::int16_t;
export using std::int32_t;
export using std::int64_t;
export using std::uint8_t;
export using std::uint16_t;
export using std::uint32_t;
export using std::uint64_t;
export using std::intptr_t;
export using std::uintptr_t;

// <cstddef>
export using std::nullptr_t;
export using std::ptrdiff_t;
export using std::size_t;

// <cstdlib>
export using std::abort;
export using std::atoi;
export using std::exit;
export using std::free;
export using std::getenv;
export using std::malloc;
export using std::strtod;
export using std::strtol;
export using std::strtoul;
export using std::system;

// <cstdio>
export using std::FILE;
export using std::fclose;
export using std::fflush;
export using std::fgets;
export using std::fopen;
export using std::fread;
export using std::fseek;
export using std::ftell;
export using std::fwrite;
export using std::sscanf;
export using std::vfprintf;
#if 0
export using std::printf;
export using std::fprintf;
export using std::snprintf;
export using std::sprintf;
#endif

// <cstring>
export using std::memcmp;
export using std::memcpy;
export using std::memmove;
export using std::memset;
export using std::strchr;
export using std::strcmp;
export using std::strcpy;
export using std::strerror;
export using std::strlen;
export using std::strncmp;
export using std::strncpy;
export using std::strstr;

// <cctype>
export using std::isalnum;
export using std::isalpha;
export using std::isblank;
export using std::iscntrl;
export using std::isdigit;
export using std::isgraph;
export using std::islower;
export using std::isprint;
export using std::ispunct;
export using std::isspace;
export using std::isupper;
export using std::isxdigit;
export using std::tolower;
export using std::toupper;

// <cmath>
export using std::abs;
export using std::acos;
export using std::asin;
export using std::atan;
export using std::atan2;
export using std::ceil;
export using std::cos;
export using std::exp;
export using std::fabs;
export using std::floor;
export using std::fmod;
export using std::log;
export using std::log2;
export using std::log10;
export using std::lerp;
export using std::nextafter;
export using std::pow;
export using std::round;
export using std::sin;
export using std::sqrt;
export using std::tan;
export using std::isnan;
export using std::isfinite;
export using std::isinf;

// <ctime>
export using std::clock;
export using std::clock_t;
#if ! defined(_WIN32)
export using std::localtime;
export using std::time;
#endif
export using std::time_t;
export using std::tm;

// ==== c++11 ====

// <type_traits>
export using std::false_type;
export using std::integral_constant;
export using std::true_type;
export using std::bool_constant;

export using std::enable_if;
export using std::enable_if_t;

export using std::conditional;
export using std::conditional_t;

export using std::decay;
export using std::decay_t;

export using std::underlying_type;
export using std::underlying_type_t;

export using std::make_signed;
export using std::make_signed_t;
export using std::make_unsigned;
export using std::make_unsigned_t;

export using std::is_arithmetic;
export using std::is_arithmetic_v;
export using std::is_array;
export using std::is_array_v;
export using std::is_base_of;
export using std::is_base_of_v;
export using std::is_class;
export using std::is_class_v;
export using std::is_const;
export using std::is_const_v;
export using std::is_constructible;
export using std::is_constructible_v;
export using std::is_default_constructible;
export using std::is_default_constructible_v;
export using std::is_enum;
export using std::is_enum_v;
export using std::is_floating_point;
export using std::is_floating_point_v;
export using std::is_integral;
export using std::is_integral_v;
export using std::is_lvalue_reference;
export using std::is_lvalue_reference_v;
export using std::is_pointer;
export using std::is_pointer_v;
export using std::is_reference;
export using std::is_reference_v;
export using std::is_rvalue_reference;
export using std::is_rvalue_reference_v;
export using std::is_same;
export using std::is_same_v;
export using std::is_signed;
export using std::is_signed_v;
export using std::is_standard_layout;
export using std::is_standard_layout_v;
export using std::is_union;
export using std::is_union_v;
export using std::is_void;
export using std::is_void_v;

export using std::add_const;
export using std::add_const_t;
export using std::add_lvalue_reference;
export using std::add_lvalue_reference_t;
export using std::add_pointer;
export using std::add_pointer_t;
export using std::add_rvalue_reference;
export using std::add_rvalue_reference_t;

export using std::remove_const;
export using std::remove_const_t;
export using std::remove_cv;
export using std::remove_cv_t;
export using std::remove_extent;
export using std::remove_extent_t;
export using std::remove_pointer;
export using std::remove_pointer_t;
export using std::remove_reference;
export using std::remove_reference_t;

export using std::is_nothrow_constructible;
export using std::is_nothrow_constructible_v;
export using std::is_nothrow_default_constructible;
export using std::is_nothrow_default_constructible_v;
export using std::is_nothrow_destructible;
export using std::is_nothrow_destructible_v;
export using std::is_trivially_destructible;
export using std::is_trivially_destructible_v;

export using std::is_copy_constructible;
export using std::is_copy_constructible_v;
export using std::is_nothrow_copy_constructible;
export using std::is_nothrow_copy_constructible_v;
export using std::is_trivially_copy_constructible;
export using std::is_trivially_copy_constructible_v;
export using std::is_trivially_copyable;
export using std::is_trivially_copyable_v;

export using std::is_move_constructible;
export using std::is_move_constructible_v;
export using std::is_nothrow_move_constructible;
export using std::is_nothrow_move_constructible_v;
export using std::is_trivially_move_constructible;
export using std::is_trivially_move_constructible_v;

export using std::is_copy_assignable;
export using std::is_copy_assignable_v;
export using std::is_nothrow_copy_assignable;
export using std::is_nothrow_copy_assignable_v;
export using std::is_trivially_copy_assignable;
export using std::is_trivially_copy_assignable_v;

export using std::is_move_assignable;
export using std::is_move_assignable_v;
export using std::is_nothrow_move_assignable;
export using std::is_nothrow_move_assignable_v;
export using std::is_trivially_move_assignable;
export using std::is_trivially_move_assignable_v;

export using std::is_nothrow_assignable;
export using std::is_nothrow_assignable_v;

export using std::is_convertible;
export using std::is_convertible_v;

export using std::alignment_of;
export using std::alignment_of_v;

// <limits>
export using std::numeric_limits;

// <new>
export using std::bad_alloc;
export using std::max_align_t;
export using std::nothrow;
export using std::nothrow_t;

// <atomic>
export using std::atomic;
export using std::atomic_thread_fence;
export using std::atomic_uint32_t;
export using std::atomic_uint64_t;
export using std::memory_order;
export using std::memory_order_acq_rel;
export using std::memory_order_acquire;
export using std::memory_order_relaxed;
export using std::memory_order_release;
export using std::memory_order_seq_cst;

// <bitset>
export using std::bitset;

// <string>
export using std::basic_string;
export using std::char_traits;
export using std::getline;
export using std::stod;
export using std::stof;
export using std::stoi;
export using std::stol;
export using std::stoll;
export using std::stoul;
export using std::stoull;
export using std::string;
export using std::to_string;
export using std::to_wstring;
export using std::wstring;

// <cstdarg>
export using std::va_list;

// <fstream>
export using std::basic_fstream;
export using std::basic_ifstream;
export using std::basic_ofstream;
export using std::fstream;
export using std::ifstream;
export using std::ofstream;

// <iostream>
export using std::cerr;
export using std::cin;
export using std::clog;
export using std::cout;
export using std::endl;
export using std::flush;
export using std::ostream;
export using std::istream;
export using std::iostream;
export using std::ios;
export using std::ios_base;
export using std::streampos;
export using std::streamsize;

// <sstream>
export using std::basic_istringstream;
export using std::basic_ostringstream;
export using std::basic_stringstream;
export using std::istringstream;
export using std::ostringstream;
export using std::stringstream;

// <regex>
export using std::cmatch;
export using std::regex;
export using std::regex_error;
export using std::regex_match;
export using std::regex_replace;
export using std::regex_search;
export using std::smatch;
export using std::sregex_iterator;
export using std::ssub_match;

namespace regex_constants
{
export using std::regex_constants::operator|;
export using std::regex_constants::operator|=;
export using std::regex_constants::syntax_option_type;
} // namespace regex_constants

// <algorithm>
export using std::all_of;
export using std::any_of;
export using std::binary_search;
export using std::clamp;
export using std::copy;
export using std::copy_if;
export using std::copy_n;
export using std::count;
export using std::count_if;
export using std::distance;
export using std::equal;
export using std::fill;
export using std::fill_n;
export using std::find;
export using std::find_first_of;
export using std::find_if;
export using std::find_if_not;
export using std::for_each;
export using std::is_sorted;
export using std::iter_swap;
export using std::lower_bound;
export using std::max;
export using std::max_element;
export using std::min;
export using std::min_element;
export using std::minmax;
export using std::mismatch;
export using std::none_of;
export using std::nth_element;
export using std::partition;
export using std::remove;
export using std::remove_if;
export using std::replace;
export using std::replace_if;
export using std::reverse;
export using std::rotate;
export using std::search;
export using std::sort;
export using std::stable_sort;
export using std::swap_ranges;
export using std::transform;
export using std::unique;
export using std::upper_bound;

// <numeric>
export using std::accumulate;
export using std::adjacent_difference;
export using std::iota;
export using std::partial_sum;
export using std::reduce;

// <memory>
export using std::addressof;
export using std::allocator;
export using std::allocator_traits;
export using std::default_delete;
export using std::enable_shared_from_this;
export using std::make_shared;
export using std::shared_ptr;
export using std::unique_ptr;
export using std::weak_ptr;

// <functional>
export using std::bind;
export using std::cref;
export using std::function;
export using std::greater;
export using std::greater_equal;
export using std::hash;
export using std::less;
export using std::less_equal;
export using std::reference_wrapper;
export using std::ref;

namespace placeholders
{
export using std::placeholders::_1;
export using std::placeholders::_2;
export using std::placeholders::_3;
export using std::placeholders::_4;
export using std::placeholders::_5;
} // namespace placeholders

// <tuple>
export using std::get;
export using std::ignore;
export using std::make_tuple;
export using std::tie;
export using std::tuple;
// tuple_element / tuple_size already exported above for structured bindings

// <utility>
export using std::declval;
export using std::forward;
export using std::move;
export using std::pair;
export using std::make_pair;
export using std::swap;

// <iterator>
export using std::advance;
export using std::back_inserter;
// back_insert_iterator + iterator already exported above
export using std::begin;
export using std::cbegin;
export using std::cend;
export using std::end;
export using std::front_inserter;
export using std::inserter;
export using std::next;
export using std::ostream_iterator;
export using std::prev;
export using std::rbegin;
export using std::rend;
export using std::reverse_iterator;
export using std::ssize;
export using std::size;
export using std::data;

// containers
export using std::array;
export using std::deque;
export using std::list;
export using std::map;
export using std::multimap;
export using std::multiset;
export using std::set;
export using std::stack;
export using std::queue;
export using std::priority_queue;
export using std::unordered_map;
export using std::unordered_multimap;
export using std::unordered_multiset;
export using std::unordered_set;
export using std::vector;

// <initializer_list>
export using std::initializer_list;

// <mutex>
export using std::adopt_lock;
export using std::call_once;
export using std::defer_lock;
export using std::lock;
export using std::lock_guard;
export using std::mutex;
export using std::once_flag;
export using std::recursive_mutex;
export using std::scoped_lock;
export using std::try_to_lock;
export using std::unique_lock;

// <condition_variable>
export using std::condition_variable;
export using std::condition_variable_any;
export using std::cv_status;

// <future>
export using std::async;
export using std::future;
export using std::future_status;
export using std::launch;
export using std::packaged_task;
export using std::promise;
export using std::shared_future;

// <thread>
export using std::thread;
namespace this_thread
{
export using std::this_thread::get_id;
export using std::this_thread::sleep_for;
export using std::this_thread::sleep_until;
export using std::this_thread::yield;
} // namespace this_thread

// <chrono>
namespace chrono
{
export using std::chrono::duration;
export using std::chrono::duration_cast;
export using std::chrono::high_resolution_clock;
export using std::chrono::hours;
export using std::chrono::microseconds;
export using std::chrono::milliseconds;
export using std::chrono::minutes;
export using std::chrono::nanoseconds;
export using std::chrono::seconds;
export using std::chrono::steady_clock;
export using std::chrono::system_clock;
export using std::chrono::time_point;
export using std::chrono::time_point_cast;
export using std::chrono::operator-;
export using std::chrono::operator+;
export using std::chrono::operator*;
export using std::chrono::operator/;
export using std::chrono::operator%;
export using std::chrono::operator>;
export using std::chrono::operator<;
export using std::chrono::operator<=;
export using std::chrono::operator>=;
export using std::chrono::operator==;
} // namespace chrono

// <random>
export using std::bernoulli_distribution;
export using std::default_random_engine;
export using std::mt19937;
export using std::mt19937_64;
export using std::normal_distribution;
export using std::random_device;
export using std::uniform_int_distribution;
export using std::uniform_real_distribution;

// <stdexcept>
export using std::domain_error;
export using std::invalid_argument;
export using std::length_error;
export using std::logic_error;
export using std::out_of_range;
export using std::overflow_error;
export using std::range_error;
export using std::runtime_error;
export using std::underflow_error;

// <system_error>
export using std::errc;
export using std::error_category;
export using std::error_code;
export using std::error_condition;
export using std::generic_category;
export using std::system_category;
export using std::system_error;

// <exception>
export using std::current_exception;
export using std::exception;
export using std::exception_ptr;
export using std::make_exception_ptr;
export using std::rethrow_exception;
export using std::terminate;
#if __cplusplus < 202002L
export using std::uncaught_exception;
#endif
export using std::uncaught_exceptions;

// ==== c++14 ====

// <utility>
export using std::exchange;
export using std::index_sequence;
export using std::index_sequence_for;
export using std::integer_sequence;
export using std::make_index_sequence;
export using std::make_integer_sequence;

// <memory>
export using std::make_unique;

// ==== c++17 ====

// <type_traits>
export using std::conjunction;
export using std::conjunction_v;
export using std::disjunction;
export using std::disjunction_v;
export using std::is_aggregate;
export using std::is_aggregate_v;
export using std::is_invocable;
export using std::is_invocable_r;
export using std::is_invocable_r_v;
export using std::is_invocable_v;
export using std::invoke_result;
export using std::invoke_result_t;
export using std::negation;
export using std::negation_v;
export using std::void_t;

// <cstddef>
export using std::byte;
export using std::to_integer;

// <new>
export using std::align_val_t;
export using std::launder;

// <string_view>
export using std::basic_string_view;
export using std::string_view;
export using std::wstring_view;

// <optional>
export using std::bad_optional_access;
export using std::make_optional;
export using std::nullopt;
export using std::nullopt_t;
export using std::optional;

// <variant>
export using std::bad_variant_access;
export using std::get_if;
export using std::holds_alternative;
export using std::monostate;
export using std::variant;
export using std::variant_alternative;
export using std::variant_alternative_t;
export using std::variant_npos;
export using std::variant_size;
export using std::variant_size_v;
export using std::visit;

// <functional>
export using std::invoke;
export using std::not_fn;

// <tuple>
export using std::apply;
export using std::make_from_tuple;

// <charconv>
export using std::from_chars;
export using std::from_chars_result;
export using std::to_chars;
export using std::to_chars_result;

// <memory>
export using std::destroy;
export using std::destroy_at;
export using std::destroy_n;
export using std::uninitialized_copy;
export using std::uninitialized_copy_n;
export using std::uninitialized_default_construct;
export using std::uninitialized_default_construct_n;
export using std::uninitialized_fill;
export using std::uninitialized_fill_n;
export using std::uninitialized_move;
export using std::uninitialized_value_construct;

// <shared_mutex>
export using std::shared_lock;
export using std::shared_mutex;
export using std::shared_timed_mutex;

// <filesystem>
namespace filesystem
{
export using std::filesystem::absolute;
export using std::filesystem::canonical;
export using std::filesystem::copy;
export using std::filesystem::create_directories;
export using std::filesystem::create_directory;
export using std::filesystem::current_path;
export using std::filesystem::begin;
export using std::filesystem::end;
export using std::filesystem::directory_entry;
export using std::filesystem::directory_iterator;
export using std::filesystem::directory_options;
export using std::filesystem::exists;
export using std::filesystem::file_size;
export using std::filesystem::file_status;
export using std::filesystem::file_type;
export using std::filesystem::is_directory;
export using std::filesystem::is_regular_file;
export using std::filesystem::path;
export using std::filesystem::read_symlink;
export using std::filesystem::recursive_directory_iterator;
export using std::filesystem::relative;
export using std::filesystem::remove;
export using std::filesystem::remove_all;
export using std::filesystem::rename;
export using std::filesystem::status;
export using std::filesystem::temp_directory_path;
} // namespace filesystem

// <memory_resource>
namespace pmr
{
export using std::pmr::deque;
export using std::pmr::get_default_resource;
export using std::pmr::memory_resource;
export using std::pmr::polymorphic_allocator;
export using std::pmr::set_default_resource;
export using std::pmr::synchronized_pool_resource;
export using std::pmr::unsynchronized_pool_resource;
export using std::pmr::vector;
} // namespace pmr

// ==== c++20 ====

// <type_traits>
export using std::remove_cvref;
export using std::remove_cvref_t;
export using std::type_identity;
export using std::type_identity_t;

// <concepts>
export using std::constructible_from;
export using std::convertible_to;
export using std::derived_from;
export using std::destructible;
export using std::integral;
export using std::same_as;
export using std::semiregular;
export using std::signed_integral;
export using std::unsigned_integral;

// <compare> (already exported strong_ordering / weak_ordering at top)
export using std::partial_ordering;

// <bit>
export using std::bit_cast;
export using std::bit_ceil;
export using std::bit_floor;
export using std::bit_width;
export using std::countl_one;
export using std::countl_zero;
export using std::countr_one;
export using std::countr_zero;
export using std::has_single_bit;
export using std::popcount;

// <format>
export using std::basic_format_context;
export using std::basic_format_parse_context;
export using std::basic_format_string;
export using std::format;
export using std::format_args;
export using std::format_string;
export using std::format_to;
export using std::format_to_n;
export using std::formatted_size;
export using std::formatter;
export using std::make_format_args;
export using std::vformat;
export using std::vformat_to;

// <source_location>
export using std::source_location;

// <ranges>
namespace ranges
{
export using std::ranges::begin;
export using std::ranges::data;
export using std::ranges::distance;
export using std::ranges::end;
export using std::ranges::range;
export using std::ranges::range_value_t;
export using std::ranges::size;
} // namespace ranges
namespace ranges::views
{
export using std::ranges::views::transform;
} // namespace ranges::views

// <algorithm>
export using std::lexicographical_compare_three_way;

// <memory>
export using std::construct_at;

// <iterator>
export using std::iter_value_t;

// <span>
export using std::as_bytes;
export using std::as_writable_bytes;
export using std::dynamic_extent;
export using std::span;

// ==== c++23 ====

// <functional>
#ifdef __cpp_lib_move_only_function
export using std::move_only_function;
#endif

} // namespace std

// libstdc++ vector::iterator is __gnu_cxx::__normal_iterator, with comparison
// operators defined as free functions in __gnu_cxx::. Without explicit export,
// ADL can't find them through `import cppstd;`. Non-portable but needed for
// libstdc++ — libc++ uses different iterator types, so this block is a no-op
// elsewhere (guarded by _GLIBCXX_RELEASE).
#if defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE <= 15
namespace __gnu_cxx
{
export using __gnu_cxx::operator==;
export using __gnu_cxx::operator<=>;
export using __gnu_cxx::operator-;
export using __gnu_cxx::operator+;
} // namespace __gnu_cxx
#endif

namespace std
{

// previously exported (kept for backward compat with prior cppstd block)
export using std::is_error_code_enum;
export using std::tuple_element;
export using std::tuple_element_t;
export using std::tuple_size;
export using std::tuple_size_v;
export using std::back_insert_iterator;
export using std::istreambuf_iterator;
export using std::iterator;
export using std::coroutine_handle;
export using std::coroutine_traits;
export using std::strong_ordering;
export using std::weak_ordering;

// Free-function operators in std:: that ADL needs visible at import sites.
// Without an explicit `export using`, GMF-attached overloads aren't reachable.
// libstdc++ vector::iterator is __gnu_cxx::__normal_iterator with hidden-friend
// operators in __gnu_cxx::; those are still reachable via ADL through the
// vector type even with these std exports active.
export using std::operator+;
#if defined(__APPLE__)
export using std::operator==;
export using std::operator!=;
export using std::operator<;
export using std::operator<=;
export using std::operator>;
export using std::operator>=;
#endif
export using std::operator<<;
export using std::operator>>;
export using std::operator|;
export using std::operator&;
export using std::operator^;
export using std::operator~;
export using std::operator|=;
export using std::operator&=;
export using std::operator^=;

} // namespace std
