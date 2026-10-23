#pragma once
#include <string>
#include <type_traits>
#include <utility>

//std::string format("format $1-5", args[0-4])
//
//Formats the fromat string into a std::string output
//Arguments are placed into the string anywhere $1 through $5 are found
//
//For example:
//format("test $1", 2.5f) returns "test 2.50000"
//
//Float, Double, unsigned, int, c string, and std::string arguments are supported
//
//There are no formatting options

struct FormatArg {
	enum ArgType {
		Arg_float,
		Arg_double,
		Arg_unsigned,
		Arg_int,
		Arg_cstr,
		Arg_string
	};

	unsigned print(char* buffer, unsigned space);
	
	FormatArg();
	FormatArg(float F);
	FormatArg(double D);
	FormatArg(unsigned U);
	FormatArg(int I);
	FormatArg(const char* C);
	FormatArg(const std::string& S);

	unsigned type;
	union {
		float f;
		double d;
		unsigned u;
		int i;
		const char* c;
		const std::string* s;
	};
};

void format(std::string& arg, const char* fmt, unsigned argn, FormatArg* argv);
std::string format(const char* format, FormatArg a1);
std::string format(const char* format, FormatArg a1, FormatArg a2);
std::string format(const char* format, FormatArg a1, FormatArg a2, FormatArg a3);
std::string format(const char* format, FormatArg a1, FormatArg a2, FormatArg a3, FormatArg a4);
std::string format(const char* format, FormatArg a1, FormatArg a2, FormatArg a3, FormatArg a4, FormatArg a5);

//An exact match for any convertible arguments, so that argument dependent lookup
//on std::string arguments does not make calls ambiguous with C++20's std::format
template<class... Args>
	requires (sizeof...(Args) >= 1 && sizeof...(Args) <= 5 && (std::is_constructible_v<FormatArg, Args&&> && ...))
inline std::string format(const char* fmt, Args&&... args) {
	return ::format(fmt, FormatArg(std::forward<Args>(args))...);
}
