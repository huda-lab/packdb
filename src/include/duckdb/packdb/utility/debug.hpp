#ifndef DEBUG_HPP
#define DEBUG_HPP

#include <map>
#include <iostream>
#include <iterator>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <vector>

namespace packdb{
extern const char* RED;
extern const char* GREEN;
extern const char* RESET;

template <typename T,typename U>                                                   
std::pair<T,U> operator+(const std::pair<T,U> & l,const std::pair<T,U> & r) {   
	return {l.first+r.first,l.second+r.second};                                    
}

class Profiler{
private:
	std::map<std::string, std::pair<double, int>> clocks;
	std::map<std::string, std::chrono::time_point<std::chrono::high_resolution_clock>> timePoints;
public:
	Profiler();
	void clock(const std::string& label="");
	void stop(const std::string& label="");
	void add(const Profiler& pro);
	void print() const;
};

// Type trait for checking if a type supports default output to std::cout
template <class T, class = void>
struct DefaultIO : std::false_type {};

template <class T>
struct DefaultIO<T, std::void_t<decltype(std::cout << std::declval<T &>())>> : std::true_type {};

// Type trait for checking if a type is a tuple
template <class T, class = void>
struct IsTuple : std::false_type {};

template <class T>
struct IsTuple<T, std::void_t<typename std::tuple_size<T>::type>> : std::true_type {};

// Type trait for checking if a type is iterable (has begin())
template <class T, class = void>
struct Iterable : std::false_type {};

template <class T>
struct Iterable<T, std::void_t<decltype(std::begin(std::declval<T>()))>> : std::true_type {};

// Function to determine spacing based on type traits
template <class T>
constexpr char Space(const T &) {
    return (Iterable<T>::value || IsTuple<T>::value) ? ' ' : ' ';
    // Note: Original code always returns ' ', which may be a bug. Kept as is for fidelity.
}

// Writer struct for formatted output
template <auto &os>
struct Writer {
    template <class T>
    void Impl(T const &t) const {
        if constexpr (DefaultIO<T>::value) {
            os << t;
        } else if constexpr (Iterable<T>::value) {
            int i = 0;
            os << "[";
            for (auto &&x : t) {
                ((i++) ? (os << Space(x), Impl(x)) : Impl(x));
            }
            os << "]";
        } else if constexpr (IsTuple<T>::value) {
            std::apply([this](auto const &... args) {
                int i = 0;
                os << "{";
                (((i++) ? (os << ' ', Impl(args)) : Impl(args)), ...);
                os << "}";
            }, t);
        } else {
            static_assert(IsTuple<T>::value, "No matching type for print");
        }
    }

    template <class F, class... Ts>
    auto &operator()(F const &f, Ts const &... ts) const {
        Impl(f);
        ((os << ' ', Impl(ts)), ...);
        os << '\n';
        return *this;
    }
};

// Debug function to replace deb macro
template <typename... Args>
void debug(const char* file, int line, const char* names, Args&&... args) {
    std::cerr << RED << "File " << file << ", Line " << line << RESET << "\n";
    std::string names_str(names);
    std::replace(names_str.begin(), names_str.end(), ',', ' ');
    std::stringstream ss(names_str);
    auto print = [&ss](auto&& arg) {
        std::string name;
        ss >> name;
        std::cerr << name << " = ";
        packdb::Writer<std::cerr>{}(arg);
    };
    (print(std::forward<Args>(args)), ...);
}

// Assertion function to replace ASSERT macro
inline void assert_func(bool condition, const char* expr) {
    if (!condition) {
        throw std::runtime_error(expr);
    }
}

}

#define SFINAE(x, ...)             \
	template <class, class = void> \
	struct x : std::false_type {}; \
	template <class T>             \
	struct x<T, std::void_t<__VA_ARGS__>> : std::true_type {}

SFINAE(DefaultIO, decltype(std::cout << std::declval<T &>()));
SFINAE(IsTuple, typename std::tuple_size<T>::type);
SFINAE(Iterable, decltype(begin(std::declval<T>())));

template <class T>
constexpr char Space(const T &) {
	return (Iterable<T>::value or IsTuple<T>::value) ? ' ' : ' ';
}

template <auto &os>
struct Writer {
	template <class T>
	void Impl(T const &t) const {
		if constexpr (DefaultIO<T>::value) os << t;
		else if constexpr (Iterable<T>::value) {
			int i = 0;
			os << "[";
			for (auto &&x : t) ((i++) ? (os << Space(x), Impl(x)) : Impl(x));
			os << "]";
		} else if constexpr (IsTuple<T>::value)
			std::apply([this](auto const &... args) {
				int i = 0;
				os << "{";
				(((i++) ? (os << ' ', Impl(args)) : Impl(args)), ...);
				os << "}";
			}, t);
		else static_assert(IsTuple<T>::value, "No matching type for print");
	}
	template <class F, class... Ts>
	auto &operator()(F const &f, Ts const &... ts) const {
		return Impl(f), ((os << ' ', Impl(ts)), ...), os <<'\n', *this;
	}
};

#define deb(...) packdb::debug(__FILE__, __LINE__, #__VA_ARGS__, __VA_ARGS__)
#define ASSERT(expr) packdb::assert_func(expr, #expr)

#endif