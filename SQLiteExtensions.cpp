/* Add your header comment here */
#include <sqlite3ext.h> /* Do not use <sqlite3.h>! */
SQLITE_EXTENSION_INIT1
#include <cstdint>
#include <any>
#include <cstddef>
#include <vector>
#include <functional>
#include <exception>
#include <cmath>
#include <tuple>
#include <string>
#include <sstream>
#include <cinttypes>

//#define S1(x) #x
//#define S2(x) S1(x)
#define TO_STR(WHAT) [&]() -> std::string { std::stringstream sstream; sstream << WHAT; return sstream.str(); }()
#define LOCATION_STR TO_STR(__FILE__ << ":" << __LINE__ << ":" << __func__)
#define TYPE_STR(CODE) TO_STR("\n" << typeid(CODE).name() << "\n")
#define THROW_NESTED(CODE) try{ CODE } catch (...) { std::throw_with_nested(std::invalid_argument(LOCATION_STR + "\n" + #CODE + "\n")); }
#define THROW_NESTED_COMMENT(COMMENT, CODE) try{ CODE } catch (...) { std::throw_with_nested(std::invalid_argument(LOCATION_STR + COMMENT + "\n" + #CODE + "\n")); }
// prints the explanatory string of an exception. If the exception is nested,
// recurses to print the explanatory of the exception it holds
std::string exception_what(const std::exception& e, int level = 0)
{
	std::stringstream sstream;
	sstream << "\n[" << level << " " << e.what() << "]";
	try {
		std::rethrow_if_nested(e);
	}
	catch (const std::exception& e) {
		sstream << exception_what(e, level + 1);
	}
	catch (...) {}
	return sstream.str();
}

namespace sqlitewrap {
	using Arg = std::any;// <std::tuple<const void*, int>, double, int64_t, std::string, std::nullptr_t>;
	using Args = std::vector<Arg>;

	//template<class Result> using RowFunctionPtr = std::function<Result(sqlitewrap::Args&)>*;
	//template<class POD> using AggregateRowFunctionPtr = std::function<void(POD*, sqlitewrap::Args&)>*;
	namespace helper {
#pragma region https://functionalcpp.wordpress.com/2013/08/05/function-traits/
		template<class F>
		struct function_traits;

		// function pointer
		template<class R, class... Args>
		struct function_traits<R(*)(Args...)> : public function_traits<R(Args...)>
		{};

		template<class R, class... Args>
		struct function_traits<R(Args...)>
		{
			using return_type = R;

			static constexpr std::size_t arity = sizeof...(Args);

			template <std::size_t N>
			struct argument
			{
				static_assert(N < arity, "error: invalid parameter index.");
				using type = typename std::tuple_element<N, std::tuple<Args...>>::type;
			};
		};

		// member function pointer
		template<class C, class R, class... Args>
		struct function_traits<R(C::*)(Args...)> : public function_traits<R(C&, Args...)>
		{};

		// const member function pointer
		template<class C, class R, class... Args>
		struct function_traits<R(C::*)(Args...) const> : public function_traits<R(C&, Args...)>
		{};

		// member object pointer
		template<class C, class R>
		struct function_traits<R(C::*)> : public function_traits<R(C&)>
		{};

		// functor
		template<class F>
		struct function_traits
		{
		private:
			using call_type = function_traits<decltype(&F::operator())>; //F::type::operator() compiler error
		public:
			using return_type = typename call_type::return_type;

			static constexpr std::size_t arity = call_type::arity - 1;

			template <std::size_t N>
			struct argument
			{
				static_assert(N < arity, "error: invalid parameter index.");
				using type = typename call_type::template argument<N + 1>::type;
			};
		};

		template<class F>
		struct function_traits<F&> : public function_traits<F>
		{};

		template<class F>
		struct function_traits<F&&> : public function_traits<F>
		{};
#pragma endregion
		template <std::size_t... Indices> struct indices { using next = indices<Indices..., sizeof...(Indices)>; };

		template <std::size_t N> struct build_indices { using type = typename build_indices<N - 1>::type::next; };
		template <> struct build_indices<0> { using type = indices<>; };

		template <std::size_t N> using build_indices_t = typename build_indices<N>::type;

		template <class... Args>
		struct type_list
		{
			template <std::size_t N>
			using type = typename std::tuple_element<N, std::tuple<Args...>>::type;
		};

		using amiguous_types = type_list<sqlite_int64, double, int, long double>;

		template<class FuncType, class ArgsType, size_t... I>
		typename function_traits<FuncType>::return_type call(FuncType& f, ArgsType& args, indices<I...>) {
			THROW_NESTED(
				try { return f(std::any_cast<typename function_traits<decltype(f)>::argument<I>::type>(args[I])...); }
			catch (const std::bad_any_cast& e) {}
			try { return f(std::any_cast<amiguous_types::type<0>>(args[I])...); }
			catch (const std::bad_any_cast& e) {}
			try { return f(std::any_cast<amiguous_types::type<1>>(args[I])...); }
			catch (const std::bad_any_cast& e) {}
			throw std::invalid_argument("Did not manage to convert the type.");
			)
		}
		template<class FuncType, class ArgsType>
		typename function_traits<typename FuncType>::return_type call(FuncType& f, ArgsType& args) {
			return call(f, args, build_indices_t<function_traits<decltype(f)>::arity>{});
		}

	};
	template<class FuncType>
	struct caller
	{
		FuncType* m_f;
		caller(FuncType& f)
			:m_f(&f)
		{
		}
		auto operator()(sqlitewrap::Args& args)
		{
			THROW_NESTED(
				return helper::call(*m_f, args);
			)
		}
	};
	sqlitewrap::Args GetSQLArguments(int argc, sqlite3_value ** argv) {
		sqlitewrap::Args args;
		for (int argNo = 0; argNo != argc; argNo++)
		{
			switch (sqlite3_value_type(argv[argNo]))
			{
			case SQLITE_INTEGER:
			{
				THROW_NESTED(
					args.emplace(std::end(args), sqlite3_value_int64(argv[argNo]));
				)
					break;
			}
			case SQLITE_FLOAT:
			{
				THROW_NESTED(
					args.emplace(std::end(args), sqlite3_value_double(argv[argNo]));
				)
					break;
			}
			case SQLITE_BLOB:
			{
				THROW_NESTED(
					auto volatile blobSize = sqlite3_value_bytes(argv[argNo]);
				auto volatile blobPtr = sqlite3_value_blob(argv[argNo]);
				args.emplace(std::end(args), std::make_tuple(blobPtr, blobSize));
				)
					break;
			}
			case SQLITE_NULL:
			{
				THROW_NESTED(
					args.emplace(std::end(args), nullptr);
				)
					break;
			}
			case SQLITE3_TEXT:
			{
				THROW_NESTED(
					auto volatile textSize = sqlite3_value_bytes(argv[argNo]);
				//http://sqlite.1065341.n5.nabble.com/Types-for-strings-non-expert-question-td51078.html "bad design on my part" (D. Richard Hipp )
				auto volatile textPtr = reinterpret_cast<const char*>(sqlite3_value_text(argv[argNo]));
				args.emplace(std::end(args), std::string(textPtr, textSize));
				)
					break;
			}
			default:
				throw std::invalid_argument("SQLiteExtensions: Unknown SQL data type.");
				break;
			}
		}
		return args;
	}

	static void Result(sqlite3_context* context, nullptr_t value)
	{
		sqlite3_result_null(context);
	}

	static void Result(sqlite3_context* context, double value)
	{
		sqlite3_result_double(context, value);
	}

	static void Result(sqlite3_context* context, long double value)
	{
		sqlite3_result_blob(context, &value, sizeof(value), SQLITE_TRANSIENT);
	}

	static void Result(sqlite3_context* context, int64_t value)
	{
		sqlite3_result_int64(context, value);
	}

	static void Result(sqlite3_context* context, int value)
	{
		sqlite3_result_int(context, value);
	}

	static void Result(sqlite3_context* context, bool value)
	{
		sqlite3_result_blob(context, &value, sizeof(value), SQLITE_TRANSIENT);
	}

	static void Result(sqlite3_context* context, std::ldiv_t value)
	{
		sqlite3_result_blob(context, &value, sizeof(value), SQLITE_TRANSIENT);
	}

	static void Result(sqlite3_context* context, std::lldiv_t value)
	{
		sqlite3_result_blob(context, &value, sizeof(value), SQLITE_TRANSIENT);
	}

	static void Result(sqlite3_context* context, std::imaxdiv_t value)
	{
		sqlite3_result_blob(context, &value, sizeof(value), SQLITE_TRANSIENT);
	}

	static void Result(sqlite3_context* context, std::string value)
	{
		sqlite3_result_text(context, value.c_str(), value.size(), SQLITE_TRANSIENT);
	}

	static void Result(sqlite3_context* context, std::tuple<const void*, int> value)
	{
		sqlite3_result_blob(context, std::get<0>(value), std::get<1>(value), SQLITE_TRANSIENT);
	}

	template<class POD>
	static POD* GetPOD(sqlite3_context*)
	{
		return reinterpret_cast<POD*>(sqlite3_aggregate_context(context, sizeof(decltype(POD))));
	}
	template<class POD>
	static POD* GetPODFinal(sqlite3_context*)
	{
		return reinterpret_cast<POD*>(sqlite3_aggregate_context(context, 0));
	}

	template<class Fun, class Args>
	static void AggregateRowFunctionWrapper(sqlite3_context* context, int argc, sqlite3_value **argv, Fun& F)
	{
		F(sqlitewrap::GetPOD<Args>(context), sqlitewrap::GetSQLArguments(argc, argv));
	}

	template<class Fun>
	static void AggregateResultWrapper(sqlite3_context* context, Fun& F)
	{
		try {
			sqlitewrap::Result(context, F(GetPODFinal<Args>(context)));
		}
		catch (const std::exception& e) {
			sqlite3_result_error(context, (LOCATION_STR + " " + exception_what(e) + " " + typeid(F).name()).c_str(), -1);
		}
	}

	template<class Fun>
	static void RowFunctionWrapper(sqlite3_context* context, int argc, sqlite3_value **argv, Fun& F)
	{
		try {
			sqlitewrap::Result(context, F(sqlitewrap::GetSQLArguments(argc, argv)));
		}
		catch (const std::exception& e)
		{
			sqlite3_result_error(context, (LOCATION_STR + " " + exception_what(e) + " " + typeid(F).name()).c_str(), -1);
		}
	}


}

#define GENERATE_CALLER(FNAME,NAME)\
static void call_##FNAME(sqlite3_context* context, int argc, sqlite3_value **argv)\
{\
	sqlitewrap::RowFunctionWrapper(context, argc, argv, sqlitewrap::caller<decltype(##NAME)>(##NAME));\
}\

#define GENERATE_CALLER_EXPLICIT(TYPE,FNAME,NAME)\
static void call_##FNAME(sqlite3_context* context, int argc, sqlite3_value **argv)\
{\
	sqlitewrap::RowFunctionWrapper(context, argc, argv, sqlitewrap::caller<##TYPE>(##NAME));\
}\

#pragma region GENERATE_CALLER
GENERATE_CALLER_EXPLICIT(double(double), acos, std::acos)
GENERATE_CALLER_EXPLICIT(double(double), acosh, std::acosh)
GENERATE_CALLER_EXPLICIT(double(double), asin, std::asin)
GENERATE_CALLER_EXPLICIT(double(double), asinh, std::asinh)
GENERATE_CALLER_EXPLICIT(double(double), atan, std::atan)
GENERATE_CALLER_EXPLICIT(double(double, double), atan2, std::atan2)
GENERATE_CALLER_EXPLICIT(double(double), atanh, std::atanh)
GENERATE_CALLER_EXPLICIT(double(double), cbrt, std::cbrt)
GENERATE_CALLER_EXPLICIT(double(double), ceil, std::ceil)
GENERATE_CALLER_EXPLICIT(double(double, double), copysign, std::copysign)
GENERATE_CALLER_EXPLICIT(double(double), cos, std::cos)
GENERATE_CALLER_EXPLICIT(double(double), cosh, std::cosh)
GENERATE_CALLER_EXPLICIT(double(double), erf, std::erf)
GENERATE_CALLER_EXPLICIT(double(double), erfc, std::erfc)
GENERATE_CALLER_EXPLICIT(double(double), exp, std::exp)
GENERATE_CALLER_EXPLICIT(double(double), exp2, std::exp2)
GENERATE_CALLER_EXPLICIT(double(double), expm1, std::expm1)
GENERATE_CALLER_EXPLICIT(double(double), fabs, std::fabs)
GENERATE_CALLER_EXPLICIT(double(double, double), fdim, std::fdim)
GENERATE_CALLER_EXPLICIT(double(double), floor, std::floor)
GENERATE_CALLER_EXPLICIT(double(double, double, double), fma, std::fma)
GENERATE_CALLER_EXPLICIT(double(double, double), fmax, std::fmax)
GENERATE_CALLER_EXPLICIT(double(double, double), fmin, std::fmin)
GENERATE_CALLER_EXPLICIT(double(double, double), fmod, std::fmod)
//GENERATE_CALLER_EXPLICIT(int(double), fpclassify, std::fpclassify)
//GENERATE_CALLER_EXPLICIT(double(double, double), frexp, std::frexp)
GENERATE_CALLER_EXPLICIT(double(double, double), hypot, std::hypot)
GENERATE_CALLER_EXPLICIT(int(double), ilogb, std::ilogb)
GENERATE_CALLER(imaxabs, std::imaxabs)
GENERATE_CALLER(imaxdiv, std::imaxdiv)
GENERATE_CALLER_EXPLICIT(bool(double), isfinite, std::isfinite)
GENERATE_CALLER_EXPLICIT(bool(double, double), isgreater, std::isgreater)
GENERATE_CALLER_EXPLICIT(bool(double, double), isgreaterequal, std::isgreaterequal)
GENERATE_CALLER_EXPLICIT(bool(double), isinf, std::isinf)
GENERATE_CALLER_EXPLICIT(bool(double, double), isless, std::isless)
GENERATE_CALLER_EXPLICIT(bool(double, double), islessequal, std::islessequal)
GENERATE_CALLER_EXPLICIT(bool(double, double), islessgreater, std::islessgreater)
GENERATE_CALLER_EXPLICIT(bool(double), isnan, std::isnan)
GENERATE_CALLER_EXPLICIT(bool(double), isnormal, std::isnormal)
GENERATE_CALLER_EXPLICIT(bool(double, double), isunordered, std::isunordered)
GENERATE_CALLER(labs, std::labs)
GENERATE_CALLER_EXPLICIT(double(double, int), ldexp, std::ldexp)
GENERATE_CALLER(ldiv, std::ldiv)
GENERATE_CALLER_EXPLICIT(double(double), lgamma, std::lgamma)
GENERATE_CALLER(llabs, std::llabs)
GENERATE_CALLER(lldiv, std::lldiv)
GENERATE_CALLER_EXPLICIT(long long(double), llrint, std::llrint)
GENERATE_CALLER_EXPLICIT(long long(double), llround, std::llround)
GENERATE_CALLER_EXPLICIT(double(double), log, std::log)
GENERATE_CALLER_EXPLICIT(double(double), log10, std::log10)
GENERATE_CALLER_EXPLICIT(double(double), log1p, std::log1p)
GENERATE_CALLER_EXPLICIT(double(double), log2, std::log2)
GENERATE_CALLER_EXPLICIT(double(double), logb, std::logb)
GENERATE_CALLER_EXPLICIT(long(double), lrint, std::lrint)
GENERATE_CALLER_EXPLICIT(long(double), lround, std::lround)
//GENERATE_CALLER_EXPLICIT(double(double, double), modf, std::modf)
GENERATE_CALLER_EXPLICIT(double(double), nearbyint, std::nearbyint)
GENERATE_CALLER_EXPLICIT(double(double, double), nextafter, std::nextafter)
//GENERATE_CALLER_EXPLICIT(long double(long double, long double), nexttoward, std::nexttoward)
GENERATE_CALLER_EXPLICIT(double(double, double), pow, std::pow)
GENERATE_CALLER_EXPLICIT(double(double, double), remainder, std::remainder)
//GENERATE_CALLER_EXPLICIT(double(double, double), remquo, std::remquo)
GENERATE_CALLER_EXPLICIT(double(double), rint, std::rint)
GENERATE_CALLER_EXPLICIT(double(double), round, std::round)
//GENERATE_CALLER_EXPLICIT(long double(long double, long), scalbln, std::scalbln)
//GENERATE_CALLER_EXPLICIT(double(double, long), scalbn, std::scalbn)
//GENERATE_CALLER_EXPLICIT(bool(double), signbit, std::signbit)
GENERATE_CALLER_EXPLICIT(double(double), sin, std::sin)
GENERATE_CALLER_EXPLICIT(double(double), sinh, std::sinh)
GENERATE_CALLER_EXPLICIT(double(double), sqrt, std::sqrt)
GENERATE_CALLER_EXPLICIT(double(double), tan, std::tan)
GENERATE_CALLER_EXPLICIT(double(double), tanh, std::tanh)
GENERATE_CALLER_EXPLICIT(double(double), tgamma, std::tgamma)
GENERATE_CALLER_EXPLICIT(double(double), trunc, std::trunc)
#pragma endregion
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
/* TODO: Change the entry point name so that "extension" is replaced by
** text derived from the shared library filename as follows:  Copy every
** ASCII alphabetic character from the filename after the last "/" through
** the next following ".", converting each character to lowercase, and
** discarding the first three characters if they are "lib".
*/
int sqlite3_sqliteextensions_init(
	sqlite3 *db,
	char **pzErrMsg,
	const sqlite3_api_routines *pApi
)
{
	int rc = SQLITE_OK;
	SQLITE_EXTENSION_INIT2(pApi);
	try {
		/* Insert here calls to
		**     sqlite3_create_function_v2(),
		**     sqlite3_create_collation_v2(),
		**     sqlite3_create_module_v2(), and/or
		**     sqlite3_vfs_register()
		** to register the new features that your extension adds.
		*/
#pragma region create_function
		sqlite3_create_function_v2(db, "acos", 1, SQLITE_UTF8, nullptr, &call_acos, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "acosh", 1, SQLITE_UTF8, nullptr, &call_acosh, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "asin", 1, SQLITE_UTF8, nullptr, &call_asin, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "asinh", 1, SQLITE_UTF8, nullptr, &call_asinh, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "atan", 1, SQLITE_UTF8, nullptr, &call_atan, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "atan2", 2, SQLITE_UTF8, nullptr, &call_atan2, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "atanh", 1, SQLITE_UTF8, nullptr, &call_atanh, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "cbrt", 1, SQLITE_UTF8, nullptr, &call_cbrt, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "ceil", 1, SQLITE_UTF8, nullptr, &call_ceil, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "copysign", 2, SQLITE_UTF8, nullptr, &call_copysign, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "cos", 1, SQLITE_UTF8, nullptr, &call_cos, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "cosh", 1, SQLITE_UTF8, nullptr, &call_cosh, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "erf", 1, SQLITE_UTF8, nullptr, &call_erf, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "erfc", 1, SQLITE_UTF8, nullptr, &call_erfc, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "exp", 1, SQLITE_UTF8, nullptr, &call_exp, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "exp2", 1, SQLITE_UTF8, nullptr, &call_exp2, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "expm1", 1, SQLITE_UTF8, nullptr, &call_expm1, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "fabs", 1, SQLITE_UTF8, nullptr, &call_fabs, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "fdim", 2, SQLITE_UTF8, nullptr, &call_fdim, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "floor", 1, SQLITE_UTF8, nullptr, &call_floor, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "fma", 3, SQLITE_UTF8, nullptr, &call_fma, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "fmax", 2, SQLITE_UTF8, nullptr, &call_fmax, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "fmin", 2, SQLITE_UTF8, nullptr, &call_fmin, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "fmod", 2, SQLITE_UTF8, nullptr, &call_fmod, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "hypot", 2, SQLITE_UTF8, nullptr, &call_hypot, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "ilogb", 1, SQLITE_UTF8, nullptr, &call_ilogb, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "imaxabs", 1, SQLITE_UTF8, nullptr, &call_imaxabs, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "imaxdiv", 1, SQLITE_UTF8, nullptr, &call_imaxdiv, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "isfinite", 1, SQLITE_UTF8, nullptr, &call_isfinite, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "isgreater", 2, SQLITE_UTF8, nullptr, &call_isgreater, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "isgreaterequal", 2, SQLITE_UTF8, nullptr, &call_isgreaterequal, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "isinf", 1, SQLITE_UTF8, nullptr, &call_isinf, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "isless", 2, SQLITE_UTF8, nullptr, &call_isless, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "islessequal", 2, SQLITE_UTF8, nullptr, &call_islessequal, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "islessgreater", 2, SQLITE_UTF8, nullptr, &call_islessgreater, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "isnan", 1, SQLITE_UTF8, nullptr, &call_isnan, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "isnormal", 1, SQLITE_UTF8, nullptr, &call_isnormal, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "isunordered", 2, SQLITE_UTF8, nullptr, &call_isunordered, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "labs", 1, SQLITE_UTF8, nullptr, &call_labs, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "ldexp", 2, SQLITE_UTF8, nullptr, &call_ldexp, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "ldiv", 1, SQLITE_UTF8, nullptr, &call_ldiv, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "lgamma", 1, SQLITE_UTF8, nullptr, &call_lgamma, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "llabs", 1, SQLITE_UTF8, nullptr, &call_llabs, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "lldiv", 1, SQLITE_UTF8, nullptr, &call_lldiv, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "llrint", 1, SQLITE_UTF8, nullptr, &call_llrint, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "llround", 1, SQLITE_UTF8, nullptr, &call_llround, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "log", 1, SQLITE_UTF8, nullptr, &call_log, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "log10", 1, SQLITE_UTF8, nullptr, &call_log10, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "log1p", 1, SQLITE_UTF8, nullptr, &call_log1p, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "log2", 1, SQLITE_UTF8, nullptr, &call_log2, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "logb", 1, SQLITE_UTF8, nullptr, &call_logb, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "lrint", 1, SQLITE_UTF8, nullptr, &call_lrint, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "lround", 1, SQLITE_UTF8, nullptr, &call_lround, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "nearbyint", 1, SQLITE_UTF8, nullptr, &call_nearbyint, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "nextafter", 2, SQLITE_UTF8, nullptr, &call_nextafter, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "pow", 2, SQLITE_UTF8, nullptr, &call_pow, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "remainder", 2, SQLITE_UTF8, nullptr, &call_remainder, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "rint", 1, SQLITE_UTF8, nullptr, &call_rint, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "round", 1, SQLITE_UTF8, nullptr, &call_round, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "sin", 1, SQLITE_UTF8, nullptr, &call_sin, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "sinh", 1, SQLITE_UTF8, nullptr, &call_sinh, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "sqrt", 1, SQLITE_UTF8, nullptr, &call_sqrt, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "tan", 1, SQLITE_UTF8, nullptr, &call_tan, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "tanh", 1, SQLITE_UTF8, nullptr, &call_tanh, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "tgamma", 1, SQLITE_UTF8, nullptr, &call_tgamma, nullptr, nullptr, nullptr);
		sqlite3_create_function_v2(db, "trunc", 1, SQLITE_UTF8, nullptr, &call_trunc, nullptr, nullptr, nullptr);

#pragma endregion
	}
	catch (const std::exception& e)
	{
		rc = SQLITE_ERROR;
	}
	return rc;
}
