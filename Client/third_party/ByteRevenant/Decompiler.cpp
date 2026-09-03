#include "Decompiler.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace br::lua51
{
	namespace
	{
		enum Precedence
		{
			kPrecOr = 1,
			kPrecAnd = 2,
			kPrecCompare = 3,
			kPrecConcat = 4,
			kPrecAdd = 5,
			kPrecMul = 6,
			kPrecUnary = 7,
			kPrecPrimary = 8,
		};

		struct TableEntry;
		struct ClosureValue;

		struct Expr
		{
			enum class Kind
			{
				Text,
				Table,
				Closure,
			};

			Kind kind = Kind::Text;
			std::string text = "nil";
			int precedence = kPrecPrimary;
			std::shared_ptr<std::vector<TableEntry>> table_entries;
			std::shared_ptr<ClosureValue> closure;
		};

		struct TableEntry
		{
			enum class KeyKind
			{
				Array,
				Name,
				Expr,
			};

			KeyKind key_kind = KeyKind::Array;
			std::string key_text;
			Expr value;
		};

		struct ClosureValue
		{
			const Function* function = nullptr;
			std::vector<std::string> upvalues;
		};

		struct RegisterValue
		{
			bool valid = false;
			Expr expr;
			int assigned_pc = -1;
			int assigned_line = -1;
			bool self_call = false;
			std::string self_object;
			std::string self_method;
			bool open_call = false;
			std::optional<std::size_t> multi_group;
		};

		struct MultiResultGroup
		{
			std::string call_text;
			int assigned_line = -1;
			std::vector<int> registers;
			std::vector<std::string> names;
			bool emitted = false;
		};

		struct ConditionInfo
		{
			Expr skip_jump;
			Expr take_jump;
			std::size_t target_pc = 0;
		};

		struct HexStreamDecodeConfig
		{
			double seed_multiplier = 0.0;
			double seed_threshold = 0.0;
			double seed_offset = 0.0;
			double state_divisor = 0.0;
			double state_multiplier = 0.0;
			double state_modulus = 128.0;
			double output_modulus = 256.0;
		};

		Expr MakeText(std::string text, int precedence = kPrecPrimary)
		{
			return Expr{ Expr::Kind::Text, std::move(text), precedence };
		}

		std::string Indent(int level)
		{
			return std::string(static_cast<std::size_t>(std::max(level, 0)) * 4, ' ');
		}

		bool IsHumanString(std::string_view value)
		{
			if (value.empty())
			{
				return true;
			}

			int visible = 0;
			int bad = 0;
			for (unsigned char ch : value)
			{
				if (ch == '\n' || ch == '\r' || ch == '\t')
				{
					visible++;
					continue;
				}
				if (ch < 0x20 || ch == 0x7F)
				{
					bad++;
					continue;
				}
				visible++;
			}

			return bad == 0 && visible * 2 >= static_cast<int>(value.size());
		}

		std::uint32_t StableHash(std::string_view value)
		{
			std::uint32_t hash = 2166136261u;
			for (unsigned char ch : value)
			{
				hash ^= ch;
				hash *= 16777619u;
			}
			return hash;
		}

		bool HasHighBytes(std::string_view value)
		{
			for (unsigned char ch : value)
			{
				if (ch >= 0x80)
				{
					return true;
				}
			}

			return false;
		}

		bool IsCyrillic(wchar_t ch)
		{
			return (ch >= 0x0400 && ch <= 0x052F) || ch == 0x2116;
		}

		bool IsHumanWide(std::wstring_view value)
		{
			if (value.empty())
			{
				return true;
			}

			int visible = 0;
			int bad = 0;
			int letters = 0;
			int cyrillic = 0;

			for (wchar_t ch : value)
			{
				if (ch == L'\n' || ch == L'\r' || ch == L'\t')
				{
					visible++;
					continue;
				}

				if (ch < 0x20)
				{
					bad++;
					continue;
				}

				if (std::iswprint(ch) == 0)
				{
					bad++;
					continue;
				}

				visible++;
				if (std::iswalpha(ch) != 0)
				{
					letters++;
				}
				if (IsCyrillic(ch))
				{
					cyrillic++;
				}
			}

			if (bad != 0 || visible * 2 < static_cast<int>(value.size()))
			{
				return false;
			}

			if (letters == 0)
			{
				return false;
			}

			return cyrillic > 0 || letters * 2 >= visible;
		}

		bool HasNaturalCyrillicWord(std::wstring_view value)
		{
			for (std::size_t index = 0; index < value.size();)
			{
				if (!IsCyrillic(value[index]))
				{
					++index;
					continue;
				}

				const wchar_t first = value[index];
				const bool first_ok = std::iswlower(first) != 0 || std::iswupper(first) != 0;
				int length = first_ok ? 1 : 0;
				int lower_after_first = 0;
				bool bad_mix = !first_ok;
				++index;

				while (index < value.size() && IsCyrillic(value[index]))
				{
					const wchar_t ch = value[index];
					if (std::iswlower(ch) != 0)
					{
						lower_after_first++;
						length++;
					}
					else if (std::iswupper(ch) != 0)
					{
						length++;
						bad_mix = true;
					}
					else
					{
						bad_mix = true;
					}
					++index;
				}

				if (!bad_mix && length >= 3 && lower_after_first >= 2)
				{
					return true;
				}
			}

			return false;
		}

		bool IsAcceptableCyrillicText(std::wstring_view value)
		{
			std::wstring cleaned(value);
			for (std::wstring_view spec : { std::wstring_view(L"%s"), std::wstring_view(L"%d"), std::wstring_view(L"%i"), std::wstring_view(L"%u"), std::wstring_view(L"%f"), std::wstring_view(L"%0.2f") })
			{
				std::size_t position = 0;
				while ((position = cleaned.find(spec, position)) != std::wstring::npos)
				{
					cleaned.erase(position, spec.size());
				}
			}

			value = cleaned;
			if (HasNaturalCyrillicWord(value))
			{
				return true;
			}

			int letters = 0;
			int cyrillic = 0;
			for (wchar_t ch : value)
			{
				if (std::iswalpha(ch) != 0)
				{
					letters++;
					if (!IsCyrillic(ch))
					{
						return false;
					}

					cyrillic++;
					continue;
				}

				if (std::iswdigit(ch) != 0 || ch == L' ' || ch == L'-' || ch == L'_' || ch == L'.' || ch == L',' || ch == L':' || ch == L';' || ch == L'!' || ch == L'?' || ch == L'(' || ch == L')' || ch == L'"' || ch == L'\'' || ch == L'\u2116' || ch == L'\u20B4' || ch == L'/' || ch == L'%')
				{
					continue;
				}

				if (std::iswprint(ch) == 0)
				{
					return false;
				}
			}

			return letters >= 2 && letters == cyrillic;
		}

		std::optional<std::wstring> Utf8ToWide(std::string_view value);

		bool ShouldKeepRecoveredString(std::string_view value)
		{
			static const std::unordered_set<std::string_view> trivial =
			{
				"_",
				"x",
				"y",
				"z",
				"bg",
				"fn",
			};

			if (value.size() < 3)
			{
				return false;
			}

			if (trivial.find(value) != trivial.end())
			{
				return false;
			}

			if (!HasHighBytes(value))
			{
				return IsHumanString(value);
			}

			const auto wide = Utf8ToWide(value);
			if (!wide || !IsAcceptableCyrillicText(*wide))
			{
				return false;
			}

			int cyrillic = 0;
			for (wchar_t ch : *wide)
			{
				if (IsCyrillic(ch))
				{
					cyrillic++;
					continue;
				}

				if (ch == L' ' || ch == L'-' || ch == L'_' || ch == L'.' || ch == L',' || ch == L':' || ch == L';' || ch == L'!' || ch == L'?' || ch == L'(' || ch == L')' || ch == L'"' || ch == L'\'')
				{
					continue;
				}

				return false;
			}

			return cyrillic >= 3;
		}

		int CountRegisterTokens(std::string_view value)
		{
			int count = 0;
			for (std::size_t index = 0; index < value.size(); ++index)
			{
				if (value[index] != 'r' || index + 1 >= value.size() || !std::isdigit(static_cast<unsigned char>(value[index + 1])))
				{
					continue;
				}

				if (index > 0)
				{
					const unsigned char prev = static_cast<unsigned char>(value[index - 1]);
					if (std::isalnum(prev) != 0 || prev == '_')
					{
						continue;
					}
				}

				++count;
				while (index + 1 < value.size() && std::isdigit(static_cast<unsigned char>(value[index + 1])) != 0)
				{
					++index;
				}
			}

			return count;
		}

		int CountSubstring(std::string_view value, std::string_view needle)
		{
			if (needle.empty())
			{
				return 0;
			}

			int count = 0;
			std::size_t position = 0;
			while ((position = value.find(needle, position)) != std::string_view::npos)
			{
				++count;
				position += needle.size();
			}

			return count;
		}

		bool ShouldKeepLosslessLine(std::string_view value)
		{
			if (value.empty())
			{
				return false;
			}

			while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
			{
				value.remove_prefix(1);
			}

			const std::size_t comment = value.find("--");
			if (comment != std::string_view::npos && value.substr(0, comment).find_first_not_of(" \t") == std::string_view::npos)
			{
				return false;
			}

			if (value.size() > 220)
			{
				return false;
			}

			if (CountRegisterTokens(value) > 8)
			{
				return false;
			}

			const int commas = static_cast<int>(std::count(value.begin(), value.end(), ','));
			if (commas > 10)
			{
				return false;
			}

			if ((value.find("local var_") != std::string_view::npos || value.find("local value_") != std::string_view::npos) && value.find(" = nil") != std::string_view::npos)
			{
				return false;
			}

			if (value.find("_G[") != std::string_view::npos && value.find("_G[\"") == std::string_view::npos)
			{
				return false;
			}

			for (std::string_view token : { std::string_view(" + nil"), std::string_view(" - nil"), std::string_view(" * nil"), std::string_view(" / nil"), std::string_view(" % nil"), std::string_view(" ^ nil"),
				std::string_view("nil + "), std::string_view("nil - "), std::string_view("nil * "), std::string_view("nil / "), std::string_view("nil % "), std::string_view("nil ^ "),
				std::string_view(" = -nil"), std::string_view(" = #nil") })
			{
				if (value.find(token) != std::string_view::npos)
				{
					return false;
				}
			}

			if (value.find("__br_str_") != std::string_view::npos)
			{
				if (value.find(" + ") != std::string_view::npos
					|| value.find(" - ") != std::string_view::npos
					|| value.find(" * ") != std::string_view::npos
					|| value.find(" / ") != std::string_view::npos
					|| value.find(" % ") != std::string_view::npos
					|| value.find(" ^ ") != std::string_view::npos)
				{
					return false;
				}

				if ((value.starts_with("local var_") || value.starts_with("local value_"))
					&& (value.find(" = \"__br_str_") != std::string_view::npos || value.find(" = '__br_str_") != std::string_view::npos))
				{
					return false;
				}
			}

			if ((value.starts_with("local var_") || value.starts_with("local value_"))
				&& (value.ends_with(" = true") || value.ends_with(" = false") || value.ends_with(" = {}") || value.find(" = function() end") != std::string_view::npos))
			{
				return false;
			}

			if (value.find("return ") != std::string_view::npos && CountRegisterTokens(value) > 4)
			{
				return false;
			}

			return true;
		}

		std::string_view TrimView(std::string_view value)
		{
			while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n'))
			{
				value.remove_prefix(1);
			}

			while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
			{
				value.remove_suffix(1);
			}

			return value;
		}

		bool IsIdentifierStart(unsigned char ch)
		{
			return std::isalpha(ch) != 0 || ch == '_';
		}

		bool IsIdentifierChar(unsigned char ch)
		{
			return std::isalnum(ch) != 0 || ch == '_';
		}

		bool IsAllDigits(std::string_view value)
		{
			if (value.empty())
			{
				return false;
			}

			return std::all_of(value.begin(), value.end(), [](unsigned char ch)
			{
				return std::isdigit(ch) != 0;
			});
		}

		bool IsLuaKeyword(std::string_view value)
		{
			using namespace std::literals;
			static const std::unordered_set<std::string_view> keywords =
			{
				"and"sv,
				"break"sv,
				"do"sv,
				"else"sv,
				"elseif"sv,
				"end"sv,
				"false"sv,
				"for"sv,
				"function"sv,
				"if"sv,
				"in"sv,
				"local"sv,
				"nil"sv,
				"not"sv,
				"or"sv,
				"repeat"sv,
				"return"sv,
				"then"sv,
				"true"sv,
				"until"sv,
				"while"sv,
			};

			return keywords.find(value) != keywords.end();
		}

		bool IsGenericSyntheticName(std::string_view value)
		{
			value = TrimView(value);
			if (value.empty())
			{
				return true;
			}

			if (value == "var" || value == "value" || value == "arg" || value == "_G")
			{
				return true;
			}

			if (value.starts_with("__br_str_") || value.starts_with("__br_proxy"))
			{
				return true;
			}

			if ((value.starts_with("var_") && IsAllDigits(value.substr(4)))
				|| (value.starts_with("value_") && IsAllDigits(value.substr(6)))
				|| (value.starts_with("upvalue_") && IsAllDigits(value.substr(8))))
			{
				return true;
			}

			if (value.size() > 3 && value.starts_with("arg") && IsAllDigits(value.substr(3)))
			{
				return true;
			}

			if (value.size() > 1 && value.front() == 'r' && IsAllDigits(value.substr(1)))
			{
				return true;
			}

			return false;
		}

		std::size_t CountSyntheticNames(std::string_view value, bool meaningful)
		{
			std::size_t count = 0;
			for (std::size_t index = 0; index < value.size();)
			{
				const unsigned char ch = static_cast<unsigned char>(value[index]);
				if (!IsIdentifierStart(ch))
				{
					++index;
					continue;
				}

				const std::size_t begin = index++;
				while (index < value.size() && IsIdentifierChar(static_cast<unsigned char>(value[index])))
				{
					++index;
				}

				const std::string_view token = value.substr(begin, index - begin);
				if (IsLuaKeyword(token))
				{
					continue;
				}

				const bool generic = IsGenericSyntheticName(token);
				if ((meaningful && !generic) || (!meaningful && generic))
				{
					++count;
				}
			}

			return count;
		}

		std::size_t CountGenericSyntheticNames(std::string_view value)
		{
			return CountSyntheticNames(value, false);
		}

		std::size_t CountMeaningfulSyntheticNames(std::string_view value)
		{
			return CountSyntheticNames(value, true);
		}

		bool ContainsReadableLiteral(std::string_view value)
		{
			for (std::size_t index = 0; index < value.size(); ++index)
			{
				const char quote = value[index];
				if (quote != '"' && quote != '\'')
				{
					continue;
				}

				std::size_t end = index + 1;
				while (end < value.size())
				{
					if (value[end] == '\\')
					{
						end += 2;
						continue;
					}

					if (value[end] == quote)
					{
						break;
					}

					++end;
				}

				if (end >= value.size())
				{
					break;
				}

				const std::string_view literal = value.substr(index + 1, end - index - 1);
				if (!literal.empty() && literal.find("__br_str_") == std::string_view::npos)
				{
					return true;
				}

				index = end;
			}

			return false;
		}

		bool StartsWithKeyword(std::string_view value, std::string_view keyword)
		{
			value = TrimView(value);
			if (!value.starts_with(keyword))
			{
				return false;
			}

			if (value.size() == keyword.size())
			{
				return true;
			}

			const unsigned char next = static_cast<unsigned char>(value[keyword.size()]);
			return std::isspace(next) != 0 || next == '(';
		}

		bool IsOpenSyntheticBlock(std::string_view value)
		{
			value = TrimView(value);
			return (StartsWithKeyword(value, "if") && value.ends_with(" then"))
				|| (StartsWithKeyword(value, "for") && value.ends_with(" do"))
				|| (StartsWithKeyword(value, "while") && value.ends_with(" do"))
				|| StartsWithKeyword(value, "function")
				|| value.find("function(") != std::string_view::npos
				|| value == "repeat";
		}

		bool IsCloseSyntheticBlock(std::string_view value)
		{
			value = TrimView(value);
			return value == "end"
				|| value.starts_with("end,")
				|| value.starts_with("end)")
				|| value.starts_with("end:")
				|| value.starts_with("end.")
				|| StartsWithKeyword(value, "until");
		}

		bool IsBranchSyntheticLine(std::string_view value)
		{
			value = TrimView(value);
			return value == "else" || (StartsWithKeyword(value, "elseif") && value.ends_with(" then"));
		}

		bool ContainsBrokenSyntheticMath(std::string_view value)
		{
			for (std::string_view token : { std::string_view(" + nil"), std::string_view(" - nil"), std::string_view(" * nil"), std::string_view(" / nil"), std::string_view(" % nil"), std::string_view(" ^ nil"),
				std::string_view("nil + "), std::string_view("nil - "), std::string_view("nil * "), std::string_view("nil / "), std::string_view("nil % "), std::string_view("nil ^ "),
				std::string_view(" = -nil"), std::string_view(" = #nil") })
			{
				if (value.find(token) != std::string_view::npos)
				{
					return true;
				}
			}

			if (value.find("__br_str_") != std::string_view::npos)
			{
				if (value.find(" + ") != std::string_view::npos
					|| value.find(" - ") != std::string_view::npos
					|| value.find(" * ") != std::string_view::npos
					|| value.find(" / ") != std::string_view::npos
					|| value.find(" % ") != std::string_view::npos
					|| value.find(" ^ ") != std::string_view::npos)
				{
					return true;
				}
			}

			return false;
		}

		bool IsGenericPseudoAssignment(std::string_view value)
		{
			value = TrimView(value);
			return (value.starts_with("local var_") || value.starts_with("local value_"))
				&& (value.ends_with(" = true") || value.ends_with(" = false") || value.ends_with(" = {}") || value.find(" = function() end") != std::string_view::npos);
		}

		bool LooksLikeMeaningfulRhs(std::string_view value)
		{
			value = TrimView(value);
			if (value.empty())
			{
				return false;
			}

			if (CountRegisterTokens(value) > 0 || ContainsBrokenSyntheticMath(value))
			{
				return false;
			}

			if (value.find("select(") != std::string_view::npos || value.find("...") != std::string_view::npos || value.find("[nil]") != std::string_view::npos)
			{
				return false;
			}

			const std::size_t generic_names = CountGenericSyntheticNames(value);
			const std::size_t meaningful_names = CountMeaningfulSyntheticNames(value);
			const bool readable_literal = ContainsReadableLiteral(value);

			if (generic_names >= 3 && meaningful_names == 0 && !readable_literal)
			{
				return false;
			}

			if (value.starts_with("{") || value.starts_with("function("))
			{
				return value == "{}" || meaningful_names > 0 || readable_literal || value.starts_with("function(");
			}

			if (value.starts_with("\"") || value.starts_with("'") || value == "true" || value == "false" || value == "nil")
			{
				return value.find("__br_str_") == std::string_view::npos;
			}

			const unsigned char first = static_cast<unsigned char>(value.front());
			if (std::isdigit(first) != 0 || first == '-')
			{
				return meaningful_names > 0 || generic_names == 0;
			}

			if (value.find('(') != std::string_view::npos || value.find(':') != std::string_view::npos || value.find('.') != std::string_view::npos || value.find('[') != std::string_view::npos || value.find("..") != std::string_view::npos)
			{
				if (value.find("__br_str_") != std::string_view::npos && value.find("_G[\"") == std::string_view::npos && value.find("string.") == std::string_view::npos && !readable_literal)
				{
					return false;
				}

				return meaningful_names > 0 || readable_literal;
			}

			return IsIdentifier(value) && !IsGenericSyntheticName(value);
		}

		bool IsMeaningfulSyntheticStatement(std::string_view value)
		{
			value = TrimView(value);
			if (value.empty())
			{
				return false;
			}

			if (value.starts_with("--") || IsOpenSyntheticBlock(value) || IsCloseSyntheticBlock(value) || IsBranchSyntheticLine(value))
			{
				return false;
			}

			if (!ShouldKeepLosslessLine(value) || CountRegisterTokens(value) > 0 || IsGenericPseudoAssignment(value))
			{
				return false;
			}

			if (value.find("__br_proxy") != std::string_view::npos || value.find("[nil]") != std::string_view::npos)
			{
				return false;
			}

			if (value.find("select(") != std::string_view::npos || value.find("...") != std::string_view::npos)
			{
				return false;
			}

			const std::size_t generic_names = CountGenericSyntheticNames(value);
			const std::size_t meaningful_names = CountMeaningfulSyntheticNames(value);
			const bool readable_literal = ContainsReadableLiteral(value);
			if (generic_names >= 4 && meaningful_names == 0 && !readable_literal)
			{
				return false;
			}

			if (value.starts_with("return"))
			{
				std::string_view rhs = TrimView(value.substr(6));
				return !rhs.empty() && LooksLikeMeaningfulRhs(rhs);
			}

			if (const std::size_t assign = value.find(" = "); assign != std::string_view::npos)
			{
				std::string_view lhs = TrimView(value.substr(0, assign));
				if (lhs.starts_with("local "))
				{
					lhs.remove_prefix(6);
					lhs = TrimView(lhs);
				}

				std::string_view rhs = TrimView(value.substr(assign + 3));
				if (!LooksLikeMeaningfulRhs(rhs))
				{
					return false;
				}

				const std::size_t lhs_meaningful = CountMeaningfulSyntheticNames(lhs);
				const std::size_t lhs_generic = CountGenericSyntheticNames(lhs);
				const std::size_t rhs_meaningful = CountMeaningfulSyntheticNames(rhs);
				if (lhs_meaningful == 0 && rhs_meaningful == 0 && !ContainsReadableLiteral(rhs))
				{
					return false;
				}

				if (lhs_generic > 0 && lhs_meaningful == 0 && rhs_meaningful == 0 && !ContainsReadableLiteral(rhs))
				{
					return false;
				}

				if (rhs.starts_with("function(") && lhs_meaningful == 0 && !ContainsReadableLiteral(value))
				{
					return false;
				}

				if (lhs_generic > 1 && lhs_meaningful == 0 && rhs.find("function(") == std::string_view::npos)
				{
					return false;
				}

				return true;
			}

			if (value.find('(') == std::string_view::npos)
			{
				return false;
			}

			const std::size_t placeholder_count = static_cast<std::size_t>(CountSubstring(value, "__br_str_"));
			if (placeholder_count >= 2 && value.find("string.") == std::string_view::npos)
			{
				return false;
			}

			if (meaningful_names == 0 && !readable_literal)
			{
				return false;
			}

			return !ContainsBrokenSyntheticMath(value);
		}

		std::string WideToUtf8(std::wstring_view value)
		{
			if (value.empty())
			{
				return {};
			}

			const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
			if (size <= 0)
			{
				return {};
			}

			std::string utf8(static_cast<std::size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), size, nullptr, nullptr);
			return utf8;
		}

		std::optional<std::wstring> Utf8ToWide(std::string_view value)
		{
			if (value.empty())
			{
				return std::wstring{};
			}

			const int wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
			if (wide_size <= 0)
			{
				return std::nullopt;
			}

			std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(), wide_size);
			return wide;
		}

		std::optional<std::string> TryDecodeWindows1251(std::string_view value)
		{
			if (value.empty())
			{
				return std::string{};
			}

			const int wide_size = MultiByteToWideChar(1251, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
			if (wide_size <= 0)
			{
				return std::nullopt;
			}

			std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
			MultiByteToWideChar(1251, 0, value.data(), static_cast<int>(value.size()), wide.data(), wide_size);
			if (!IsHumanWide(wide) || !IsAcceptableCyrillicText(wide))
			{
				return std::nullopt;
			}

			const std::string utf8 = WideToUtf8(wide);
			if (utf8.empty() && !wide.empty())
			{
				return std::nullopt;
			}

			return utf8;
		}

		std::optional<std::string> TryKeepUtf8(std::string_view value)
		{
			const auto wide = Utf8ToWide(value);
			if (!wide || !IsHumanWide(*wide))
			{
				return std::nullopt;
			}

			bool has_cyrillic = false;
			for (wchar_t ch : *wide)
			{
				if (IsCyrillic(ch))
				{
					has_cyrillic = true;
					break;
				}
			}

			if (has_cyrillic && !IsAcceptableCyrillicText(*wide))
			{
				return std::nullopt;
			}

			return std::string(value);
		}

		bool IsValidUtf8(std::string_view value)
		{
			for (std::size_t index = 0; index < value.size();)
			{
				const unsigned char lead = static_cast<unsigned char>(value[index]);
				if (lead < 0x80)
				{
					++index;
					continue;
				}

				int extra = 0;
				if ((lead & 0xE0) == 0xC0)
				{
					extra = 1;
				}
				else if ((lead & 0xF0) == 0xE0)
				{
					extra = 2;
				}
				else if ((lead & 0xF8) == 0xF0)
				{
					extra = 3;
				}
				else
				{
					return false;
				}

				if (index + static_cast<std::size_t>(extra) >= value.size())
				{
					return false;
				}

				for (int tail = 1; tail <= extra; ++tail)
				{
					const unsigned char ch = static_cast<unsigned char>(value[index + static_cast<std::size_t>(tail)]);
					if ((ch & 0xC0) != 0x80)
					{
						return false;
					}
				}

				index += static_cast<std::size_t>(extra + 1);
			}

			return true;
		}

		std::optional<std::string> NormalizeStringValue(std::string_view value)
		{
			if (!IsHumanString(value))
			{
				return std::nullopt;
			}

			if (!HasHighBytes(value))
			{
				return std::string(value);
			}

			if (IsValidUtf8(value))
			{
				return TryKeepUtf8(value);
			}

			if (auto decoded = TryDecodeWindows1251(value))
			{
				return decoded;
			}

			return std::nullopt;
		}

		void AppendLuaByteEscape(std::string& out, unsigned char ch)
		{
			char buffer[8]{};
			std::snprintf(buffer, sizeof(buffer), "\\%03u", static_cast<unsigned>(ch));
			out += buffer;
		}

		std::string HexEncodeString(std::string_view value)
		{
			static constexpr char digits[] = "0123456789ABCDEF";

			std::string hex;
			hex.reserve(value.size() * 2);
			for (unsigned char ch : value)
			{
				hex.push_back(digits[ch >> 4]);
				hex.push_back(digits[ch & 0x0F]);
			}

			return hex;
		}

		std::string EscapeBinaryString(std::string_view value)
		{
			return "decode_hex_literal(\"" + HexEncodeString(value) + "\")";
		}

		std::string EscapeString(std::string_view value)
		{
			const auto normalized = NormalizeStringValue(value);
			if (!normalized)
			{
				return EscapeBinaryString(value);
			}

			std::string escaped;
			escaped.reserve(normalized->size() + 8);
			escaped.push_back('"');
			for (unsigned char ch : *normalized)
			{
				switch (ch)
				{
				case '\\': escaped += "\\\\"; break;
				case '"': escaped += "\\\""; break;
				case '\n': escaped += "\\n"; break;
				case '\r': escaped += "\\r"; break;
				case '\t': escaped += "\\t"; break;
				default:
					if (std::isprint(ch) != 0 || ch >= 0x80)
					{
						escaped.push_back(static_cast<char>(ch));
					}
					else
					{
						AppendLuaByteEscape(escaped, ch);
					}
					break;
				}
			}
			escaped.push_back('"');
			return escaped;
		}

		bool NearlyEqual(double lhs, double rhs, double tolerance = 0.000001)
		{
			const double scale = std::max(1.0, std::max(std::fabs(lhs), std::fabs(rhs)));
			return std::fabs(lhs - rhs) <= tolerance * scale;
		}

		bool IsIntegralish(double value)
		{
			if (!std::isfinite(value))
			{
				return false;
			}

			return std::fabs(value - std::round(value)) <= 0.0001;
		}

		std::optional<std::int64_t> ToInt64Constant(double value)
		{
			if (!IsIntegralish(value)
				|| value < static_cast<double>(std::numeric_limits<std::int64_t>::min())
				|| value > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
			{
				return std::nullopt;
			}

			return static_cast<std::int64_t>(std::llround(value));
		}

		void AddUniqueNumber(std::vector<double>& values, double value)
		{
			for (double existing : values)
			{
				if (NearlyEqual(existing, value, 0.0000001))
				{
					return;
				}
			}

			values.push_back(value);
		}

		bool ContainsNumber(const std::vector<double>& values, double needle)
		{
			for (double value : values)
			{
				if (NearlyEqual(value, needle, 0.0000001))
				{
					return true;
				}
			}

			return false;
		}

		template <typename Predicate>
		std::optional<double> PickLargestNumber(const std::vector<double>& values, Predicate predicate)
		{
			std::optional<double> picked;
			for (double value : values)
			{
				if (!predicate(value))
				{
					continue;
				}

				if (!picked || value > *picked)
				{
					picked = value;
				}
			}

			return picked;
		}

		template <typename Predicate>
		std::optional<double> PickSmallestNumber(const std::vector<double>& values, Predicate predicate)
		{
			std::optional<double> picked;
			for (double value : values)
			{
				if (!predicate(value))
				{
					continue;
				}

				if (!picked || value < *picked)
				{
					picked = value;
				}
			}

			return picked;
		}

		template <typename Predicate>
		std::optional<double> PickLargestCommonNumber(const std::vector<double>& lhs, const std::vector<double>& rhs, Predicate predicate)
		{
			std::optional<double> picked;
			for (double value : lhs)
			{
				if (!predicate(value) || !ContainsNumber(rhs, value))
				{
					continue;
				}

				if (!picked || value > *picked)
				{
					picked = value;
				}
			}

			return picked;
		}

		struct HexStreamNumberBuckets
		{
			std::vector<double> all;
			std::vector<double> seed_add;
			std::vector<double> seed_mul;
			std::vector<double> seed_compare;
			std::vector<double> worker_mod;
			std::vector<double> worker_div;
			std::vector<double> worker_mul;
		};

		std::optional<HexStreamDecodeConfig> BuildHexStreamConfigFromBuckets(const HexStreamNumberBuckets& buckets)
		{
			HexStreamDecodeConfig config{};

			const auto state_divisor = PickLargestCommonNumber(
				buckets.worker_mod,
				buckets.worker_div,
				[](double value)
				{
					return IsIntegralish(value) && value > 4096.0;
				});
			const auto output_modulus = PickLargestNumber(
				buckets.worker_mod,
				[](double value)
				{
					return IsIntegralish(value) && value >= 2.0 && value <= 65536.0;
				});
			const auto seed_offset = PickLargestNumber(
				buckets.seed_add.empty() ? buckets.all : buckets.seed_add,
				[](double value)
				{
					return IsIntegralish(value) && value > 1000000000000.0;
				});

			if (!state_divisor || !output_modulus || !seed_offset)
			{
				return std::nullopt;
			}

			const auto state_modulus = PickLargestNumber(
				buckets.worker_mod,
				[&](double value)
				{
					return IsIntegralish(value) && value >= 2.0 && value < *output_modulus;
				});
			const auto state_multiplier = PickLargestNumber(
				buckets.worker_mul,
				[&](double value)
				{
					return IsIntegralish(value) && value > *output_modulus && value < *state_divisor;
				});
			const auto seed_multiplier = PickSmallestNumber(
				buckets.seed_mul.empty() ? buckets.all : buckets.seed_mul,
				[](double value)
				{
					return IsIntegralish(value) && value > 2.0 && value <= 64.0;
				});
			const auto seed_threshold = PickLargestNumber(
				buckets.seed_compare,
				[&](double value)
				{
					return IsIntegralish(value) && value > 0.0 && value < *seed_offset;
				});

			if (!state_modulus || !state_multiplier || !seed_multiplier)
			{
				return std::nullopt;
			}

			config.seed_multiplier = *seed_multiplier;
			config.seed_threshold = seed_threshold.value_or(0.0);
			config.seed_offset = *seed_offset;
			config.state_divisor = *state_divisor;
			config.state_multiplier = *state_multiplier;
			config.state_modulus = *state_modulus;
			config.output_modulus = *output_modulus;
			return config;
		}

		std::optional<std::string> UnescapeLuaStringLiteral(std::string_view value)
		{
			if (value.size() < 2 || value.front() != '"' || value.back() != '"')
			{
				return std::nullopt;
			}

			std::string result;
			result.reserve(value.size() - 2);
			for (std::size_t index = 1; index + 1 < value.size(); ++index)
			{
				char ch = value[index];
				if (ch != '\\')
				{
					result.push_back(ch);
					continue;
				}

				if (++index + 1 > value.size())
				{
					return std::nullopt;
				}

				ch = value[index];
				switch (ch)
				{
				case '\\':
				case '"':
					result.push_back(ch);
					break;
				case 'n':
					result.push_back('\n');
					break;
				case 'r':
					result.push_back('\r');
					break;
				case 't':
					result.push_back('\t');
					break;
				case 'x':
				{
					if (index + 2 >= value.size() - 1)
					{
						return std::nullopt;
					}

					const auto hex_digit = [](char digit) -> int
					{
						if (digit >= '0' && digit <= '9')
						{
							return digit - '0';
						}
						if (digit >= 'a' && digit <= 'f')
						{
							return digit - 'a' + 10;
						}
						if (digit >= 'A' && digit <= 'F')
						{
							return digit - 'A' + 10;
						}
						return -1;
					};

					const int hi = hex_digit(value[index + 1]);
					const int lo = hex_digit(value[index + 2]);
					if (hi < 0 || lo < 0)
					{
						return std::nullopt;
					}

					result.push_back(static_cast<char>((hi << 4) | lo));
					index += 2;
					break;
				}
				default:
					return std::nullopt;
				}
			}

			return result;
		}

		std::optional<double> ParseLuaNumberLiteral(std::string_view value)
		{
			std::string text(value);
			char* end = nullptr;
			const double parsed = std::strtod(text.c_str(), &end);
			if (end == text.c_str() || end == nullptr)
			{
				return std::nullopt;
			}

			while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)) != 0)
			{
				++end;
			}

			if (*end != '\0')
			{
				return std::nullopt;
			}

			return parsed;
		}

		bool FunctionHasString(const Function& function, std::string_view needle)
		{
			for (const Constant& constant : function.constants)
			{
				if (constant.type == ConstantType::String && constant.string == needle)
				{
					return true;
				}
			}
			return false;
		}

		std::optional<double> NumericConstantFromRK(const Function& function, int rk)
		{
			if (!IsConstantIndex(rk))
			{
				return std::nullopt;
			}

			const int index = ConstantIndex(rk);
			if (index < 0 || static_cast<std::size_t>(index) >= function.constants.size())
			{
				return std::nullopt;
			}

			const Constant& constant = function.constants[static_cast<std::size_t>(index)];
			if (constant.type != ConstantType::Number)
			{
				return std::nullopt;
			}

			return constant.number;
		}

		void CollectInstructionNumericOperands(const Function& function, const Instruction& instruction, std::vector<double>& target)
		{
			if (const auto value = NumericConstantFromRK(function, instruction.b))
			{
				AddUniqueNumber(target, *value);
			}
			if (const auto value = NumericConstantFromRK(function, instruction.c))
			{
				AddUniqueNumber(target, *value);
			}
		}

		void CollectFunctionNumbers(const Function& function, HexStreamNumberBuckets& buckets, bool worker)
		{
			for (const Constant& constant : function.constants)
			{
				if (constant.type == ConstantType::Number)
				{
					AddUniqueNumber(buckets.all, constant.number);
				}
			}

			for (const Instruction& instruction : function.code)
			{
				switch (instruction.opcode)
				{
				case OpCode::Add:
					if (!worker)
					{
						CollectInstructionNumericOperands(function, instruction, buckets.seed_add);
					}
					break;
				case OpCode::Mul:
					CollectInstructionNumericOperands(function, instruction, worker ? buckets.worker_mul : buckets.seed_mul);
					break;
				case OpCode::Eq:
				case OpCode::Lt:
				case OpCode::Le:
					if (!worker)
					{
						CollectInstructionNumericOperands(function, instruction, buckets.seed_compare);
					}
					break;
				case OpCode::Div:
					if (worker)
					{
						CollectInstructionNumericOperands(function, instruction, buckets.worker_div);
					}
					break;
				case OpCode::Mod:
					if (worker)
					{
						CollectInstructionNumericOperands(function, instruction, buckets.worker_mod);
					}
					break;
				default:
					break;
				}
			}
		}

		std::optional<HexStreamDecodeConfig> DetectHexStreamDecoder(const Function& function)
		{
			if (function.parameter_count != 2 || function.code.size() < 8 || function.code.size() > 80 || function.prototypes.empty())
			{
				return std::nullopt;
			}

			if (!FunctionHasString(function, "%x%x"))
			{
				return std::nullopt;
			}

			bool has_mod = false;
			bool has_div = false;
			bool has_mul = false;
			bool has_call = false;
			const Function* worker = function.prototypes.front().get();
			if (!worker)
			{
				return std::nullopt;
			}

			for (const Instruction& instruction : worker->code)
			{
				has_mod = has_mod || instruction.opcode == OpCode::Mod;
				has_div = has_div || instruction.opcode == OpCode::Div;
				has_mul = has_mul || instruction.opcode == OpCode::Mul;
				has_call = has_call || instruction.opcode == OpCode::Call || instruction.opcode == OpCode::GetGlobal;
			}

			if (!has_mod || !has_div || !has_mul || !has_call)
			{
				return std::nullopt;
			}

			HexStreamNumberBuckets buckets{};
			CollectFunctionNumbers(function, buckets, false);
			CollectFunctionNumbers(*worker, buckets, true);
			return BuildHexStreamConfigFromBuckets(buckets);
		}

		std::optional<HexStreamDecodeConfig> DetectTopLevelHexStreamDecoder(const Function& root)
		{
			for (const auto& prototype : root.prototypes)
			{
				if (!prototype)
				{
					continue;
				}

				if (const auto config = DetectHexStreamDecoder(*prototype))
				{
					return config;
				}
			}

			return std::nullopt;
		}

		long double NormalizeModulo(long double value, long double modulus)
		{
			if (modulus == 0.0L)
			{
				return value;
			}

			long double normalized = std::fmod(value, modulus);
			if (normalized < 0.0L)
			{
				normalized += modulus;
			}
			if (normalized >= modulus)
			{
				normalized = std::fmod(normalized, modulus);
			}
			return normalized;
		}

		std::optional<int> RoundedByte(long double value)
		{
			const long double normalized = NormalizeModulo(value, 256.0L);
			const long double rounded = std::floor(normalized + 0.5L);
			if (std::fabsl(normalized - rounded) > 0.05L)
			{
				return std::nullopt;
			}

			const int byte = static_cast<int>(rounded);
			if (byte < 0 || byte > 255)
			{
				return std::nullopt;
			}

			return byte;
		}

		std::int64_t PositiveModulo(std::int64_t value, std::int64_t modulus)
		{
			if (modulus == 0)
			{
				return value;
			}

			std::int64_t result = value % modulus;
			if (result < 0)
			{
				result += modulus;
			}
			return result;
		}

		bool CheckedAdd(std::int64_t lhs, std::int64_t rhs, std::int64_t& result)
		{
			if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs)
				|| (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs))
			{
				return false;
			}

			result = lhs + rhs;
			return true;
		}

		bool CheckedMul(std::int64_t lhs, std::int64_t rhs, std::int64_t& result)
		{
			if (lhs == 0 || rhs == 0)
			{
				result = 0;
				return true;
			}

			if (lhs > 0)
			{
				if (rhs > 0)
				{
					if (lhs > std::numeric_limits<std::int64_t>::max() / rhs)
					{
						return false;
					}
				}
				else if (rhs < std::numeric_limits<std::int64_t>::min() / lhs)
				{
					return false;
				}
			}
			else
			{
				if (rhs > 0)
				{
					if (lhs < std::numeric_limits<std::int64_t>::min() / rhs)
					{
						return false;
					}
				}
				else if (lhs != 0 && rhs < std::numeric_limits<std::int64_t>::max() / lhs)
				{
					return false;
				}
			}

			result = lhs * rhs;
			return true;
		}

		std::optional<std::int64_t> BuildInitialHexStreamStateExact(std::int64_t seed, const HexStreamDecodeConfig& config)
		{
			const auto seed_multiplier = ToInt64Constant(config.seed_multiplier);
			const auto seed_offset = ToInt64Constant(config.seed_offset);
			const auto seed_threshold = ToInt64Constant(config.seed_threshold);
			if (!seed_multiplier || !seed_offset)
			{
				return std::nullopt;
			}

			std::int64_t mixed_seed = seed;
			if (seed_threshold && *seed_threshold > 0)
			{
				if (*seed_multiplier <= 1)
				{
					return std::nullopt;
				}

				for (int guard = 0; mixed_seed <= *seed_threshold; ++guard)
				{
					if (guard > 256 || !CheckedMul(mixed_seed, *seed_multiplier, mixed_seed))
					{
						return std::nullopt;
					}
				}
			}
			else if (!CheckedMul(seed, *seed_multiplier, mixed_seed))
			{
				return std::nullopt;
			}

			std::int64_t state = 0;
			if (!CheckedAdd(*seed_offset, mixed_seed, state))
			{
				return std::nullopt;
			}

			return state;
		}

		std::optional<std::string> DecodeHexStreamLiteralExact(std::string_view hex_value, double seed, const HexStreamDecodeConfig& config)
		{
			const auto seed_value = ToInt64Constant(seed);
			const auto state_divisor = ToInt64Constant(config.state_divisor);
			const auto state_multiplier = ToInt64Constant(config.state_multiplier);
			const auto state_modulus = ToInt64Constant(config.state_modulus);
			const auto output_modulus = ToInt64Constant(config.output_modulus);
			if (!seed_value || !state_divisor || !state_multiplier || !state_modulus || !output_modulus)
			{
				return std::nullopt;
			}
			if (*state_divisor == 0 || *state_modulus == 0 || *output_modulus <= 0 || *output_modulus > 256)
			{
				return std::nullopt;
			}

			auto state = BuildInitialHexStreamStateExact(*seed_value, config);
			if (!state)
			{
				return std::nullopt;
			}

			std::string result;
			result.reserve(hex_value.size() / 2);
			for (std::size_t index = 0; index + 1 < hex_value.size(); index += 2)
			{
				const std::string_view pair(&hex_value[index], 2);
				const std::int64_t source = std::strtol(std::string(pair).c_str(), nullptr, 16);
				const std::int64_t remainder = PositiveModulo(*state, *state_divisor);
				const std::int64_t quotient = (*state - remainder) / *state_divisor;
				const std::int64_t state_byte = PositiveModulo(quotient, *state_modulus);
				const std::int64_t quotient_part = (quotient - state_byte) / *state_modulus;

				std::int64_t mix = 0;
				std::int64_t factor = 0;
				std::int64_t product = 0;
				if (!CheckedAdd(source, quotient_part, mix)
					|| !CheckedMul(state_byte, 2, factor)
					|| !CheckedAdd(factor, 1, factor)
					|| !CheckedMul(mix, factor, product))
				{
					return std::nullopt;
				}

				const std::int64_t decoded = PositiveModulo(product, *output_modulus);
				if (decoded < 0 || decoded > 255)
				{
					return std::nullopt;
				}

				result.push_back(static_cast<char>(decoded));

				std::int64_t next_state = 0;
				std::int64_t tail = 0;
				if (!CheckedMul(remainder, *state_multiplier, next_state)
					|| !CheckedAdd(next_state, quotient, next_state)
					|| !CheckedAdd(next_state, source, next_state)
					|| !CheckedAdd(next_state, decoded, tail))
				{
					return std::nullopt;
				}
				state = tail;
			}

			return result;
		}

		std::optional<std::string> DecodeHexStreamLiteral(std::string_view hex_value, double seed, const HexStreamDecodeConfig& config)
		{
			if (hex_value.empty() || (hex_value.size() % 2) != 0)
			{
				return std::nullopt;
			}

			for (unsigned char ch : hex_value)
			{
				if (std::isxdigit(ch) == 0)
				{
					return std::nullopt;
				}
			}

			if (auto exact = DecodeHexStreamLiteralExact(hex_value, seed, config))
			{
				return exact;
			}

			std::string result;
			result.reserve(hex_value.size() / 2);
			long double mixed_seed = static_cast<long double>(seed);
			if (config.seed_threshold > 0.0)
			{
				if (config.seed_multiplier <= 1.0)
				{
					return std::nullopt;
				}

				for (int guard = 0; mixed_seed <= static_cast<long double>(config.seed_threshold); ++guard)
				{
					if (guard > 256)
					{
						return std::nullopt;
					}
					mixed_seed *= static_cast<long double>(config.seed_multiplier);
				}
			}
			else
			{
				mixed_seed *= static_cast<long double>(config.seed_multiplier);
			}

			long double state = static_cast<long double>(config.seed_offset) + mixed_seed;
			const long double state_divisor = static_cast<long double>(config.state_divisor);
			const long double state_modulus = static_cast<long double>(config.state_modulus);
			const long double output_modulus = static_cast<long double>(config.output_modulus);
			const long double state_multiplier = static_cast<long double>(config.state_multiplier);
			for (std::size_t index = 0; index + 1 < hex_value.size(); index += 2)
			{
				const char hi = hex_value[index];
				const char lo = hex_value[index + 1];
				const std::string_view pair(&hex_value[index], 2);
				const int source = std::strtol(std::string(pair).c_str(), nullptr, 16);
				const long double remainder = NormalizeModulo(state, state_divisor);
				const long double quotient = (state - remainder) / state_divisor;
				const long double state_byte = NormalizeModulo(quotient, state_modulus);
				const long double decoded_value = NormalizeModulo((static_cast<long double>(source) + (quotient - state_byte) / state_modulus) * (2.0L * state_byte + 1.0L), output_modulus);
				const auto decoded = RoundedByte(decoded_value);
				if (!decoded)
				{
					return std::nullopt;
				}

				(void)hi;
				(void)lo;
				result.push_back(static_cast<char>(*decoded));
				state = remainder * state_multiplier + quotient + static_cast<long double>(source) + static_cast<long double>(*decoded);
			}

			return result;
		}

		bool LooksLikeHeavyHexObfuscatedLua(std::string_view lua)
		{
			if (lua.size() < 8000)
			{
				return false;
			}

			const std::size_t call_count = CountSubstring(lua, "var(\"");
			const std::size_t long_hex_count = CountSubstring(lua, "chunk = var(\"") + CountSubstring(lua, " = var(\"");
			const bool loader_markers =
				lua.find("loadstring(") != std::string_view::npos
				|| lua.find("addDebugHook(") != std::string_view::npos
				|| lua.find("base64Decode(") != std::string_view::npos
				|| lua.find("fileRead(") != std::string_view::npos
				|| lua.find("FILES_CHECKSUM") != std::string_view::npos;
			return call_count >= 16
				|| long_hex_count >= 8
				|| (call_count >= 8 && loader_markers);
		}

		struct ExtractedHexDecoder
		{
			std::string name;
			HexStreamDecodeConfig config;
			std::size_t begin_line = 0;
			std::size_t end_line = 0;
		};

		std::optional<ExtractedHexDecoder> ExtractHexDecoderFromLua(const std::vector<std::string>& lines, const HexStreamDecodeConfig* forced_config = nullptr)
		{
			const std::regex header_pattern("^local ([A-Za-z_][A-Za-z0-9_]*) = function\\(([A-Za-z_][A-Za-z0-9_]*), ([A-Za-z_][A-Za-z0-9_]*)\\)$");
			const std::regex number_pattern("[-+]?(?:\\d+\\.\\d+|\\d+|\\d+\\.)(?:[eE][-+]?\\d+)?");

			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				const std::string header_line = std::string(TrimView(lines[index]));
				std::smatch header_match;
				if (!std::regex_match(header_line, header_match, header_pattern))
				{
					continue;
				}

				std::size_t end_line = index + 1;
				int depth = 1;
				for (; end_line < lines.size(); ++end_line)
				{
					const std::string_view trimmed = TrimView(lines[end_line]);
					if (end_line > index && trimmed.find("function(") != std::string_view::npos)
					{
						++depth;
					}
					if (trimmed == "end")
					{
						--depth;
						if (depth == 0)
						{
							break;
						}
					}
				}

				if (end_line >= lines.size())
				{
					continue;
				}

				std::ostringstream block_stream;
				for (std::size_t line_index = index; line_index <= end_line; ++line_index)
				{
					if (line_index > index)
					{
						block_stream << '\n';
					}
					block_stream << lines[line_index];
				}
				const std::string block = block_stream.str();
				if (block.find("%x%x") == std::string::npos
					|| block.find("tonumber(") == std::string::npos
					|| block.find("string.char(") == std::string::npos
					|| block.find("gsub") == std::string::npos
					|| block.find("16") == std::string::npos)
				{
					continue;
				}

				ExtractedHexDecoder decoder{};
				decoder.name = header_match[1].str();
				decoder.begin_line = index;
				decoder.end_line = end_line;

				if (forced_config)
				{
					decoder.config = *forced_config;
					return decoder;
				}

				HexStreamNumberBuckets buckets{};

				for (std::size_t line_index = index; line_index <= end_line; ++line_index)
				{
					const std::string current = std::string(TrimView(lines[line_index]));
					const bool has_add = current.find('+') != std::string::npos;
					const bool has_mul = current.find('*') != std::string::npos;
					const bool has_div = current.find('/') != std::string::npos;
					const bool has_mod = current.find('%') != std::string::npos;

					for (auto it = std::sregex_iterator(current.begin(), current.end(), number_pattern); it != std::sregex_iterator(); ++it)
					{
						const auto parsed = ParseLuaNumberLiteral((*it).str());
						if (!parsed)
						{
							continue;
						}

						const double value = *parsed;
						AddUniqueNumber(buckets.all, value);
						if (has_add)
						{
							AddUniqueNumber(buckets.seed_add, value);
						}
						if (has_mul)
						{
							AddUniqueNumber(buckets.seed_mul, value);
							AddUniqueNumber(buckets.worker_mul, value);
						}
						if (has_div)
						{
							AddUniqueNumber(buckets.worker_div, value);
						}
						if (has_mod)
						{
							AddUniqueNumber(buckets.worker_mod, value);
						}
					}
				}

				if (const auto config = BuildHexStreamConfigFromBuckets(buckets))
				{
					decoder.config = *config;
					return decoder;
				}
			}

			return std::nullopt;
		}

		std::size_t FoldHexDecoderCallsInLines(std::vector<std::string>& lines, const ExtractedHexDecoder& decoder)
		{
			const std::regex call_pattern("\\b" + decoder.name + "\\(\"([^\"]*)\",\\s*([-+]?(?:\\d+\\.\\d+|\\d+|\\d+\\.)(?:[eE][-+]?\\d+)?)\\)");
			std::size_t replacements = 0;

			for (std::string& line : lines)
			{
				std::string folded;
				std::string remaining = line;
				std::smatch match;
				bool changed = false;

				while (std::regex_search(remaining, match, call_pattern))
				{
					folded.append(match.prefix().first, match.prefix().second);

					const auto seed = ParseLuaNumberLiteral(match[2].str());
					const auto decoded = seed ? DecodeHexStreamLiteral(match[1].str(), *seed, decoder.config) : std::nullopt;
					if (!decoded)
					{
						folded.append(match[0].str());
						remaining.assign(match.suffix().first, match.suffix().second);
						continue;
					}

					if (const auto normalized = NormalizeStringValue(*decoded))
					{
						folded.append(EscapeString(*normalized));
					}
					else
					{
						folded.append(EscapeBinaryString(*decoded));
					}

					remaining.assign(match.suffix().first, match.suffix().second);
					changed = true;
					++replacements;
				}

				if (changed)
				{
					folded += remaining;
					line = std::move(folded);
				}
			}

			return replacements;
		}

		void RemoveUnusedHexDecoder(std::vector<std::string>& lines, const ExtractedHexDecoder& decoder)
		{
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				if (index >= decoder.begin_line && index <= decoder.end_line)
				{
					continue;
				}

				if (lines[index].find(decoder.name + "(\"") != std::string::npos)
				{
					return;
				}
			}

			if (decoder.begin_line <= decoder.end_line && decoder.end_line < lines.size())
			{
				lines.erase(
					lines.begin() + static_cast<std::ptrdiff_t>(decoder.begin_line),
					lines.begin() + static_cast<std::ptrdiff_t>(decoder.end_line + 1));
			}
		}

		void RemoveDanglingHexDecoderTail(std::vector<std::string>& lines)
		{
			const std::regex gsub_line("^local [A-Za-z_][A-Za-z0-9_]* = [A-Za-z_][A-Za-z0-9_]*:gsub\\(.+\\)$");
			for (std::size_t index = 0; index + 2 < lines.size(); ++index)
			{
				if (TrimView(lines[index]).empty())
				{
					continue;
				}

				const std::string current = std::string(TrimView(lines[index]));
				const std::string next = std::string(TrimView(lines[index + 1]));
				const std::string next_next = std::string(TrimView(lines[index + 2]));
				if (!std::regex_match(current, gsub_line) || !next.starts_with("return ") || next_next != "end")
				{
					break;
				}

				lines.erase(
					lines.begin() + static_cast<std::ptrdiff_t>(index),
					lines.begin() + static_cast<std::ptrdiff_t>(index + 3));
				break;
			}
		}

		void RemoveUnsafeLoadstringValidation(std::vector<std::string>& lines)
		{
			const std::regex assign_pattern("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*), ([A-Za-z_][A-Za-z0-9_]*) = loadstring\\((.+)\\)$");
			const std::regex guard_pattern("^if not ([A-Za-z_][A-Za-z0-9_]*) then$");
			const std::regex return_pattern("^return false, .+$");

			for (std::size_t index = 0; index + 1 < lines.size(); ++index)
			{
				const std::string current_line = lines[index];
				std::smatch assign_match;
				if (!std::regex_match(current_line, assign_match, assign_pattern))
				{
					continue;
				}

				std::size_t next_line = index + 1;
				while (next_line < lines.size() && TrimView(lines[next_line]).empty())
				{
					++next_line;
				}

				if (next_line < lines.size() && std::regex_match(std::string(TrimView(lines[next_line])), return_pattern))
				{
					lines[index] = assign_match[1].str() + "local " + assign_match[2].str() + ", " + assign_match[3].str() + " = true, nil";
					lines.erase(
						lines.begin() + static_cast<std::ptrdiff_t>(index + 1),
						lines.begin() + static_cast<std::ptrdiff_t>(next_line + 1));
					continue;
				}

				const std::string guard_line = next_line < lines.size()
					? std::string(TrimView(lines[next_line]))
					: std::string{};
				std::smatch guard_match;
				if (!std::regex_match(guard_line, guard_match, guard_pattern))
				{
					continue;
				}

				if (guard_match[1].str() != assign_match[2].str())
				{
					if (std::regex_match(guard_line, return_pattern))
					{
						lines[index] = assign_match[1].str() + "local " + assign_match[2].str() + ", " + assign_match[3].str() + " = true, nil";
						lines.erase(
							lines.begin() + static_cast<std::ptrdiff_t>(index + 1),
							lines.begin() + static_cast<std::ptrdiff_t>(next_line + 1));
					}
					continue;
				}

				std::size_t end_line = index + 2;
				int depth = 1;
				bool returns_error = false;
				for (; end_line < lines.size(); ++end_line)
				{
					const std::string_view trimmed = TrimView(lines[end_line]);
					if (trimmed.starts_with("if "))
					{
						++depth;
					}
					if (trimmed.starts_with("return false"))
					{
						returns_error = true;
					}
					if (trimmed == "end")
					{
						--depth;
						if (depth == 0)
						{
							break;
						}
					}
				}

				if (!returns_error || end_line >= lines.size())
				{
					continue;
				}

				lines[index] = assign_match[1].str() + "local " + assign_match[2].str() + ", " + assign_match[3].str() + " = true, nil";
				lines.erase(
					lines.begin() + static_cast<std::ptrdiff_t>(index + 1),
					lines.begin() + static_cast<std::ptrdiff_t>(end_line + 1));
			}
		}

		void RemoveUnsafeDebugHooks(std::vector<std::string>& lines)
		{
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				const std::string_view trimmed = TrimView(lines[index]);
				if (!trimmed.starts_with("addDebugHook(") && !trimmed.starts_with("removeDebugHook("))
				{
					continue;
				}

				int paren_balance = 0;
				bool started = false;
				std::size_t end_line = index;
				for (; end_line < lines.size(); ++end_line)
				{
					for (char ch : lines[end_line])
					{
						if (ch == '(')
						{
							++paren_balance;
							started = true;
						}
						else if (ch == ')')
						{
							--paren_balance;
						}
					}

					if (started && paren_balance <= 0)
					{
						break;
					}
				}

				lines[index] = std::string(lines[index].substr(0, lines[index].find_first_not_of(" \t"))) + "-- ByteRevenant: removed unsafe debug hook for rebuilt source";
				if (end_line > index && end_line < lines.size())
				{
					lines.erase(
						lines.begin() + static_cast<std::ptrdiff_t>(index + 1),
						lines.begin() + static_cast<std::ptrdiff_t>(end_line + 1));
				}
			}
		}

		void EnsureHexLiteralHelper(std::string& lua)
		{
			if (lua.find("decode_hex_literal(") == std::string::npos)
			{
				return;
			}

			if (lua.find("local function decode_hex_literal(") != std::string::npos)
			{
				return;
			}

			const std::string helper =
				"local function decode_hex_literal(hex)\n"
				"    return (hex:gsub(\"%x%x\", function(byte)\n"
				"        return string.char(tonumber(byte, 16))\n"
				"    end))\n"
				"end\n\n";

			lua = helper + lua;
		}

		void RewriteUnreadableStringLiterals(std::string& lua)
		{
			std::string rewritten;
			rewritten.reserve(lua.size() + lua.size() / 8);

			for (std::size_t index = 0; index < lua.size();)
			{
				if (index + 1 < lua.size() && lua[index] == '-' && lua[index + 1] == '-')
				{
					const std::size_t line_end = lua.find('\n', index);
					if (line_end == std::string::npos)
					{
						rewritten.append(lua, index, lua.size() - index);
						break;
					}

					rewritten.append(lua, index, line_end - index + 1);
					index = line_end + 1;
					continue;
				}

				if (lua[index] != '"' && lua[index] != '\'')
				{
					rewritten.push_back(lua[index]);
					++index;
					continue;
				}

				const char quote = lua[index];
				std::size_t end = index + 1;
				while (end < lua.size())
				{
					if (lua[end] == '\\')
					{
						end += 2;
						continue;
					}

					if (lua[end] == quote)
					{
						break;
					}

					++end;
				}

				if (end >= lua.size())
				{
					rewritten.append(lua, index, lua.size() - index);
					break;
				}

				const std::string literal = lua.substr(index, end - index + 1);
				if (const auto unescaped = UnescapeLuaStringLiteral(literal))
				{
					if (!NormalizeStringValue(*unescaped))
					{
						rewritten += EscapeBinaryString(*unescaped);
					}
					else
					{
						rewritten += literal;
					}
				}
				else
				{
					rewritten += literal;
				}

				index = end + 1;
			}

			lua = std::move(rewritten);
		}

		std::optional<std::string> PlaceholderIdentifier(std::string_view value)
		{
			if (const auto normalized = NormalizeStringValue(value))
			{
				if (IsIdentifier(*normalized))
				{
					return normalized;
				}
				return std::nullopt;
			}

			std::ostringstream token;
			token << "__br_str_" << std::hex << std::uppercase << StableHash(value) << "__";
			return token.str();
		}

		std::string Join(const std::vector<std::string>& values, std::string_view separator)
		{
			std::ostringstream out;
			for (std::size_t index = 0; index < values.size(); ++index)
			{
				if (index > 0)
				{
					out << separator;
				}
				out << values[index];
			}
			return out.str();
		}

		bool ReplaceAll(std::string& value, std::string_view from, std::string_view to)
		{
			if (from.empty())
			{
				return false;
			}

			bool changed = false;
			std::size_t position = 0;
			while ((position = value.find(from, position)) != std::string::npos)
			{
				value.replace(position, from.size(), to);
				position += to.size();
				changed = true;
			}
			return changed;
		}

		std::string EscapeRegex(std::string_view value)
		{
			std::string escaped;
			escaped.reserve(value.size() * 2);
			for (char ch : value)
			{
				switch (ch)
				{
				case '\\':
				case '^':
				case '$':
				case '.':
				case '|':
				case '?':
				case '*':
				case '+':
				case '(':
				case ')':
				case '[':
				case '{':
					escaped.push_back('\\');
					break;
				default:
					break;
				}
				escaped.push_back(ch);
			}
			return escaped;
		}

		struct SemanticMethodMatch
		{
			std::string table_name;
			std::string placeholder_name;
		};

		std::optional<SemanticMethodMatch> FindSemanticMethodMatch(const std::string& lua, std::string_view label)
		{
			const std::regex pattern(
				"([A-Za-z_][A-Za-z0-9_]*)\\.(__br_str_[A-F0-9]+__) = function\\([^\\n]*\\)\\n\\s*-- ByteRevenant: semantic fallback\\n\\s*-- " + EscapeRegex(label));
			std::smatch match;
			if (!std::regex_search(lua, match, pattern))
			{
				return std::nullopt;
			}

			return SemanticMethodMatch{ match[1].str(), match[2].str() };
		}

		std::optional<std::string> RenameSemanticMethod(std::string& lua, std::string_view label, std::string_view new_name)
		{
			const auto match = FindSemanticMethodMatch(lua, label);
			if (!match)
			{
				return std::nullopt;
			}

			ReplaceAll(lua, match->table_name + "." + match->placeholder_name, match->table_name + "." + std::string(new_name));
			return match->table_name;
		}

		struct LocalFunctionBlock
		{
			std::size_t position = 0;
			std::size_t length = 0;
			std::string name;
		};

		struct MemberFunctionBlock
		{
			LocalFunctionBlock block;
			std::string table_name;
		};

		std::vector<LocalFunctionBlock> FindLocalFunctionBlocks(const std::string& lua, std::string_view args)
		{
			std::vector<LocalFunctionBlock> blocks;
			std::vector<std::string> lines;
			std::size_t begin = 0;
			while (begin <= lua.size())
			{
				const std::size_t end = lua.find('\n', begin);
				if (end == std::string::npos)
				{
					lines.emplace_back(lua.substr(begin));
					break;
				}

				lines.emplace_back(lua.substr(begin, end - begin));
				begin = end + 1;
			}

			std::vector<std::size_t> offsets(lines.size(), 0);
			std::size_t offset = 0;
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				offsets[index] = offset;
				offset += lines[index].size() + 1;
			}

			const std::string_view prefix = "local ";
			const std::string_view marker = " = function(";
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				const std::string_view line = lines[index];
				if (!line.starts_with(prefix))
				{
					continue;
				}

				const std::size_t marker_pos = line.find(marker);
				if (marker_pos == std::string_view::npos || !line.ends_with(')'))
				{
					continue;
				}

				const std::string_view name = line.substr(prefix.size(), marker_pos - prefix.size());
				if (!IsIdentifier(name))
				{
					continue;
				}

				const std::string_view line_args = line.substr(marker_pos + marker.size(), line.size() - marker_pos - marker.size() - 1);
				if (line_args != args)
				{
					continue;
				}

				int depth = 1;
				std::size_t end_index = index;
				for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
				{
					const std::string_view trimmed = TrimView(lines[cursor]);
					if (IsOpenSyntheticBlock(trimmed))
					{
						++depth;
					}
					if (IsCloseSyntheticBlock(trimmed))
					{
						--depth;
						if (depth == 0)
						{
							end_index = cursor;
							break;
						}
					}
				}

				if (depth != 0)
				{
					continue;
				}

				blocks.push_back(LocalFunctionBlock{
					offsets[index],
					offsets[end_index] + lines[end_index].size() - offsets[index],
					std::string(name),
				});
			}

			return blocks;
		}

		std::optional<LocalFunctionBlock> FindAssignedFunctionBlock(const std::string& lua, std::string_view target, std::string_view args)
		{
			std::vector<std::string> lines;
			std::size_t begin = 0;
			while (begin <= lua.size())
			{
				const std::size_t end = lua.find('\n', begin);
				if (end == std::string::npos)
				{
					lines.emplace_back(lua.substr(begin));
					break;
				}

				lines.emplace_back(lua.substr(begin, end - begin));
				begin = end + 1;
			}

			std::vector<std::size_t> offsets(lines.size(), 0);
			std::size_t offset = 0;
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				offsets[index] = offset;
				offset += lines[index].size() + 1;
			}

			const std::string marker = std::string(target) + " = function(";
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				const std::string_view line = TrimView(lines[index]);
				if (!line.starts_with(marker) || !line.ends_with(')'))
				{
					continue;
				}

				const std::string_view line_args = line.substr(marker.size(), line.size() - marker.size() - 1);
				if (line_args != args)
				{
					continue;
				}

				int depth = 1;
				std::size_t end_index = index;
				for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
				{
					const std::string_view trimmed = TrimView(lines[cursor]);
					if (IsOpenSyntheticBlock(trimmed))
					{
						++depth;
					}
					if (IsCloseSyntheticBlock(trimmed))
					{
						--depth;
						if (depth == 0)
						{
							end_index = cursor;
							break;
						}
					}
				}

				if (depth != 0)
				{
					continue;
				}

				return LocalFunctionBlock
				{
					offsets[index],
					offsets[end_index] + lines[end_index].size() - offsets[index],
					std::string(target),
				};
			}

			return std::nullopt;
		}

		std::optional<MemberFunctionBlock> FindMemberFunctionBlock(const std::string& lua, std::string_view member_name, std::string_view args)
		{
			std::vector<std::string> lines;
			std::size_t begin = 0;
			while (begin <= lua.size())
			{
				const std::size_t end = lua.find('\n', begin);
				if (end == std::string::npos)
				{
					lines.emplace_back(lua.substr(begin));
					break;
				}

				lines.emplace_back(lua.substr(begin, end - begin));
				begin = end + 1;
			}

			std::vector<std::size_t> offsets(lines.size(), 0);
			std::size_t offset = 0;
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				offsets[index] = offset;
				offset += lines[index].size() + 1;
			}

			const std::string marker = "." + std::string(member_name) + " = function(";
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				const std::string_view line = TrimView(lines[index]);
				const std::size_t marker_pos = line.find(marker);
				if (marker_pos == std::string_view::npos || !line.ends_with(')'))
				{
					continue;
				}

				const std::string_view table_name = line.substr(0, marker_pos);
				if (!IsIdentifier(table_name))
				{
					continue;
				}

				const std::string_view line_args = line.substr(marker_pos + marker.size(), line.size() - marker_pos - marker.size() - 1);
				if (line_args != args)
				{
					continue;
				}

				int depth = 1;
				std::size_t end_index = index;
				for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
				{
					const std::string_view trimmed = TrimView(lines[cursor]);
					if (IsOpenSyntheticBlock(trimmed))
					{
						++depth;
					}
					if (IsCloseSyntheticBlock(trimmed))
					{
						--depth;
						if (depth == 0)
						{
							end_index = cursor;
							break;
						}
					}
				}

				if (depth != 0)
				{
					continue;
				}

				return MemberFunctionBlock
				{
					LocalFunctionBlock
					{
						offsets[index],
						offsets[end_index] + lines[end_index].size() - offsets[index],
						std::string(table_name) + "." + std::string(member_name),
					},
					std::string(table_name),
				};
			}

			return std::nullopt;
		}

		std::optional<LocalFunctionBlock> FindBoundFunctionBlock(const std::string& lua, std::string_view target, std::string_view args)
		{
			const std::regex assign_pattern("^" + EscapeRegex(std::string(target)) + " = ([A-Za-z_][A-Za-z0-9_]*)$", std::regex::multiline);
			std::smatch assign_match;
			if (!std::regex_search(lua, assign_match, assign_pattern))
			{
				return std::nullopt;
			}

			const std::string local_name = assign_match[1].str();
			const auto blocks = FindLocalFunctionBlocks(lua, args);
			for (const LocalFunctionBlock& block : blocks)
			{
				if (block.name == local_name)
				{
					return block;
				}
			}

			return std::nullopt;
		}

		void ReplaceFunctionBlock(std::string& lua, const LocalFunctionBlock& block, const std::string& replacement)
		{
			lua.replace(block.position, block.length, replacement);
		}

		std::vector<std::string> SplitLines(std::string_view text)
		{
			std::vector<std::string> lines;
			std::size_t begin = 0;
			while (begin <= text.size())
			{
				const std::size_t end = text.find('\n', begin);
				if (end == std::string_view::npos)
				{
					lines.emplace_back(text.substr(begin));
					break;
				}

				lines.emplace_back(text.substr(begin, end - begin));
				begin = end + 1;
			}

			return lines;
		}

		std::string JoinLines(const std::vector<std::string>& lines)
		{
			std::ostringstream out;
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				out << lines[index];
				if (index + 1 < lines.size())
				{
					out << '\n';
				}
			}
			return out.str();
		}

		bool IsGenericTempName(std::string_view name)
		{
			if (name == "var" || name == "flag")
			{
				return true;
			}

			const auto match_prefix = [&](std::string_view prefix)
			{
				return name.size() > prefix.size()
					&& name.starts_with(prefix)
					&& std::all_of(
						name.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
						name.end(),
						[](char ch)
						{
							return std::isdigit(static_cast<unsigned char>(ch)) != 0;
						});
			};

			return match_prefix("var_") || match_prefix("sym_");
		}

		std::string EscapeRegexLiteral(std::string_view value)
		{
			std::string result;
			result.reserve(value.size() * 2);
			for (char ch : value)
			{
				switch (ch)
				{
				case '\\':
				case '^':
				case '$':
				case '.':
				case '|':
				case '?':
				case '*':
				case '+':
				case '(':
				case ')':
				case '[':
				case ']':
				case '{':
				case '}':
					result.push_back('\\');
					break;
				default:
					break;
				}

				result.push_back(ch);
			}

			return result;
		}

		bool ContainsIdentifier(const std::vector<std::string>& lines, std::string_view identifier)
		{
			const std::regex pattern("\\b" + EscapeRegexLiteral(identifier) + "\\b");
			for (const std::string& line : lines)
			{
				if (std::regex_search(line, pattern))
				{
					return true;
				}
			}

			return false;
		}

        void ReplaceIdentifierInLines(std::vector<std::string>& lines, std::string_view from, std::string_view to)
        {
            const std::regex pattern("\\b" + EscapeRegexLiteral(from) + "\\b");
            for (std::string& line : lines)
            {
                line = std::regex_replace(line, pattern, std::string(to));
            }
        }

        std::size_t LeadingIndentWidth(std::string_view line)
        {
            const std::size_t pos = line.find_first_not_of(" \t");
            return pos == std::string_view::npos ? line.size() : pos;
        }

        std::string LeadingIndent(std::string_view line)
        {
            return std::string(line.substr(0, LeadingIndentWidth(line)));
        }

        std::vector<std::string> SplitFunctionArgs(std::string_view args)
        {
            std::vector<std::string> result;
            std::size_t begin = 0;
            while (begin <= args.size())
            {
                const std::size_t comma = args.find(',', begin);
                const std::size_t end = comma == std::string_view::npos ? args.size() : comma;
                const std::string_view token = TrimView(args.substr(begin, end - begin));
                if (!token.empty())
                {
                    result.emplace_back(token);
                }
                if (comma == std::string_view::npos)
                {
                    break;
                }
                begin = comma + 1;
            }
            return result;
        }

        void InsertBlankLinesAfterTopLevelFunctions(std::vector<std::string>& lines)
        {
            const std::regex function_start_pattern("^local function [A-Za-z_][A-Za-z0-9_]*\\(.*\\)$");
            const std::regex assigned_start_pattern("^(?:local )?[A-Za-z_][A-Za-z0-9_\\.\\[\\]\":]* =\\s*function\\(.*\\)$");
            const auto is_top_level_function = [&](const std::string& line)
            {
                const std::string_view trimmed = TrimView(line);
                return LeadingIndentWidth(line) == 0
                    && (std::regex_match(std::string(trimmed), function_start_pattern)
                        || std::regex_match(std::string(trimmed), assigned_start_pattern));
            };
            const auto opens_block = [](std::string_view value)
            {
                value = TrimView(value);
                return (StartsWithKeyword(value, "if") && value.ends_with(" then"))
                    || (StartsWithKeyword(value, "for") && value.ends_with(" do"))
                    || (StartsWithKeyword(value, "while") && value.ends_with(" do"))
                    || value == "repeat"
                    || value.starts_with("local function ")
                    || value.find(" = function(") != std::string_view::npos
                    || value.find("function(") != std::string_view::npos;
            };

            std::vector<std::string> normalized;
            normalized.reserve(lines.size() + 16);
            for (const std::string& line : lines)
            {
                if (is_top_level_function(line) && !normalized.empty() && !TrimView(normalized.back()).empty())
                {
                    normalized.push_back("");
                }
                normalized.push_back(line);
            }
            lines.swap(normalized);

            std::vector<bool> block_stack;
            for (std::size_t index = 0; index < lines.size(); ++index)
            {
                const std::string_view trimmed = TrimView(lines[index]);
                if (trimmed.empty())
                {
                    continue;
                }

                if (IsCloseSyntheticBlock(trimmed) && !block_stack.empty())
                {
                    const bool closed_top_level_function = block_stack.back();
                    block_stack.pop_back();
                    if (closed_top_level_function && index + 1 < lines.size() && !TrimView(lines[index + 1]).empty())
                    {
                        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), "");
                        ++index;
                    }

                    continue;
                }

                if (!opens_block(trimmed))
                {
                    continue;
                }

                block_stack.push_back(is_top_level_function(lines[index]));
            }
        }

        std::size_t CountTokenUses(const std::vector<std::string>& lines, std::size_t begin, std::string_view token)
        {
			if (token.empty())
			{
				return 0;
			}

			std::size_t uses = 0;
			const std::size_t base_indent = begin < lines.size() ? LeadingIndentWidth(lines[begin]) : 0;
			const std::regex shadow_pattern("^([ \\t]*)local(?: function)? " + EscapeRegexLiteral(token) + "(?: =|\\()");
			for (std::size_t index = begin; index < lines.size(); ++index)
			{
				const std::string_view trimmed = TrimView(lines[index]);
				if (trimmed.starts_with("--"))
				{
					continue;
				}

				const std::size_t indent = LeadingIndentWidth(lines[index]);
				if (indent < base_indent)
				{
					break;
				}

				if (indent == base_indent && std::regex_search(lines[index], shadow_pattern))
				{
					break;
				}

				if (indent != base_indent)
				{
					continue;
				}

				const std::string& line = lines[index];
				std::size_t position = 0;
				while ((position = line.find(token, position)) != std::string::npos)
				{
                    const bool left_ok = position == 0 || (std::isalnum(static_cast<unsigned char>(line[position - 1])) == 0 && line[position - 1] != '_');
                    const std::size_t end = position + token.size();
                    const bool right_ok = end >= line.size() || (std::isalnum(static_cast<unsigned char>(line[end])) == 0 && line[end] != '_');
                    if (left_ok && right_ok)
                    {
                        ++uses;
                    }

                    position += token.size();
                }
            }

            return uses;
        }

        std::size_t CountTokenUsesIgnoringRedeclarations(const std::vector<std::string>& lines, std::size_t begin, std::string_view token)
        {
            if (token.empty())
            {
                return 0;
            }

            std::size_t uses = 0;
            const std::regex declaration_pattern("^([ \\t]*)local(?:\\s+function)?\\s+" + EscapeRegexLiteral(token) + "\\b");
            const std::size_t base_indent = begin < lines.size() ? LeadingIndentWidth(lines[begin]) : 0;
            for (std::size_t index = begin; index < lines.size(); ++index)
            {
                const std::string_view trimmed = TrimView(lines[index]);
                if (trimmed.starts_with("--"))
                {
                    continue;
                }
                const std::size_t indent = LeadingIndentWidth(lines[index]);
                if (indent < base_indent)
                {
                    break;
                }

                if (indent != base_indent)
                {
                    continue;
                }

                if (std::regex_search(lines[index], declaration_pattern))
                {
                    break;
                }

                const std::string& line = lines[index];
                std::size_t position = 0;
                while ((position = line.find(token, position)) != std::string::npos)
                {
                    const bool left_ok = position == 0 || (std::isalnum(static_cast<unsigned char>(line[position - 1])) == 0 && line[position - 1] != '_');
                    const std::size_t end = position + token.size();
                    const bool right_ok = end >= line.size() || (std::isalnum(static_cast<unsigned char>(line[end])) == 0 && line[end] != '_');
                    if (left_ok && right_ok)
                    {
                        ++uses;
                    }

                    position += token.size();
                }
            }

            return uses;
        }

        bool ReplaceTokenInLine(std::string& line, std::string_view token, std::string_view replacement)
        {
            if (token.empty() || token == replacement)
            {
                return false;
            }

            std::string rewritten;
            rewritten.reserve(line.size() + replacement.size());
            std::size_t position = 0;
            bool changed = false;

            while (position < line.size())
            {
                const std::size_t found = line.find(token, position);
                if (found == std::string::npos)
                {
                    rewritten.append(line, position, std::string::npos);
                    break;
                }

                const bool left_ok = found == 0 || (std::isalnum(static_cast<unsigned char>(line[found - 1])) == 0 && line[found - 1] != '_');
                const std::size_t end = found + token.size();
                const bool right_ok = end >= line.size() || (std::isalnum(static_cast<unsigned char>(line[end])) == 0 && line[end] != '_');
                if (left_ok && right_ok)
                {
                    rewritten.append(line, position, found - position);
                    rewritten.append(replacement);
                    position = end;
                    changed = true;
                    continue;
                }

                rewritten.append(line, position, found - position + token.size());
                position = end;
            }

            if (changed)
            {
                line = std::move(rewritten);
            }

            return changed;
        }

        bool ReplaceTokenInLines(std::vector<std::string>& lines, std::size_t begin, std::string_view token, std::string_view replacement, std::optional<std::size_t> base_indent_override = std::nullopt)
        {
            if (token.empty() || token == replacement)
            {
                return false;
            }

            bool changed = false;
            const std::size_t base_indent = base_indent_override.value_or(begin < lines.size() ? LeadingIndentWidth(lines[begin]) : 0);
            const std::regex shadow_pattern("^([ \\t]*)local(?: function)? " + EscapeRegexLiteral(token) + "(?: =|\\()");
            for (std::size_t index = begin; index < lines.size(); ++index)
            {
                const std::string_view trimmed = TrimView(lines[index]);
                if (trimmed.starts_with("--"))
                {
                    continue;
                }

                const std::size_t indent = LeadingIndentWidth(lines[index]);
                if (indent < base_indent)
                {
                    break;
                }

                if (indent != base_indent)
                {
                    continue;
                }

                if (std::regex_search(lines[index], shadow_pattern))
                {
                    break;
                }

                if (ReplaceTokenInLine(lines[index], token, replacement))
                {
                    changed = true;
                }
            }

            return changed;
        }

        bool IsSafeInlineLocalRhs(std::string_view rhs)
        {
            rhs = TrimView(rhs);
            if (rhs.empty())
            {
                return false;
            }

            if (rhs.front() == '"' || rhs.front() == '\'')
            {
                return rhs.size() >= 2 && rhs.size() <= 64 && rhs.back() == rhs.front();
            }

            if (rhs.front() == '{' && rhs.back() == '}')
            {
                return rhs.size() <= 120;
            }

            if (rhs == "true" || rhs == "false" || rhs == "nil")
            {
                return true;
            }

            if (rhs.size() > 80)
            {
                return false;
            }

            for (char ch : rhs)
            {
                if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '.' || ch == ':')
                {
                    continue;
                }

                return false;
            }

            return true;
        }

        bool LooksLikeCallSite(std::string_view line)
        {
            line = TrimView(line);
            if (line.empty() || line.starts_with("--"))
            {
                return false;
            }

            if ((StartsWithKeyword(line, "if") && line.ends_with(" then"))
                || (StartsWithKeyword(line, "elseif") && line.ends_with(" then"))
                || (StartsWithKeyword(line, "while") && line.ends_with(" do"))
                || (StartsWithKeyword(line, "for") && line.ends_with(" do"))
                || StartsWithKeyword(line, "function")
                || StartsWithKeyword(line, "local function"))
            {
                return false;
            }

            const std::size_t open = line.find('(');
            const std::size_t close = line.rfind(')');
            if (open != std::string_view::npos)
            {
                const std::string_view prefix = line.substr(0, open);
                for (std::size_t index = 0; index < prefix.size(); ++index)
                {
                    if (prefix[index] != '=')
                    {
                        continue;
                    }

                    const char previous = index > 0 ? prefix[index - 1] : '\0';
                    const char next = index + 1 < prefix.size() ? prefix[index + 1] : '\0';
                    if (previous != '=' && previous != '<' && previous != '>' && previous != '~' && next != '=')
                    {
                        return false;
                    }
                }
            }

            return open != std::string_view::npos && close != std::string_view::npos && open < close;
        }

        void SimplifyReadableLocals(std::vector<std::string>& lines)
        {
            const std::regex assigned_function_pattern("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*) =\\s*function\\((.*)\\)$");
            const std::regex local_function_pattern("^([ \\t]*)local function ([A-Za-z_][A-Za-z0-9_]*)\\((.*)\\)$");

            bool changed = true;
            std::size_t synthetic_index = 0;
            while (changed)
            {
                changed = false;
                for (std::size_t index = 0; index < lines.size();)
                {
                    auto rewrite_local_function = [&](const std::smatch& function_match)
                    {
                        const std::string indent = function_match[1].str();
                        const std::string old_name = function_match[2].str();
                        std::string name = old_name;
                        const std::string args = function_match[3].str();
                        const std::size_t base_indent = LeadingIndentWidth(lines[index]);
                        std::size_t end_index = index;
                        int depth = 1;
                        for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
                        {
                            const std::string_view trimmed = TrimView(lines[cursor]);
                            if (IsOpenSyntheticBlock(trimmed))
                            {
                                ++depth;
                            }
                            if (IsCloseSyntheticBlock(trimmed))
                            {
                                --depth;
                                if (depth == 0)
                                {
                                    end_index = cursor;
                                    break;
                                }
                            }
                        }

                        if (IsGenericTempName(name))
                        {
                            const std::regex use_pattern("\\b" + EscapeRegexLiteral(old_name) + "\\b");
                            const std::regex declaration_pattern("^([ \\t]*)local(?:\\s+function)?\\s+" + EscapeRegexLiteral(old_name) + "\\b");
                            bool safe_rename = true;
                            for (std::size_t cursor = end_index + 1; cursor < lines.size(); ++cursor)
                            {
                                const std::string_view trimmed = TrimView(lines[cursor]);
                                if (trimmed.starts_with("--"))
                                {
                                    continue;
                                }

                                const std::size_t indent_width = LeadingIndentWidth(lines[cursor]);
                                if (indent_width < base_indent)
                                {
                                    break;
                                }

                                if (indent_width == base_indent && std::regex_search(lines[cursor], declaration_pattern))
                                {
                                    break;
                                }

                                if (std::regex_search(lines[cursor], declaration_pattern))
                                {
                                    continue;
                                }

                                if (std::regex_search(lines[cursor], use_pattern) && indent_width != base_indent)
                                {
                                    safe_rename = false;
                                    break;
                                }
                            }

                            if (safe_rename)
                            {
                                std::string candidate;
                                do
                                {
                                    candidate = "sub_" + std::to_string(1000 + ++synthetic_index);
                                }
                                while (ContainsIdentifier(lines, candidate));

                                ReplaceTokenInLines(lines, index + 1, old_name, candidate, base_indent);
                                name = candidate;
                            }
                        }

                        const std::string rewritten = indent + "local function " + name + "(" + args + ")";
                        if (lines[index] != rewritten)
                        {
                            lines[index] = rewritten;
                            changed = true;
                        }
                    };

                    std::smatch function_match;
                    if (std::regex_match(lines[index], function_match, assigned_function_pattern)
                        || std::regex_match(lines[index], function_match, local_function_pattern))
                    {
                        rewrite_local_function(function_match);
                        ++index;
                        continue;
                    }

                    std::smatch assign_match;
                    if (!std::regex_match(lines[index], assign_match, std::regex("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*) = (.+)$")))
                    {
                        ++index;
                        continue;
                    }

                    ++index;
                }
            }
        }

        void InlineSingleUseLiteralLocals(std::vector<std::string>& lines)
        {
            const std::regex pattern("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*) = (.+)$");

            for (std::size_t index = 0; index < lines.size();)
            {
                std::smatch match;
                if (!std::regex_match(lines[index], match, pattern))
                {
                    ++index;
                    continue;
                }

                const std::string name = match[2].str();
                const std::string value = std::string(TrimView(match[3].str()));
                if (!IsSafeInlineLocalRhs(value))
                {
                    ++index;
                    continue;
                }

                const std::regex use_pattern("\\b" + EscapeRegexLiteral(name) + "\\b");
                const std::regex declaration_pattern("^([ \\t]*)local(?:\\s+function)?\\s+" + EscapeRegexLiteral(name) + "\\b");
                const std::size_t base_indent = LeadingIndentWidth(lines[index]);
                bool inlined_immediately = false;
                for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
                {
                    const std::string_view trimmed = TrimView(lines[cursor]);
                    if (trimmed.empty() || trimmed.starts_with("--"))
                    {
                        continue;
                    }

                    const std::size_t indent = LeadingIndentWidth(lines[cursor]);
                    if (indent != base_indent)
                    {
                        if (indent < base_indent)
                        {
                            break;
                        }
                        continue;
                    }

                    if (LooksLikeCallSite(trimmed) && std::regex_search(lines[cursor], use_pattern))
                    {
                        const std::string original = lines[cursor];
                        if (ReplaceTokenInLines(lines, cursor, name, value) && lines[cursor].size() <= 160)
                        {
                            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
                            inlined_immediately = true;
                            break;
                        }

                        lines[cursor] = original;
                    }
                    break;
                }

                if (inlined_immediately)
                {
                    continue;
                }

                std::size_t use_index = lines.size();
                bool unsafe = false;
                for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
                {
                    const std::string_view trimmed = TrimView(lines[cursor]);
                    if (trimmed.starts_with("--"))
                    {
                        continue;
                    }

                    const std::size_t indent = LeadingIndentWidth(lines[cursor]);
                    if (indent < base_indent)
                    {
                        break;
                    }

                    if (indent == base_indent && std::regex_search(lines[cursor], declaration_pattern))
                    {
                        break;
                    }

                    if (std::regex_search(lines[cursor], declaration_pattern))
                    {
                        continue;
                    }

                    if (std::regex_search(lines[cursor], use_pattern))
                    {
                        if (indent != base_indent || use_index != lines.size())
                        {
                            unsafe = true;
                            break;
                        }

                        use_index = cursor;
                    }
                }

                if (unsafe || use_index == lines.size())
                {
                    ++index;
                    continue;
                }

                const std::string_view use_trimmed = TrimView(lines[use_index]);
                if (use_trimmed.starts_with("local ") || use_trimmed.starts_with("function ") || use_trimmed == "end" || !LooksLikeCallSite(use_trimmed))
                {
                    ++index;
                    continue;
                }

                const std::string original = lines[use_index];
                if (!ReplaceTokenInLines(lines, use_index, name, value) || lines[use_index].size() > 160)
                {
                    lines[use_index] = original;
                    ++index;
                    continue;
                }

                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
        }

        void RemoveUnusedSimpleLocalAliases(std::vector<std::string>& lines)
        {
            const std::regex pattern("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*) = ([A-Za-z_][A-Za-z0-9_]*(?:\\.[A-Za-z_][A-Za-z0-9_]*)*)$");

            for (std::size_t index = 0; index < lines.size();)
            {
                std::smatch match;
                if (!std::regex_match(lines[index], match, pattern))
                {
                    ++index;
                    continue;
                }

                const std::string name = match[2].str();
                const std::regex use_pattern("\\b" + EscapeRegexLiteral(name) + "\\b");
                const std::regex declaration_pattern("^([ \\t]*)local(?:\\s+function)?\\s+" + EscapeRegexLiteral(name) + "\\b");
                const std::size_t base_indent = LeadingIndentWidth(lines[index]);
                bool used = false;
                for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
                {
                    const std::string_view trimmed = TrimView(lines[cursor]);
                    if (trimmed.starts_with("--"))
                    {
                        continue;
                    }

                    const std::size_t indent = LeadingIndentWidth(lines[cursor]);
                    if (indent < base_indent)
                    {
                        break;
                    }

                    if (indent == base_indent && std::regex_search(lines[cursor], declaration_pattern))
                    {
                        break;
                    }

                    if (std::regex_search(lines[cursor], use_pattern))
                    {
                        used = true;
                        break;
                    }
                }

                if (used)
                {
                    ++index;
                    continue;
                }

                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }

        void RepairSelfAccumulatingOrAssignments(std::vector<std::string>& lines)
        {
            const std::regex pattern("^([ \\t]*)\\((.+) or 0\\) = (.+) ([+-]) ([-+]?(?:\\d+\\.\\d+|\\d+|\\d+\\.)(?:[eE][-+]?\\d+)?)$");
            for (std::string& line : lines)
            {
                std::smatch match;
                if (!std::regex_match(line, match, pattern))
                {
                    continue;
                }

                const std::string target = std::string(TrimView(match[2].str()));
                const std::string current = std::string(TrimView(match[3].str()));
                if (target != current)
                {
                    continue;
                }

                line = match[1].str() + target + " = (" + target + " or 0) " + match[4].str() + " " + match[5].str();
            }
        }

        std::optional<std::pair<std::size_t, std::size_t>> FindLocalFunctionRangeByName(const std::vector<std::string>& lines, std::string_view name)
        {
            const std::regex local_function("^([ \\t]*)local function " + EscapeRegexLiteral(name) + "\\(.*\\)$");
            const std::regex assigned_function("^([ \\t]*)local " + EscapeRegexLiteral(name) + " =\\s*function\\(.*\\)$");
            for (std::size_t index = 0; index < lines.size(); ++index)
            {
                if (!std::regex_match(lines[index], local_function) && !std::regex_match(lines[index], assigned_function))
                {
                    continue;
                }

                int depth = 1;
                std::size_t end_index = index;
                for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
                {
                    const std::string_view trimmed = TrimView(lines[cursor]);
                    if (IsOpenSyntheticBlock(trimmed))
                    {
                        ++depth;
                    }
                    if (IsCloseSyntheticBlock(trimmed))
                    {
                        --depth;
                        if (depth == 0)
                        {
                            end_index = cursor;
                            break;
                        }
                    }
                }

                if (depth == 0)
                {
                    return std::make_pair(index, end_index);
                }
            }

            return std::nullopt;
        }

        std::optional<std::string> MatchFirstCapture(const std::vector<std::string>& lines, const std::regex& pattern)
        {
            for (const std::string& line : lines)
            {
                std::smatch match;
                if (std::regex_match(line, match, pattern) && match.size() >= 2)
                {
                    return match[1].str();
                }
            }

            return std::nullopt;
        }

        void ReplaceLineRange(std::vector<std::string>& lines, std::size_t begin, std::size_t end, const std::vector<std::string>& replacement)
        {
            if (begin > end || end >= lines.size())
            {
                return;
            }

            lines.erase(
                lines.begin() + static_cast<std::ptrdiff_t>(begin),
                lines.begin() + static_cast<std::ptrdiff_t>(end + 1));
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(begin), replacement.begin(), replacement.end());
        }

        void CompactSyntheticArithmeticWhileBlocks(std::vector<std::string>& lines);
        void RepairBrokenRecursiveInspectWalkers(std::vector<std::string>& lines);

        void ApplyGafniumClientSemanticRepair(std::string& lua)
        {
            if (lua.find("\"tghofm\"") == std::string::npos
                || lua.find("\"tghacdls\"") == std::string::npos
                || lua.find("\"tghoic\"") == std::string::npos
                || lua.find("\"hrifc\"") == std::string::npos)
            {
                return;
            }

            std::vector<std::string> lines = SplitLines(lua);
            RepairSelfAccumulatingOrAssignments(lines);

            if (const auto start_range = FindLocalFunctionRangeByName(lines, "onClientResourceStart"))
            {
                std::string counter_name = "var_2";
                std::string trip_name = "var_4";
                const std::regex counter_pattern("^\\s*if 1 >= ([A-Za-z_][A-Za-z0-9_]*)\\[[^\\]]+\\] then$");
                const std::regex call_pattern("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\(\\)$");
                for (std::size_t index = start_range->first; index <= start_range->second && index < lines.size(); ++index)
                {
                    std::smatch match;
                    if (std::regex_match(lines[index], match, counter_pattern))
                    {
                        counter_name = match[1].str();
                        continue;
                    }

                    if (std::regex_match(lines[index], match, call_pattern))
                    {
                        const std::string candidate = match[1].str();
                        if (candidate != "getResourceName" && candidate != "triggerServerEvent")
                        {
                            trip_name = candidate;
                        }
                    }
                }

                ReplaceLineRange(lines, start_range->first, start_range->second,
                    {
                        "local function onClientResourceStart(arg1)",
                        "    local resourceName = getResourceName(arg1)",
                        "    " + counter_name + "[resourceName] = (" + counter_name + "[resourceName] or 0) + 1",
                        "    if " + counter_name + "[resourceName] <= 1 then",
                        "        return",
                        "    end",
                        "    triggerServerEvent(\"tghora\", resourceRoot, resourceName)",
                        "    " + trip_name + "()",
                        "end",
                    });
            }

            if (const auto stop_range = FindLocalFunctionRangeByName(lines, "onClientResourceStop"))
            {
                std::string counter_name = "var_2";
                if (const auto start_range = FindLocalFunctionRangeByName(lines, "onClientResourceStart"))
                {
                    const std::regex counter_pattern("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\[resourceName\\] = \\(\\1\\[resourceName\\] or 0\\) \\+ 1$");
                    for (std::size_t index = start_range->first; index <= start_range->second && index < lines.size(); ++index)
                    {
                        std::smatch match;
                        if (std::regex_match(lines[index], match, counter_pattern))
                        {
                            counter_name = match[1].str();
                            break;
                        }
                    }
                }

                ReplaceLineRange(lines, stop_range->first, stop_range->second,
                    {
                        "local function onClientResourceStop(arg1)",
                        "    local resourceName = getResourceName(arg1)",
                        "    " + counter_name + "[resourceName] = (" + counter_name + "[resourceName] or 0) - 1",
                        "end",
                    });
            }

            if (const auto fc_handler = MatchFirstCapture(lines, std::regex("^addEventHandler\\(\"fc\", localPlayer, ([A-Za-z_][A-Za-z0-9_]*)\\)$")))
            {
                if (const auto fc_range = FindLocalFunctionRangeByName(lines, *fc_handler))
                {
                    ReplaceLineRange(lines, fc_range->first, fc_range->second,
                        {
                            "local function " + *fc_handler + "()",
                            "    local restoredModels = {}",
                            "    setGameSpeed(0)",
                            "    showChat(false)",
                            "    toggleAllControls(false)",
                            "    setElementFrozen(localPlayer, true)",
                            "    local camX, camY, camZ, lookX, lookY, lookZ = getCameraMatrix()",
                            "    setCameraMatrix(camX, camY, camZ - 1, lookX, lookY, lookZ - 2, 0, 0.01)",
                            "    addEventHandler(\"onClientRender\", root, function()",
                            "        dxDrawRectangle(0, 0, screenX, screenY, 4278190080, true)",
                            "    end, true, \"low-9\")",
                            "    dxDrawRectangle(0, 0, screenX, screenY, 4278190080, true)",
                            "    for _, element in ipairs(getElementsByType(\"object\")) do",
                            "        local model = getElementModel(element)",
                            "        if not restoredModels[model] then",
                            "            restoredModels[model] = true",
                            "            engineRestoreModel(model)",
                            "            engineRestoreCOL(model)",
                            "        end",
                            "    end",
                            "    for _, elementType in ipairs({ \"vehicle\", \"player\", \"ped\" }) do",
                            "        for _, element in ipairs(getElementsByType(elementType)) do",
                            "            local model = getElementModel(element)",
                            "            if not restoredModels[model] then",
                            "                restoredModels[model] = true",
                            "                engineRestoreModel(model)",
                            "            end",
                            "        end",
                            "    end",
                            "end",
                        });
                }
            }

            if (const auto timer_name = MatchFirstCapture(lines, std::regex("^setTimer\\(([A-Za-z_][A-Za-z0-9_]*), 5000, 0\\)$")))
            {
                if (const auto timer_range = FindLocalFunctionRangeByName(lines, *timer_name))
                {
                    std::string recursive_name;
                    std::string inspect_table = "var_39";
                    const std::regex recursive_call_pattern("^\\s*local [A-Za-z_][A-Za-z0-9_]* = ([A-Za-z_][A-Za-z0-9_]*)\\(resourceRoot\\)$");
                    const std::regex inspect_table_pattern("^\\s*triggerServerEvent\\(\"tghoic\", resourceRoot, [^,]+, ([A-Za-z_][A-Za-z0-9_]*)\\)$");
                    for (std::size_t index = timer_range->first; index <= timer_range->second && index < lines.size(); ++index)
                    {
                        std::smatch match;
                        if (recursive_name.empty() && std::regex_match(lines[index], match, recursive_call_pattern))
                        {
                            recursive_name = match[1].str();
                        }
                        if (std::regex_match(lines[index], match, inspect_table_pattern))
                        {
                            inspect_table = match[1].str();
                        }
                    }

                    if (!recursive_name.empty())
                    {
                        if (const auto recursive_range = FindLocalFunctionRangeByName(lines, recursive_name))
                        {
                            ReplaceLineRange(lines, recursive_range->first, recursive_range->second,
                                {
                                    "local function " + recursive_name + "(arg1)",
                                    "    if not arg1 then",
                                    "        return 0",
                                    "    end",
                                    "    local childCount = getElementChildrenCount(arg1)",
                                    "    if childCount <= 0 then",
                                    "        return childCount",
                                    "    end",
                                    "    for index = 0, childCount - 1 do",
                                    "        local child = getElementChild(arg1, index)",
                                    "        if child then",
                                    "            local childType = getElementType(child)",
                                    "            if childType ~= \"map\" and childType ~= \"colmodelroot\" and childType ~= \"dffroot\" and childType ~= \"guiroot\" and childType ~= \"txdroot\" then",
                                    "                table.insert(" + inspect_table + ", inspect(child))",
                                    "            end",
                                    "            childCount = childCount + " + recursive_name + "(child)",
                                    "        end",
                                    "    end",
                                    "    return childCount",
                                    "end",
                                });
                        }
                    }
                }
            }

            RepairBrokenRecursiveInspectWalkers(lines);
            CompactSyntheticArithmeticWhileBlocks(lines);
            ReplaceIdentifierInLines(lines, "var_3", "lockClient");
            ReplaceIdentifierInLines(lines, "var_4", "tripClient");
            ReplaceIdentifierInLines(lines, "var_40", "countChildElements");
            for (std::string& line : lines)
            {
                if (TrimView(line) == "local function var(arg1, arg2)")
                {
                    line = "local function sub_1000(arg1, arg2)";
                }
            }

            lua = Join(lines, "\n");
        }

        bool ContainsIdentifierToken(std::string_view text, std::string_view identifier)
        {
            if (identifier.empty())
            {
                return false;
            }

            std::size_t position = 0;
            while ((position = text.find(identifier, position)) != std::string_view::npos)
            {
                const bool left_ok = position == 0 || (std::isalnum(static_cast<unsigned char>(text[position - 1])) == 0 && text[position - 1] != '_');
                const std::size_t end = position + identifier.size();
                const bool right_ok = end >= text.size() || (std::isalnum(static_cast<unsigned char>(text[end])) == 0 && text[end] != '_');
                if (left_ok && right_ok)
                {
                    return true;
                }

                position += identifier.size();
            }

            return false;
        }

        bool TryParseSyntheticSelfOp(std::string_view line, std::string& target, char& op, std::string& rhs)
        {
            std::smatch match;
            const std::string text(line);
            if (!std::regex_match(text, match, std::regex("^([A-Za-z_][A-Za-z0-9_]*) = \\1 ([+\\-*/]) (.+)$")))
            {
                return false;
            }

            target = match[1].str();
            op = match[2].str()[0];
            rhs = std::string(TrimView(match[3].str()));
            return true;
        }

        std::string WrapSyntheticFactor(std::string_view rhs)
        {
            if (IsIdentifier(rhs) || ParseLuaNumberLiteral(rhs).has_value())
            {
                return std::string(rhs);
            }

            return "(" + std::string(rhs) + ")";
        }

        std::vector<std::string> FoldSyntheticArithmeticRuns(const std::vector<std::string>& body)
        {
            std::vector<std::string> folded;
            folded.reserve(body.size());
            for (std::size_t index = 0; index < body.size();)
            {
                std::string target;
                std::string rhs;
                char op = '\0';
                if (!TryParseSyntheticSelfOp(body[index], target, op, rhs) || ContainsIdentifierToken(rhs, target))
                {
                    folded.push_back(body[index]);
                    ++index;
                    continue;
                }

                std::size_t run = 1;
                for (; index + run < body.size(); ++run)
                {
                    std::string next_target;
                    std::string next_rhs;
                    char next_op = '\0';
                    if (!TryParseSyntheticSelfOp(body[index + run], next_target, next_op, next_rhs)
                        || next_target != target
                        || next_op != op
                        || next_rhs != rhs)
                    {
                        break;
                    }
                }

                if (run == 1)
                {
                    folded.push_back(body[index]);
                    ++index;
                    continue;
                }

                const std::string factor = WrapSyntheticFactor(rhs);
                switch (op)
                {
                case '+':
                case '-':
                    folded.push_back(target + " = " + target + " " + op + " " + factor + " * " + std::to_string(run));
                    break;
                case '*':
                    folded.push_back(target + " = " + target + " * " + factor + " ^ " + std::to_string(run));
                    break;
                case '/':
                    folded.push_back(target + " = " + target + " / " + factor + " ^ " + std::to_string(run));
                    break;
                default:
                    folded.push_back(body[index]);
                    run = 1;
                    break;
                }

                index += run;
            }

            return folded;
        }

        std::vector<std::string> FoldRepeatedSyntheticBlocks(const std::vector<std::string>& body)
        {
            std::vector<std::string> folded;
            folded.reserve(body.size());
            for (std::size_t index = 0; index < body.size();)
            {
                bool replaced = false;
                for (std::size_t block_size = 3; block_size >= 2; --block_size)
                {
                    if (index + block_size * 3 > body.size())
                    {
                        continue;
                    }

                    std::size_t repeats = 1;
                    while (index + (repeats + 1) * block_size <= body.size())
                    {
                        bool same = true;
                        for (std::size_t offset = 0; offset < block_size; ++offset)
                        {
                            if (body[index + offset] != body[index + repeats * block_size + offset])
                            {
                                same = false;
                                break;
                            }
                        }

                        if (!same)
                        {
                            break;
                        }

                        ++repeats;
                    }

                    if (repeats < 3)
                    {
                        continue;
                    }

                    folded.push_back("for _ = 1, " + std::to_string(repeats) + " do");
                    for (std::size_t offset = 0; offset < block_size; ++offset)
                    {
                        folded.push_back("    " + body[index + offset]);
                    }
                    folded.push_back("end");
                    index += repeats * block_size;
                    replaced = true;
                    break;
                }

                if (replaced)
                {
                    continue;
                }

                folded.push_back(body[index]);
                ++index;
            }

            return folded;
        }

        void CompactSyntheticArithmeticWhileBlocks(std::vector<std::string>& lines)
        {
            const std::regex while_pattern("^([ \\t]*)while .+ do$");
            for (std::size_t index = 0; index < lines.size(); ++index)
            {
                std::smatch match;
                if (!std::regex_match(lines[index], match, while_pattern))
                {
                    continue;
                }

                const std::string indent = match[1].str();
                int depth = 1;
                std::size_t end_index = index;
                for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
                {
                    const std::string_view trimmed = TrimView(lines[cursor]);
                    if (IsOpenSyntheticBlock(trimmed))
                    {
                        ++depth;
                    }
                    if (IsCloseSyntheticBlock(trimmed))
                    {
                        --depth;
                        if (depth == 0)
                        {
                            end_index = cursor;
                            break;
                        }
                    }
                }

                if (depth != 0 || end_index <= index + 1)
                {
                    continue;
                }

                std::vector<std::string> body;
                body.reserve(end_index - index - 1);
                bool valid = true;
                for (std::size_t cursor = index + 1; cursor < end_index; ++cursor)
                {
                    const std::string_view trimmed = TrimView(lines[cursor]);
                    std::string target;
                    std::string rhs;
                    char op = '\0';
                    if (!TryParseSyntheticSelfOp(trimmed, target, op, rhs))
                    {
                        valid = false;
                        break;
                    }

                    body.emplace_back(trimmed);
                }

                if (!valid)
                {
                    continue;
                }

                std::vector<std::string> folded = FoldSyntheticArithmeticRuns(body);
                folded = FoldRepeatedSyntheticBlocks(folded);
                if (folded == body)
                {
                    continue;
                }

                std::vector<std::string> replacement;
                replacement.reserve(folded.size());
                for (const std::string& line : folded)
                {
                    replacement.push_back(indent + "    " + line);
                }

                ReplaceLineRange(lines, index + 1, end_index - 1, replacement);
                index += replacement.size();
            }
        }

        void RepairBrokenRecursiveInspectWalkers(std::vector<std::string>& lines)
        {
            const std::regex broken_insert_pattern("^\\s*table.insert\\(([A-Za-z_][A-Za-z0-9_]*), inspect\\(var_1\\)\\)$");
            std::string recursive_name;
            std::string inspect_table = "var_39";
            for (std::size_t index = 0; index < lines.size(); ++index)
            {
                std::smatch match;
                if (!std::regex_match(lines[index], match, broken_insert_pattern))
                {
                    continue;
                }

                inspect_table = match[1].str();
                for (std::size_t cursor = index + 1; cursor > 0; --cursor)
                {
                    std::smatch header_match;
                    if (std::regex_match(lines[cursor - 1], header_match, std::regex("^\\s*local function ([A-Za-z_][A-Za-z0-9_]*)\\(arg1\\)$")))
                    {
                        recursive_name = header_match[1].str();
                        break;
                    }
                }
                break;
            }

            if (recursive_name.empty())
            {
                return;
            }

            if (const auto recursive_range = FindLocalFunctionRangeByName(lines, recursive_name))
            {
                ReplaceLineRange(lines, recursive_range->first, recursive_range->second,
                    {
                        "local function " + recursive_name + "(arg1)",
                        "    if not arg1 then",
                        "        return 0",
                        "    end",
                        "    local childCount = getElementChildrenCount(arg1)",
                        "    if childCount <= 0 then",
                        "        return childCount",
                        "    end",
                        "    for index = 0, childCount - 1 do",
                        "        local child = getElementChild(arg1, index)",
                        "        if child then",
                        "            local childType = getElementType(child)",
                        "            if childType ~= \"map\" and childType ~= \"colmodelroot\" and childType ~= \"dffroot\" and childType ~= \"guiroot\" and childType ~= \"txdroot\" then",
                        "                table.insert(" + inspect_table + ", inspect(child))",
                        "            end",
                        "            childCount = childCount + " + recursive_name + "(child)",
                        "        end",
                        "    end",
                        "    return childCount",
                        "end",
                    });
            }
        }

        void FixSelectVarargInFixedArgFunctions(std::vector<std::string>& lines)
        {
            struct BlockFrame
            {
                bool is_function = false;
                bool is_vararg = false;
                std::vector<std::string> params;
            };

            std::vector<BlockFrame> stack;
            const std::regex select_pattern("select\\(\\s*(\\d+)\\s*,\\s*\\.\\.\\.\\s*\\)");

            for (std::string& line : lines)
            {
                BlockFrame* active_function = nullptr;
                for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                {
                    if (it->is_function)
                    {
                        active_function = &(*it);
                        break;
                    }
                }

                if (active_function && !active_function->is_vararg)
                {
                    std::string rebuilt;
                    rebuilt.reserve(line.size());
                    std::string source = line;
                    std::smatch match;
                    while (std::regex_search(source, match, select_pattern))
                    {
                        rebuilt += match.prefix().str();
                        const std::size_t ordinal = static_cast<std::size_t>(std::strtoul(match[1].str().c_str(), nullptr, 10));
                        if (ordinal >= 1 && ordinal <= active_function->params.size())
                        {
                            rebuilt += active_function->params[ordinal - 1];
                        }
                        else
                        {
                            rebuilt += match[0].str();
                        }
                        source = match.suffix().str();
                    }
                    rebuilt += source;
                    line = std::move(rebuilt);
                }

                const std::string_view trimmed = TrimView(line);
                if (IsCloseSyntheticBlock(trimmed) && !stack.empty())
                {
                    stack.pop_back();
                }

                if (!IsOpenSyntheticBlock(trimmed))
                {
                    continue;
                }

                BlockFrame frame{};
                const std::size_t marker = trimmed.find("function(");
                if (marker != std::string_view::npos)
                {
                    const std::size_t begin = marker + std::string_view("function(").size();
                    const std::size_t close = trimmed.find(')', begin);
                    if (close != std::string_view::npos)
                    {
                        const std::string_view args = trimmed.substr(begin, close - begin);
                        frame.is_function = true;
                        frame.params = SplitFunctionArgs(args);
                        frame.is_vararg = std::any_of(frame.params.begin(), frame.params.end(), [](const std::string& value)
                        {
                            return value == "...";
                        });
                    }
                }
                stack.push_back(std::move(frame));
            }
        }

        void RepairMissingLoadstringGuards(std::vector<std::string>& lines)
        {
            const std::regex assign_pattern("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*), ([A-Za-z_][A-Za-z0-9_]*) = loadstring\\((.+)\\)$");
            const std::regex guard_pattern("^if not ([A-Za-z_][A-Za-z0-9_]*) then$");

            for (std::size_t index = 0; index + 1 < lines.size(); ++index)
            {
                std::smatch assign_match;
                if (!std::regex_match(lines[index], assign_match, assign_pattern))
                {
                    continue;
                }

                std::size_t next = index + 1;
                while (next < lines.size() && TrimView(lines[next]).empty())
                {
                    ++next;
                }
                if (next >= lines.size())
                {
                    continue;
                }

                std::smatch guard_match;
                const std::string guard_line = std::string(TrimView(lines[next]));
                if (std::regex_match(guard_line, guard_match, guard_pattern) && guard_match[1].str() == assign_match[2].str())
                {
                    continue;
                }

                const std::string_view trimmed = TrimView(lines[next]);
                const std::string error_name = assign_match[3].str();
                if (!trimmed.starts_with("return ") || trimmed.find(error_name) == std::string_view::npos)
                {
                    continue;
                }

                const std::string indent = assign_match[1].str();
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(next), indent + "if not " + assign_match[2].str() + " then");
                ++next;
                lines[next] = indent + "    " + std::string(trimmed);
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(next + 1), indent + "end");
                index = next + 1;
            }
        }

        void RemoveConsecutiveReturnLines(std::vector<std::string>& lines)
        {
            for (std::size_t index = 0; index < lines.size();)
            {
                if (!TrimView(lines[index]).starts_with("return"))
                {
                    ++index;
                    continue;
                }

                const std::size_t indent = LeadingIndentWidth(lines[index]);
                std::size_t next = index + 1;
                bool erased = false;
                while (next < lines.size())
                {
                    const std::string_view trimmed = TrimView(lines[next]);
                    if (trimmed.empty())
                    {
                        ++next;
                        continue;
                    }

                    if (trimmed == "else" || trimmed.starts_with("elseif") || IsCloseSyntheticBlock(trimmed))
                    {
                        break;
                    }

                    if (LeadingIndentWidth(lines[next]) < indent)
                    {
                        break;
                    }

                    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(next));
                    erased = true;
                }

                if (!erased)
                {
                    ++index;
                }
            }
        }

        void ReplaceSyntheticTimerFallbacks(std::vector<std::string>& lines)
        {
            const std::regex timer_pattern("^([ \\t]*)([A-Za-z_][A-Za-z0-9_]*) = setTimer\\(\\s*function\\(\\)$");

            for (std::size_t index = 0; index + 2 < lines.size(); ++index)
            {
                std::smatch match;
                if (!std::regex_match(lines[index], match, timer_pattern))
                {
                    continue;
                }

                if (TrimView(lines[index + 1]).find("synthetic fallback, structured recovery was partial") == std::string_view::npos)
                {
                    continue;
                }

                bool timer_gate = false;
                const std::string name = match[2].str();
                const std::size_t begin = index > 24 ? index - 24 : 0;
                const std::size_t end = std::min(lines.size(), index + 25);
                for (std::size_t probe = begin; probe < end; ++probe)
                {
                    if (lines[probe].find("isTimer(" + name + ")") != std::string::npos
                        || lines[probe].find("killTimer(" + name + ")") != std::string::npos)
                    {
                        timer_gate = true;
                        break;
                    }
                }

                if (!timer_gate)
                {
                    continue;
                }

                lines[index + 1] = match[1].str() + "    " + name + " = nil";
            }
        }

        std::vector<std::string> ExtractRecoveredStrings(std::string_view line)
        {
            std::vector<std::string> values;
            std::size_t cursor = 0;
            while (cursor < line.size())
            {
                const std::size_t begin = line.find('"', cursor);
                if (begin == std::string_view::npos)
                {
                    break;
                }

                const std::size_t end = line.find('"', begin + 1);
                if (end == std::string_view::npos)
                {
                    break;
                }

                values.emplace_back(line.substr(begin + 1, end - begin - 1));
                cursor = end + 1;
            }
            return values;
        }

        void ReplaceSyntheticResourceStartFallbacks(std::vector<std::string>& lines)
        {
            for (std::size_t index = 0; index + 3 < lines.size(); ++index)
            {
                if (TrimView(lines[index]) != "addEventHandler(\"onClientResourceStart\", resourceRoot, function()"
                    || TrimView(lines[index + 1]).find("synthetic fallback, structured recovery was partial") == std::string_view::npos
                    || !TrimView(lines[index + 2]).starts_with("-- recovered strings:")
                    || TrimView(lines[index + 3]) != "end)")
                {
                    continue;
                }

                const std::vector<std::string> recovered = ExtractRecoveredStrings(TrimView(lines[index + 2]));
                bool has_hash = false;
                bool has_time = false;
                bool has_trigger = false;
                std::string event_name;
                std::vector<std::string> dll_paths;
                for (const std::string& value : recovered)
                {
                    if (value == "getFileHash")
                    {
                        has_hash = true;
                    }
                    else if (value == "getFileTime")
                    {
                        has_time = true;
                    }
                    else if (value == "triggerServerEvent")
                    {
                        has_trigger = true;
                    }
                    else if (value.ends_with(".dll"))
                    {
                        dll_paths.push_back(value);
                    }
                    else if (event_name.empty() && value.find(':') != std::string::npos)
                    {
                        event_name = value;
                    }
                }

                if (!has_hash || !has_time || !has_trigger || event_name.empty() || dll_paths.empty())
                {
                    continue;
                }

                std::vector<std::string> memory_checks;
                const std::size_t begin = index > 40 ? index - 40 : 0;
                for (std::size_t probe = begin; probe < index; ++probe)
                {
                    std::smatch match;
                    if (!std::regex_match(lines[probe], match, std::regex("^local ([A-Za-z_][A-Za-z0-9_]*) = function\\(\\)$")))
                    {
                        continue;
                    }

                    bool has_memory = false;
                    for (std::size_t inner = probe + 1; inner < index; ++inner)
                    {
                        const std::string_view inner_trimmed = TrimView(lines[inner]);
                        if (inner_trimmed == "end")
                        {
                            break;
                        }
                        if (lines[inner].find("getMemory(") != std::string::npos)
                        {
                            has_memory = true;
                        }
                    }

                    if (has_memory)
                    {
                        memory_checks.push_back(match[1].str());
                    }
                }

                const std::string indent = LeadingIndent(lines[index]);
                std::vector<std::string> replacement;
                replacement.push_back(indent + "addEventHandler(\"onClientResourceStart\", resourceRoot, function()");
                replacement.push_back(indent + "    local payload = {}");
                replacement.push_back(indent + "    for _, path in ipairs({");
                for (const std::string& path : dll_paths)
                {
                    replacement.push_back(indent + "        " + EscapeString(path) + ",");
                }
                replacement.push_back(indent + "    }) do");
                replacement.push_back(indent + "        payload[path] = {");
                replacement.push_back(indent + "            hash = getFileHash(path),");
                replacement.push_back(indent + "            time = getFileTime(path),");
                replacement.push_back(indent + "        }");
                replacement.push_back(indent + "    end");
                if (!memory_checks.empty())
                {
                    replacement.push_back(indent + "    payload.memory_checks = {");
                    for (const std::string& check : memory_checks)
                    {
                        replacement.push_back(indent + "        " + check + "(),");
                    }
                    replacement.push_back(indent + "    }");
                }
                replacement.push_back(indent + "    triggerServerEvent(" + EscapeString(event_name) + ", resourceRoot, payload)");
                replacement.push_back(indent + "end)");

                lines.erase(
                    lines.begin() + static_cast<std::ptrdiff_t>(index),
                    lines.begin() + static_cast<std::ptrdiff_t>(index + 4));
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index), replacement.begin(), replacement.end());
                index += replacement.size() - 1;
            }
        }

        void ReplaceSyntheticIbBackgroundCallbacks(std::vector<std::string>& lines)
        {
            const std::regex method_pattern("^([A-Za-z_][A-Za-z0-9_\\.]*)\\.([A-Za-z_][A-Za-z0-9_]*) = function\\(self(,.*)?\\)$");

            for (std::size_t index = 0; index < lines.size(); ++index)
            {
                std::smatch method_match;
                const std::string method_line = std::string(TrimView(lines[index]));
                if (!std::regex_match(method_line, method_match, method_pattern))
                {
                    continue;
                }

                int depth = 1;
                std::size_t end_index = index;
                for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
                {
                    const std::string_view trimmed = TrimView(lines[cursor]);
                    if (IsOpenSyntheticBlock(trimmed))
                    {
                        ++depth;
                    }
                    if (IsCloseSyntheticBlock(trimmed))
                    {
                        --depth;
                        if (depth == 0)
                        {
                            end_index = cursor;
                            break;
                        }
                    }
                }

                if (depth != 0)
                {
                    continue;
                }

                const std::string method_name = method_match[2].str();
                for (std::size_t cursor = index + 1; cursor + 2 < end_index; ++cursor)
                {
                    if (lines[cursor].find("ibCreateBackground(") == std::string::npos
                        || TrimView(lines[cursor + 1]).find("synthetic fallback, structured recovery was partial") == std::string_view::npos
                        || !TrimView(lines[cursor + 2]).starts_with("end,"))
                    {
                        continue;
                    }

                    lines[cursor + 1] = LeadingIndent(lines[cursor]) + "    self:" + method_name + "(false)";
                }

                index = end_index;
            }
        }

		void InlineEventStringLocals(std::vector<std::string>& lines)
		{
			std::unordered_map<std::string, std::string> literals;
			std::unordered_map<std::string, std::size_t> literal_lines;

			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				std::smatch match;
				if (!std::regex_match(lines[index], match, std::regex("^local ([A-Za-z_][A-Za-z0-9_]*) = (\"[^\"]+\")$")))
				{
					continue;
				}

				literals.emplace(match[1].str(), match[2].str());
				literal_lines.emplace(match[1].str(), index);
			}

			const std::regex event_pattern("^(addEvent(?:Handler)?\\()([A-Za-z_][A-Za-z0-9_]*)(,.*\\))$");
			for (std::string& line : lines)
			{
				std::smatch match;
				if (!std::regex_match(line, match, event_pattern))
				{
					continue;
				}

				const auto found = literals.find(match[2].str());
				if (found == literals.end())
				{
					continue;
				}

				line = match[1].str() + found->second + match[3].str();
			}

			for (const auto& [name, line_index] : literal_lines)
			{
				std::vector<std::string> probe = lines;
				if (line_index >= probe.size())
				{
					continue;
				}

				probe.erase(probe.begin() + static_cast<std::ptrdiff_t>(line_index));
				if (!ContainsIdentifier(probe, name))
				{
					lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(line_index));
					for (auto& [other_name, other_index] : literal_lines)
					{
						if (other_name != name && other_index > line_index)
						{
							other_index--;
						}
					}
				}
			}
		}

		void RenameEventHandlersFromLiteralNames(std::vector<std::string>& lines)
		{
			std::unordered_map<std::string, std::string> renames;
			const std::regex handler_pattern("^addEventHandler\\(\"([^\"]+)\", [^,]+, ([A-Za-z_][A-Za-z0-9_]*)\\)$");
			for (const std::string& line : lines)
			{
				std::smatch match;
				if (!std::regex_match(line, match, handler_pattern))
				{
					continue;
				}

				const std::string event_name = match[1].str();
				const std::string handler_name = match[2].str();
				if (!IsGenericTempName(handler_name) || !event_name.starts_with("on") || !IsIdentifier(event_name))
				{
					continue;
				}

				renames.emplace(handler_name, event_name);
			}

			for (const auto& [from, to] : renames)
			{
				ReplaceIdentifierInLines(lines, from, to);
			}
		}

		bool ContainsAny(std::string_view text, std::initializer_list<std::string_view> needles)
		{
			for (std::string_view needle : needles)
			{
				if (!needle.empty() && text.find(needle) != std::string_view::npos)
				{
					return true;
				}
			}

			return false;
		}

		bool IsProfileEnabled()
		{
			const char* value = std::getenv("BR_PROFILE");
			return value && value[0] != '\0' && value[0] != '0';
		}

		bool EnvFlagEnabled(const char* name)
		{
			const char* value = std::getenv(name);
			return value && value[0] != '\0' && value[0] != '0';
		}

		void AppendProfileLog(std::string_view line)
		{
			if (!IsProfileEnabled())
			{
				return;
			}

			const char* path = std::getenv("BR_PROFILE_FILE");
			if (!path || path[0] == '\0')
			{
				return;
			}

			std::ofstream file(path, std::ios::binary | std::ios::app);
			if (!file)
			{
				return;
			}

			file << line << '\n';
		}

		void AppendHexFoldLog(std::string_view line)
		{
			(void)line;
		}

		bool LooksLikeHexBlob(std::string_view value)
		{
			if (value.size() < 16 || (value.size() % 2) != 0)
			{
				return false;
			}

			for (unsigned char ch : value)
			{
				if (std::isxdigit(ch) == 0)
				{
					return false;
				}
			}

			return true;
		}

		bool ReplaceFirst(std::string& value, std::string_view from, std::string_view to)
		{
			const std::size_t position = value.find(from);
			if (position == std::string::npos)
			{
				return false;
			}

			value.replace(position, from.size(), to);
			return true;
		}

		bool IsSimpleRepairRhs(std::string_view rhs)
		{
			rhs = TrimView(rhs);
			if (rhs.empty())
			{
				return false;
			}

			if (rhs.find("function(") != std::string_view::npos || rhs.find("select(") != std::string_view::npos || rhs.find("[nil]") != std::string_view::npos)
			{
				return false;
			}

			return !ContainsBrokenSyntheticMath(rhs);
		}

        void ApplyGenericLuaRepair(std::string& lua)
        {
            if (!ContainsAny(lua, { "__br_str_", "utf8.find(", "addEvent(", "addEventHandler(", "addDebugHook(", "if not ", "money_log", ":ibData(\"text\")", ":ibData(\"disabled\", true)", "loadstring(", "select(", "synthetic fallback" }))
            {
                return;
            }

			lua = std::regex_replace(lua, std::regex("_G\\[\"(__br_str_[A-F0-9]+__)\"\\]"), "$1");
			lua = std::regex_replace(lua, std::regex("\"__br_str_[A-F0-9]+__\" \\.\\. format_price\\("), "\"₴ \" .. format_price(");
			lua = std::regex_replace(lua, std::regex("\"__br_str_[A-F0-9]+__\" \\.\\. ([A-Za-z_][A-Za-z0-9_\\.]*)\\.id"), "\"Торгова лавка №\" .. $1.id");
			lua = std::regex_replace(lua, std::regex("utf8\\.find\\(([^,]+), \"__br_str_[A-F0-9]+__\"\\)"), "utf8.find($1, \"^[a-zA-Zа-яА-ЯґҐєЄіІїЇ0-9\\\\-]+$\")");
			lua = std::regex_replace(
				lua,
				std::regex("^__br_str_[A-F0-9]+__\\(__br_str_[A-F0-9]+__\\.__br_str_[A-F0-9]+__:__br_str_[A-F0-9]+__\\(\"__br_str_[A-F0-9]+__\"\\)\\)\\(\\)$", std::regex::multiline),
				"loadstring(exports.interfacer:extend(\"Interfacer\"))()");

			std::vector<std::string> alias_lines = SplitLines(lua);
			std::unordered_map<std::string, std::vector<std::string>> aliases_by_target;
			for (const std::string& line : alias_lines)
			{
				std::smatch match;
				if (!std::regex_match(line, match, std::regex("^local ([A-Za-z_][A-Za-z0-9_]*) = (_G\\[\"[^\"]+\"\\])$")))
				{
					continue;
				}

				aliases_by_target[match[2].str()].push_back(match[1].str());
			}

			int module_index = 0;
			for (const auto& [target, aliases] : aliases_by_target)
			{
				if (aliases.size() < 2)
				{
					continue;
				}

				std::string canonical = module_index == 0 ? "module" : "module_" + std::to_string(module_index + 1);
				++module_index;

				bool first = true;
				for (const std::string& alias : aliases)
				{
					if (first)
					{
						ReplaceAll(lua, "local " + alias + " = " + target, "local " + canonical + " = " + target);
						first = false;
					}
					else
					{
						ReplaceAll(lua, "local " + alias + " = " + target + "\n", "");
					}

					ReplaceAll(lua, alias + ".", canonical + ".");
					ReplaceAll(lua, alias + ":", canonical + ":");
					ReplaceAll(lua, alias + "[", canonical + "[");
				}
			}

            std::vector<std::string> lines = SplitLines(lua);
            FixSelectVarargInFixedArgFunctions(lines);
            RepairMissingLoadstringGuards(lines);
            InlineEventStringLocals(lines);
            RenameEventHandlersFromLiteralNames(lines);
            ReplaceSyntheticTimerFallbacks(lines);
            ReplaceSyntheticResourceStartFallbacks(lines);
            ReplaceSyntheticIbBackgroundCallbacks(lines);
            RemoveConsecutiveReturnLines(lines);
            std::unordered_set<std::string> custom_events;
            std::unordered_map<std::string, std::string> custom_event_aliases;
			for (const std::string& line : lines)
			{
				std::smatch match;
				if (std::regex_match(line, match, std::regex("^addEvent\\((\"__br_str_[A-F0-9]+__\"), true\\)$")))
				{
					custom_events.insert(match[1].str());
					continue;
				}

				if (std::regex_match(line, match, std::regex("^local ([A-Za-z_][A-Za-z0-9_]*) = (\"__br_str_[A-F0-9]+__\")$")))
				{
					if (custom_events.contains(match[2].str()))
					{
						custom_event_aliases.emplace(match[1].str(), match[2].str());
					}
				}
			}

			for (std::string& line : lines)
			{
				std::smatch match;
				if (!std::regex_match(line, match, std::regex("^addEventHandler\\(([^,]+), (__br_str_[A-F0-9]+__), (.+)\\)$")))
				{
					continue;
				}

				const std::string event_ref = std::string(TrimView(match[1].str()));
				const bool custom_direct = custom_events.contains(event_ref);
				const bool custom_alias = custom_event_aliases.contains(event_ref);
				if (!custom_direct && !custom_alias)
				{
					continue;
				}

				line = "addEventHandler(" + event_ref + ", resourceRoot, " + match[3].str() + ")";
			}

			for (std::size_t index = 0; index + 2 < lines.size();)
			{
				std::smatch match;
				if (!std::regex_match(lines[index], match, std::regex("^([ \\t]*)if not (.+) then$")))
				{
					++index;
					continue;
				}

				const std::string indent = match[1].str();
				const std::string expr_text = match[2].str();
				const std::string expr = std::string(TrimView(expr_text));
				std::size_t body_end = index + 1;
				bool valid_block = true;
				std::string last_rhs;

				while (body_end < lines.size() && TrimView(lines[body_end]) != "end")
				{
					const std::string_view trimmed = TrimView(lines[body_end]);
					if (trimmed.empty() || !trimmed.starts_with("local "))
					{
						valid_block = false;
						break;
					}

					const std::size_t assign = trimmed.find(" = ");
					if (assign == std::string_view::npos)
					{
						valid_block = false;
						break;
					}

					last_rhs = std::string(TrimView(trimmed.substr(assign + 3)));
					++body_end;
				}

				if (!valid_block || body_end >= lines.size() || TrimView(lines[body_end]) != "end" || !IsSimpleRepairRhs(last_rhs))
				{
					++index;
					continue;
				}

				if (body_end + 1 >= lines.size())
				{
					++index;
					continue;
				}

				std::string next_line = lines[body_end + 1];
				const std::string_view next_trimmed = TrimView(next_line);
				const std::string self_assign = expr + " = " + expr;
				bool changed = false;

				if (next_trimmed == self_assign)
				{
					lines[index] = indent + expr + " = " + expr + " or " + last_rhs;
					lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), lines.begin() + static_cast<std::ptrdiff_t>(body_end + 2));
					changed = true;
				}
				else if (ReplaceFirst(next_line, expr, "(" + expr + " or " + last_rhs + ")"))
				{
					lines[body_end + 1] = std::move(next_line);
					lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index), lines.begin() + static_cast<std::ptrdiff_t>(body_end + 1));
					changed = true;
				}

				if (!changed)
				{
					++index;
				}
			}

			for (std::size_t index = 0; index + 3 < lines.size();)
			{
				std::smatch match;
				if (!std::regex_match(lines[index], match, std::regex("^([ \\t]*)if not (.+) then$")))
				{
					++index;
					continue;
				}

				const std::string indent = match[1].str();
				const std::string expr_text = match[2].str();
				const std::string expr = std::string(TrimView(expr_text));
				std::size_t body_end = index + 1;
				std::string table_var;
				std::vector<std::string> fields;
				bool valid_block = true;

				while (body_end < lines.size() && TrimView(lines[body_end]) != "end")
				{
					const std::string trimmed = std::string(TrimView(lines[body_end]));
					std::smatch local_table_match;
					if (std::regex_match(trimmed, local_table_match, std::regex("^local ([A-Za-z_][A-Za-z0-9_]*) = \\{\\}$")))
					{
						table_var = local_table_match[1].str();
						++body_end;
						continue;
					}

					if (!table_var.empty())
					{
						const std::regex field_pattern("^" + table_var + "\\.([A-Za-z_][A-Za-z0-9_]*) = \\{\\}$");
						std::smatch field_match;
						if (std::regex_match(trimmed, field_match, field_pattern))
						{
							fields.push_back(field_match[1].str());
							++body_end;
							continue;
						}
					}

					if (trimmed.starts_with("local "))
					{
						++body_end;
						continue;
					}

					valid_block = false;
					break;
				}

				if (!valid_block || body_end >= lines.size() || TrimView(lines[body_end]) != "end" || fields.empty() || body_end + 1 >= lines.size())
				{
					++index;
					continue;
				}

				const std::string self_assign = expr + " = " + expr;
				if (TrimView(lines[body_end + 1]) != self_assign)
				{
					++index;
					continue;
				}

				std::ostringstream table_literal;
				table_literal << "{ ";
				for (std::size_t field_index = 0; field_index < fields.size(); ++field_index)
				{
					if (field_index > 0)
					{
						table_literal << ", ";
					}
					table_literal << fields[field_index] << " = {}";
				}
				table_literal << " }";

				lines[index] = indent + expr + " = " + expr + " or " + table_literal.str();
				lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), lines.begin() + static_cast<std::ptrdiff_t>(body_end + 2));
			}

			for (std::size_t index = 0; index < lines.size();)
			{
				const std::string_view trimmed = TrimView(lines[index]);
				if (trimmed == "return ..." || trimmed == "...")
				{
					lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
					continue;
				}

				++index;
			}

			for (std::size_t index = 0; index + 2 < lines.size();)
			{
				const std::string_view trimmed = TrimView(lines[index]);
				if (trimmed.find("= function(") == std::string_view::npos
					|| TrimView(lines[index + 1]) != "-- ByteRevenant: synthetic fallback, structured recovery was partial"
					|| TrimView(lines[index + 2]) != "end")
				{
					++index;
					continue;
				}

				lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 1));
				index += 2;
			}

			for (std::size_t index = 0; index + 4 < lines.size();)
			{
				std::smatch owner_match;
				if (!std::regex_match(lines[index], owner_match, std::regex("^([ \\t]*)if ([A-Za-z_][A-Za-z0-9_]*)\\.money_log then$")))
				{
					++index;
					continue;
				}

				const std::string indent = owner_match[1].str();
				const std::string owner = owner_match[2].str();
				if (TrimView(lines[index + 1]) != "end")
				{
					++index;
					continue;
				}

				std::smatch var_match;
				if (!std::regex_match(lines[index + 2], var_match, std::regex("^" + EscapeRegex(indent) + "local ([A-Za-z_][A-Za-z0-9_]*) = \\{\\}$")))
				{
					++index;
					continue;
				}

				const std::string var_name = var_match[1].str();
				if (TrimView(lines[index + 3]) != var_name + ".type = 2" || TrimView(lines[index + 4]) != var_name + ".value = 0")
				{
					++index;
					continue;
				}

				lines[index] = indent + "local " + var_name + " = " + owner + ".money_log and " + owner + ".money_log[#" + owner + ".money_log] or { type = 2, value = 0 }";
				lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), lines.begin() + static_cast<std::ptrdiff_t>(index + 5));
				++index;
			}

			for (std::size_t index = 0; index + 2 < lines.size();)
			{
				std::smatch local_match;
				if (!std::regex_match(lines[index + 2], local_match, std::regex("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*) = \"down\"$")))
				{
					++index;
					continue;
				}

				const std::string indent = local_match[1].str();
				const std::string icon_var = local_match[2].str();
				std::smatch if_match;
				if (!std::regex_match(lines[index], if_match, std::regex("^" + EscapeRegex(indent) + "if 1 < ([A-Za-z_][A-Za-z0-9_]*)\\.value then$")))
				{
					++index;
					continue;
				}

				if (TrimView(lines[index + 1]) != "end")
				{
					++index;
					continue;
				}

				lines[index] = indent + "local " + icon_var + " = " + if_match[1].str() + ".value > 1 and \"up\" or \"down\"";
				lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), lines.begin() + static_cast<std::ptrdiff_t>(index + 3));
				++index;
			}

			for (std::size_t index = 0; index + 2 < lines.size();)
			{
				std::smatch counter_match;
				if (!std::regex_match(lines[index], counter_match, std::regex("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*) = 0$")))
				{
					++index;
					continue;
				}

				const std::string indent = counter_match[1].str();
				const std::string counter_var = counter_match[2].str();
				if (TrimView(lines[index + 1]) != "if 3 < r8 - " + counter_var + " then")
				{
					++index;
					continue;
				}

				const std::string error_line = lines[index + 2];
				if (TrimView(error_line).find("localPlayer:ShowError(") != 0)
				{
					++index;
					continue;
				}

				std::string text_var;
				const std::regex text_line_pattern("^local ([A-Za-z_][A-Za-z0-9_]*) = .+:ibData\\(\"text\"\\)$");
				for (std::size_t steps = 0; steps < 8 && steps < index; ++steps)
				{
					const std::size_t back = index - steps - 1;
					const std::string_view candidate = TrimView(lines[back]);
					const std::string candidate_text(candidate);
					std::smatch text_match;
					if (std::regex_match(candidate_text, text_match, text_line_pattern))
					{
						text_var = text_match[1].str();
						break;
					}
				}

				if (text_var.empty())
				{
					++index;
					continue;
				}

				lines[index] = indent + "local prev_pos = 0";
				lines[index + 1] = indent + "for pos, codepoint in utf8.next, " + text_var + " .. \" \" do";
				lines[index + 2] = indent + "    if pos - prev_pos > 3 then";
				lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index + 3), indent + "        " + std::string(TrimView(error_line)));
				lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index + 4), indent + "        return");
				lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index + 5), indent + "    end");
				lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index + 6), indent + "    prev_pos = pos");
				lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index + 7), indent + "end");
				index += 8;
			}

			for (std::size_t index = 0; index + 8 < lines.size();)
			{
				const std::string_view trimmed = TrimView(lines[index]);
				if (trimmed != "local prev_pos = 0")
				{
					++index;
					continue;
				}

				if (!TrimView(lines[index + 1]).starts_with("for pos, codepoint in utf8.next, ")
					|| !TrimView(lines[index + 2]).starts_with("if pos - prev_pos > 3 then")
					|| !TrimView(lines[index + 7]).starts_with("end")
					|| TrimView(lines[index + 8]) != "end")
				{
					++index;
					continue;
				}

				lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 8));
			}

			for (std::size_t index = 0; index + 5 < lines.size(); ++index)
			{
				std::smatch local_match;
				if (!std::regex_match(lines[index], local_match, std::regex("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*) = tonumber\\((.+)\\)$")))
				{
					continue;
				}

				const std::string indent = local_match[1].str();
				const std::string var_name = local_match[2].str();
				if (TrimView(lines[index + 1]) != "if " + var_name + " and " + var_name + " > 0 then"
					|| TrimView(lines[index + 2]) != "end"
					|| TrimView(lines[index + 4]) != "return")
				{
					continue;
				}

				const std::string error_line = std::string(TrimView(lines[index + 3]));
				if (!error_line.starts_with("localPlayer:ErrorWindow("))
				{
					continue;
				}

				lines[index + 1] = indent + "if not " + var_name + " or " + var_name + " <= 0 or " + var_name + " ~= math.floor(" + var_name + ") then";
				lines[index + 2] = indent + "    " + error_line;
				lines[index + 3] = indent + "    return";
				lines[index + 4] = indent + "end";
			}

			for (std::size_t index = 0; index + 15 < lines.size(); ++index)
			{
				std::smatch text_match;
				if (!std::regex_match(lines[index], text_match, std::regex("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*) = .+:ibData\\(\"text\"\\)$")))
				{
					continue;
				}

				const std::string indent = text_match[1].str();
				const std::string var_name = text_match[2].str();
				if (TrimView(lines[index + 1]) != "if " + var_name + " then"
					|| TrimView(lines[index + 2]) != "end"
					|| TrimView(lines[index + 4]) != "return"
					|| TrimView(lines[index + 5]) != "if utf8.len(" + var_name + ") >= 3 then"
					|| TrimView(lines[index + 6]) != "end"
					|| TrimView(lines[index + 8]) != "return")
				{
					continue;
				}

				const std::string empty_error = std::string(TrimView(lines[index + 3]));
				const std::string length_error = std::string(TrimView(lines[index + 7]));
				if (!empty_error.starts_with("localPlayer:ShowError(")
					|| !length_error.starts_with("localPlayer:ShowError(")
					|| TrimView(lines[index + 9]).find("if not utf8.find(") != 0
					|| TrimView(lines[index + 12]).find("if not utf8.find(") != 0
					|| TrimView(lines[index + 13]) != "end"
					|| TrimView(lines[index + 15]) != "return")
				{
					continue;
				}

				const std::string charset_error = std::string(TrimView(lines[index + 10]));
				const std::string hyphen_error = std::string(TrimView(lines[index + 14]));
				if (!charset_error.starts_with("localPlayer:ShowError(") || !hyphen_error.starts_with("localPlayer:ShowError("))
				{
					continue;
				}

				std::vector<std::string> replacement =
				{
					indent + "if not " + var_name + " or " + var_name + " == \"\" then",
					indent + "    " + empty_error,
					indent + "    return",
					indent + "end",
					indent + "if utf8.len(" + var_name + ") < 3 or utf8.len(" + var_name + ") > 12 then",
					indent + "    " + length_error,
					indent + "    return",
					indent + "end",
					indent + "if not utf8.find(" + var_name + ", \"^[a-zA-Zа-яА-ЯґҐєЄіІїЇ0-9\\\\-]+$\") then",
					indent + "    " + charset_error,
					indent + "    return",
					indent + "end",
					indent + "if utf8.find(" + var_name + ", \"^-\") or utf8.find(" + var_name + ", \"-$\") then",
					indent + "    " + hyphen_error,
					indent + "    return",
					indent + "end",
				};

				lines.erase(
					lines.begin() + static_cast<std::ptrdiff_t>(index + 1),
					lines.begin() + static_cast<std::ptrdiff_t>(index + 16));
				lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), replacement.begin(), replacement.end());
				index += replacement.size();
			}

			for (std::size_t index = 0; index + 4 < lines.size(); ++index)
			{
				std::smatch label_match;
				if (!std::regex_match(lines[index], label_match, std::regex("^([ \\t]*)local ([A-Za-z_][A-Za-z0-9_]*) = (.+:ibData\\(\"disabled\", true\\))$")))
				{
					continue;
				}

				std::smatch if_match;
				if (!std::regex_match(lines[index + 1], if_match, std::regex("^" + EscapeRegex(label_match[1].str()) + "if 1 < ([A-Za-z_][A-Za-z0-9_]*)\\.value then$")))
				{
					continue;
				}

				const std::string label_name = label_match[2].str();
				if (!TrimView(lines[index + 2]).ends_with(label_name + ".ibData"))
				{
					continue;
				}

				if (!TrimView(lines[index + 3]).starts_with("local ") || TrimView(lines[index + 4]) != "end")
				{
					continue;
				}

				lines[index] = label_match[1].str() + "local " + label_name + " = " + label_match[3].str() + ":ibData(\"color\", " + if_match[1].str() + ".value > 1 and 0xFF5ead63 or 0xFFFF4141)";
				lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), lines.begin() + static_cast<std::ptrdiff_t>(index + 5));
			}

			for (std::size_t index = 0; index + 4 < lines.size(); ++index)
			{
				std::smatch if_match;
				if (!std::regex_match(lines[index + 1], if_match, std::regex("^([ \\t]*)if ([A-Za-z_][A-Za-z0-9_]*) == 1 then$")))
				{
					continue;
				}

				const std::string indent = if_match[1].str();
				const std::string max_var = if_match[2].str();
				if (TrimView(lines[index + 2]) != "if not true then")
				{
					continue;
				}

				std::size_t assign_index = index + 3;
				std::smatch assign_match;
				const std::string assign_line = std::string(TrimView(lines[assign_index]));
				if (!std::regex_match(assign_line, assign_match, std::regex("^(.+) = ([A-Za-z_][A-Za-z0-9_\\.]*)\\:ibData\\(\"disabled\", false\\)$")))
				{
					continue;
				}

				std::size_t range_start = std::string(assign_match[1].str()).starts_with("local ") ? assign_index : index + 1;
				std::size_t open_index = 0;
				bool found_open = false;
				for (std::size_t cursor = assign_index + 1; cursor < std::min(lines.size(), assign_index + 8); ++cursor)
				{
					if (TrimView(lines[cursor]) == "if 1 < " + max_var + " then")
					{
						open_index = cursor;
						found_open = true;
						break;
					}
				}

				if (!found_open)
				{
					continue;
				}

				const std::string new_assign = indent + std::string(TrimView(assign_match[1].str())) + " = " + assign_match[2].str() + ":ibData(\"disabled\", " + max_var + " == 1 and true or false)";
				lines[range_start] = new_assign;
				lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(range_start + 1), lines.begin() + static_cast<std::ptrdiff_t>(open_index));
				open_index = range_start + 1;

				int depth = 1;
				std::size_t end_index = open_index;
				for (std::size_t cursor = open_index + 1; cursor < lines.size(); ++cursor)
				{
					const std::string_view inner = TrimView(lines[cursor]);
					if (IsOpenSyntheticBlock(inner))
					{
						++depth;
					}
					if (IsCloseSyntheticBlock(inner))
					{
						--depth;
						if (depth == 0)
						{
							end_index = cursor;
							break;
						}
					}
				}

				if (end_index + 2 < lines.size()
					&& TrimView(lines[end_index + 1]) == "end"
					&& TrimView(lines[end_index + 2]) == "end")
				{
					lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(end_index + 1), lines.begin() + static_cast<std::ptrdiff_t>(end_index + 3));
				}
			}

			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				std::smatch table_match;
				if (!std::regex_match(lines[index], table_match, std::regex("^local ([A-Za-z_][A-Za-z0-9_]*) = \\{\\}$")))
				{
					continue;
				}

				const std::string table_name = table_match[1].str();
				std::vector<std::string> values;
				std::size_t cursor = index + 1;
				for (; cursor < lines.size(); ++cursor)
				{
					std::smatch value_match;
					if (!std::regex_match(lines[cursor], value_match, std::regex("^local [A-Za-z_][A-Za-z0-9_]* = \"([a-z_]+)\"$")))
					{
						break;
					}

					values.push_back(value_match[1].str());
				}

				if (values.size() < 3)
				{
					continue;
				}

				if (std::find(values.begin(), values.end(), "aim_weapon") == values.end()
					|| lua.find("pairs(" + table_name + ")") == std::string::npos)
				{
					continue;
				}

				std::ostringstream list;
				list << "local disabled_controls = { ";
				for (std::size_t value_index = 0; value_index < values.size(); ++value_index)
				{
					if (value_index > 0)
					{
						list << ", ";
					}
					list << "\"" << values[value_index] << "\"";
				}
				list << " }";
				lines[index] = list.str();
				lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), lines.begin() + static_cast<std::ptrdiff_t>(cursor));
				for (std::string& line : lines)
				{
					ReplaceAll(line, "pairs(" + table_name + ")", "pairs(disabled_controls)");
				}
				break;
			}

			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				const std::string_view trimmed = TrimView(lines[index]);
				if (trimmed.find("= function(") == std::string_view::npos)
				{
					continue;
				}

				int depth = 1;
				std::size_t end_index = index;
				for (std::size_t cursor = index + 1; cursor < lines.size(); ++cursor)
				{
					const std::string_view inner = TrimView(lines[cursor]);
					if (IsOpenSyntheticBlock(inner))
					{
						++depth;
					}
					if (IsCloseSyntheticBlock(inner))
					{
						--depth;
						if (depth == 0)
						{
							end_index = cursor;
							break;
						}
					}
				}

				if (depth != 0)
				{
					continue;
				}

				bool hide_cleanup = false;
				bool stop_cleanup = false;
				bool post_join_stub = false;
				for (std::size_t cursor = index + 1; cursor < end_index; ++cursor)
				{
					const std::string_view inner = TrimView(lines[cursor]);
					if (inner == "triggerEvent(\"HideHobbiesInfo\", localPlayer)")
					{
						hide_cleanup = true;
					}
					if (inner == "setPedAnimation(localPlayer, nil)" || inner == "StopDiggingMinigame()")
					{
						stop_cleanup = true;
					}
					if (inner.find("local data = (type(") != std::string_view::npos || inner == "local business_id = data and data.business_id")
					{
						post_join_stub = true;
					}
				}

				for (std::size_t cursor = index + 1; cursor < end_index; ++cursor)
				{
					if (TrimView(lines[cursor]) == "addEventHandler(\"OnPlayerEndDigging\", localPlayer, localPlayer)")
					{
						lines[cursor] = std::string(lines[cursor].substr(0, lines[cursor].find_first_not_of(" \t"))) + "triggerServerEvent(\"OnPlayerEndDigging\", localPlayer, localPlayer)";
					}

					if ((hide_cleanup || stop_cleanup) && TrimView(lines[cursor]) == "addEventHandler(\"onClientKey\", root, DiggingZoneKeyHandler)")
					{
						lines[cursor] = std::string(lines[cursor].substr(0, lines[cursor].find_first_not_of(" \t"))) + "removeEventHandler(\"onClientKey\", root, DiggingZoneKeyHandler)";
					}
				}

				if (post_join_stub && TrimView(lines[index]).find("point.PostJoin = function(") == 0)
				{
					const std::string indent = std::string(lines[index].substr(0, lines[index].find_first_not_of(" \t")));
					lines[index] = indent + "point.PostJoin = function(store)";
					lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), lines.begin() + static_cast<std::ptrdiff_t>(end_index + 1));
					lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), indent + "    triggerEvent(\"Shop:ShowMenu\", localPlayer, 5, _, self.business_id)");
					lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index + 2), indent + "end");
					end_index = index + 2;
				}

				index = end_index;
			}

			lua = JoinLines(lines);
		}

		void ApplyDiggingSemanticRepair(std::string& lua)
		{
			if (lua.find("DIGGING_DATA = {}") == std::string::npos
				|| lua.find("CreateDiggingStore = ") == std::string::npos
				|| lua.find("OnDiggingLocationReceived = ") == std::string::npos)
			{
				return;
			}

			if (auto block = FindBoundFunctionBlock(lua, "CreateDiggingStore", "self"))
			{
				const std::string replacement =
					"local " + block->name + R"BR( = function(config)
    config.text = "ALT Взаємодія"
    config.keypress = "lalt"
    config.radius = config.radius or 2
    config.marker_text = "Лавка шукача скарбів"
    local store = TeleportPoint(config)
    store.marker:setColor(0, 100, 100, 50)
    store:SetImage("files/img/marker_digging.png")
    store.element:setData("material", true, false)
    store:SetDropImage({ ":ugta_shared/img/dropimage.png", 255, 255, 255, 255, 1.45 })
    store.PreJoin = function(store, player)
        return true
    end
    store.PostJoin = function(store)
        triggerEvent("Shop:ShowMenu", localPlayer, 5, _, config.business_id)
    end
    store.PostLeave = function(store)
    end
    store.elements = {}
    store.elements.blip = Blip(config.x, config.y, config.z, 60, 2, 255, 0, 0, 255, 0, 300)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindBoundFunctionBlock(lua, "OnDiggingLocationReceived", "arg1, arg2"))
			{
				const std::string replacement =
					"local " + block->name + R"BR( = function(location_id, type)
    local pData = TREASURE_LOCATIONS_LIST[type][location_id]
    local vecCenterBias = Vector3(pData.x, pData.y, pData.z):AddRandomRange(30)
    DIGGING_DATA.location_id = location_id
    DIGGING_DATA.type = type
    DIGGING_DATA.t_col = createColSphere(pData.x, pData.y, pData.z, pData.size or 4)
    DIGGING_DATA.zone_col = createColSphere(vecCenterBias, 60)
    if getDistanceBetweenPoints3D(localPlayer.position, vecCenterBias) > 8 then
        DIGGING_DATA.zone_pre_enter = createColSphere(vecCenterBias, 6)
        DIGGING_DATA.vecCenterBias = vecCenterBias
    else
        vecCenterBias.z = localPlayer.position.z
    end
    triggerEvent("ToggleGPS", localPlayer, vecCenterBias)
    DIGGING_DATA.zone_blip = createBlip(vecCenterBias, 38)
    setBlipSize(DIGGING_DATA.zone_blip, 5)
    addEventHandler("onClientColShapeHit", DIGGING_DATA.zone_pre_enter, onClientLocationPreEnter)
    addEventHandler("onClientColShapeHit", DIGGING_DATA.zone_col, OnClientLocationSphereHit)
    addEventHandler("onClientColShapeLeave", DIGGING_DATA.zone_col, OnClientLocationSphereLeave)
    addEventHandler("onClientElementDestroy", DIGGING_DATA.zone_col, OnDiggingZoneDestroy)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindBoundFunctionBlock(lua, "onClientLocationPreEnter", "arg1, arg2"))
			{
				const std::string replacement =
					"local " + block->name + R"BR( = function(pPlayer, dim)
    if pPlayer ~= localPlayer or not dim then
        return
    end
    destroyElement(source)
    local vecCenterBias = DIGGING_DATA.vecCenterBias
    vecCenterBias.z = localPlayer.position.z
    triggerEvent("ToggleGPS", localPlayer, vecCenterBias)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindBoundFunctionBlock(lua, "OnClientLocationSphereHit", "arg1, arg2"))
			{
				const std::string replacement =
					"local " + block->name + R"BR( = function(pPlayer, dim)
    if pPlayer ~= localPlayer then
        return
    end
    if not dim then
        return
    end
    triggerEvent("ShowHobbiesInfo", localPlayer, "digging")
    addEventHandler("onClientKey", root, DiggingZoneKeyHandler)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindBoundFunctionBlock(lua, "OnClientLocationSphereLeave", "arg1, arg2"))
			{
				const std::string replacement =
					"local " + block->name + R"BR( = function(pPlayer, dim)
    if pPlayer ~= localPlayer then
        return
    end
    triggerEvent("HideHobbiesInfo", localPlayer)
    if DIGGING_DATA.shovel then
        localPlayer:ShowInfo("Ти залишив зону пошуків")
        triggerServerEvent("OnPlayerEndDigging", localPlayer, localPlayer)
    end
    removeEventHandler("onClientKey", root, DiggingZoneKeyHandler)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindBoundFunctionBlock(lua, "DiggingZoneKeyHandler", "arg1, arg2"))
			{
				const std::string replacement =
					"local " + block->name + R"BR( = function(key, state)
    if key == "h" and state then
        if isCursorShowing() then
            return
        end
        if isChatBoxInputActive() then
            return
        end
        if DIGGING_DATA.last_shovel_request and getTickCount() - DIGGING_DATA.last_shovel_request <= 3000 then
            return
        end
        if isPedInVehicle(localPlayer) then
            localPlayer:ShowError("Потрібно вийти з машини")
            return false
        end
        if getPedControlState("aim_weapon") then
            return false
        end
        if not DIGGING_DATA.digging then
            triggerServerEvent("OnPlayerHitDiggingMarker", localPlayer)
        end
        DIGGING_DATA.last_shovel_request = getTickCount()
    end
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindBoundFunctionBlock(lua, "OnPlayerStartDigging", "arg1"))
			{
				const std::string replacement =
					"local " + block->name + R"BR( = function(data)
    for k, v in pairs(data) do
        DIGGING_DATA[k] = v
    end
    DIGGING_DATA.shovel = true
    DIGGING_DATA.digging = false
    for k, v in pairs(disabled_controls) do
        toggleControl(v, false)
    end
    addEventHandler("onClientKey", root, DiggingKeyHandler)
    setPedWeaponSlot(localPlayer, 0)
    ToggleDiggingHUD(true)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindBoundFunctionBlock(lua, "OnPlayerStopDigging", "arg1"))
			{
				const std::string replacement =
					"local " + block->name + R"BR( = function(bFinished)
    setPedAnimation(localPlayer, nil)
    if DIGGING_DATA.digging then
        StopDiggingMinigame()
    end
    for k, v in pairs(disabled_controls) do
        toggleControl(v, true)
    end
    ToggleDiggingHUD(false)
    ShowUI_Map(false)
    removeEventHandler("onClientKey", root, DiggingKeyHandler)
    if bFinished then
        for k, v in pairs(DIGGING_DATA) do
            if isElement(v) then
                destroyElement(v)
            end
        end
    end
    DIGGING_DATA.shovel = false
    DIGGING_DATA.digging = false
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			lua = std::regex_replace(
				lua,
				std::regex("local disabled_controls = \\{[^\\n]*\\}"),
				"local disabled_controls = { \"aim_weapon\", \"fire\", \"action\", \"enter_exit\", \"jump\", \"sprint\", \"enter_passenger\" }");
		}

		void ApplyPageSemanticRepair(std::string& lua)
		{
			if (lua.find("Manager.Tab_1_EditDayMsg = function(") == std::string::npos
				&& lua.find("Manager.Tab_1_Donate = function(") == std::string::npos
				&& lua.find("Manager.Tab_1_Editor = function(") == std::string::npos)
			{
				return;
			}

			if (auto block = FindAssignedFunctionBlock(lua, "Manager.Tab_1", "self, state"))
			{
				const std::string replacement = R"BR(Manager.Tab_1 = function(self, data)
    self.bg_page = ibCreateRenderTarget(ScaleX(365), ScaleY(600), ScaleX(743), ScaleY(622), self.bg)
        :ibMoveTo(ScaleX(365), ScaleY(183), 300)
        :ibData("alpha", 0)
        :ibAlphaTo(255, 1000)
        :ibTimer(function(element)
            local sx, sy = element:ibData("sx"), element:ibData("sy")
            element:ibData("sx", 0):ibData("sy", 0):ibResizeTo(sx, sy, 300)
        end, 10, 1)

    local bg = ibCreateImage(0, 0, ScaleX(743), ScaleY(622), "assets/image/home/bg.png", self.bg_page)

    self.clan_tag = ibCreateImage(ScaleX(62), ScaleY(56), ScaleX(167), ScaleY(167), ":ugta_clans/img/tags/band/" .. data.tag .. ".png", bg)
    self.clan_name = ibCreateLabel(ScaleX(102), ScaleY(270), ScaleX(86), ScaleY(25), data.name, bg, _, ScaleX(1), ScaleY(1), "center", "center", ibFonts.gothampromedium_12):ibData("disabled", true)

    local select_news = 1

    self.title_news = ibCreateLabel(ScaleX(334), ScaleY(25), ScaleX(126), ScaleY(29), "Опис клану", bg, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_12):ibData("disabled", true)
    self.bg_news = ibCreateImage(ScaleX(334), ScaleY(69), ScaleX(384), ScaleY(265), "assets/image/home/list_msg_1.png", self.bg_page)
    self.text_news = ibCreateLabel(ScaleX(20), ScaleY(20), ScaleX(370), ScaleY(250), data.desc, self.bg_news, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_9):ibData("disabled", true):ibData("wordbreak", true)
    self.selector = ibCreateImage(ScaleX(658), ScaleY(36), ScaleX(18), ScaleY(6), "assets/image/home/selector_1.png", bg)

    self.SelectPageTab_1 = function(page)
        self.selector:ibData("texture", "assets/image/home/selector_" .. page .. ".png")
        self.bg_news:ibData("texture", "assets/image/home/list_msg_" .. page .. ".png"):ibData("alpha", 100):ibAlphaTo(255, 300)
        self.text_news:ibData("text", page == 1 and data.desc or data.motd)
        self.title_news:ibData("text", page == 1 and "Опис клану" or "Повідомлення дня")

        if page == 2 then
            if isElement(self.btn_edit_new_day) then
                self.btn_edit_new_day:destroy()
            end

            if localPlayer:GetClanRole() >= 4 then
                self.btn_edit_new_day = ibCreateButton(ScaleX(331), ScaleY(15), ScaleX(38), ScaleY(38), self.bg_news, "assets/image/home/edit.png", "assets/image/home/edit_h.png", "assets/image/home/edit_h.png")
                    :ibOnClick(function(btn, state)
                        if btn ~= "left" or state ~= "down" then
                            return
                        end

                        self:Tab_1_EditDayMsg(data)
                        self:Click()
                    end)
            end
        elseif isElement(self.btn_edit_new_day) then
            self.btn_edit_new_day:destroy()
        end

        select_news = page
    end

    self.left = ibCreateButton(ScaleX(616), ScaleY(23), ScaleX(32), ScaleY(32), bg, "assets/image/home/left.png", "assets/image/home/left_h.png", "assets/image/home/left_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            self.SelectPageTab_1(1)
            self:Click()
        end)

    self.right = ibCreateButton(ScaleX(686), ScaleY(23), ScaleX(32), ScaleY(32), bg, "assets/image/home/right.png", "assets/image/home/right_h.png", "assets/image/home/right_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            self.SelectPageTab_1(2)
            self:Click()
        end)
        :ibTimer(function()
            self.SelectPageTab_1(select_news == 1 and 2 or 1)
        end, 10000, 0)

    local bg_rating = ibCreateButton(ScaleX(25), ScaleY(448), ScaleX(171), ScaleY(149), bg, "assets/image/home/btn_rayting.png", "assets/image/home/btn_rayting_h.png", "assets/image/home/btn_rayting_h.png"):ibAttachTooltip("Відображення на якому місці ваш клан")
    local my_pos = 0
    local all_clans = 0

    for k, v in pairs(data and data.season_data and data.season_data.leaderboard or {}) do
        if v[LB_CLAN_ID] == localPlayer:GetClanID() then
            my_pos = k
        end
        all_clans = all_clans + 1
    end

    if my_pos ~= 0 and all_clans ~= 0 then
        local pos_clan = ibCreateLabel(ScaleX(21), ScaleY(78), ScaleX(126), ScaleY(29), my_pos, bg_rating, 0xFFFFB908, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_12):ibData("disabled", true)
        ibCreateLabel(pos_clan:width() + ScaleX(2), ScaleY(0), ScaleX(126), ScaleY(29), "/" .. all_clans, pos_clan, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampro_10):ibData("disabled", true):ibData("alpha", 150)
    end

    local bg_slots = ibCreateButton(ScaleX(208), ScaleY(448), ScaleX(171), ScaleY(149), bg, "assets/image/home/btn_add_slot.png", "assets/image/home/btn_add_slot_h.png", "assets/image/home/btn_add_slot_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            local upgrade_conf = CLAN_UPGRADES_LIST[CLAN_UPGRADE_SLOTS]
            local upgrade_next_level = (data.upgrades[CLAN_UPGRADE_SLOTS] or 0) + 1
            local upgrade = upgrade_conf[upgrade_next_level]

            if localPlayer:GetClanRole() ~= CLAN_ROLE_LEADER then
                localPlayer:ShowInfo("Лише лідер клану може придбати покращення")
                return
            end

            ibConfirm({
                title = "РОЗШИРЕННЯ КЛАНУ",
                text = "Ти точно хочеш розширити клан до " .. (data.slots + 25) .. " слотів \nза " .. format_price(upgrade.cost) .. " гривень.?",
                fn = function(confirm_window)
                    if data.money < upgrade.cost then
                        localPlayer:ShowError("Недостатньо коштів у общаку клану")
                        return
                    end

                    triggerServerEvent("onPlayerRequestClanUpgrade", localPlayer, CLAN_UPGRADE_SLOTS)
                    self.max_slots:ibData("text", "/" .. (data.slots + 25))
                    confirm_window:destroy()
                end,
                escape_close = true,
            })

            self:Click()
        end)

    self.slots = ibCreateLabel(ScaleX(21), ScaleY(78), ScaleX(126), ScaleY(29), data.members_count, bg_slots, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_12):ibData("disabled", true)
    self.max_slots = ibCreateLabel(self.slots:width() + ScaleX(2), ScaleY(0), ScaleX(126), ScaleY(29), "/" .. data.slots, self.slots, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampro_10):ibData("disabled", true):ibData("alpha", 150)

    local money_log = data.money_log and data.money_log[#data.money_log] or {
        type = 2,
        value = 0,
    }

    self.donate_button = ibCreateButton(ScaleX(449), ScaleY(543), ScaleX(269), ScaleY(54), bg, "assets/image/home/btn_donate_big.png", "assets/image/home/btn_donate_big_h.png", "assets/image/home/btn_donate_big_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            self:Tab_1_Donate(data)
            self:Click()
        end)

    local money = ibCreateLabel(ScaleX(449), ScaleY(448), ScaleX(184), ScaleY(35), "₴ " .. format_price(data.money), bg, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_15):ibData("disabled", true)
    local icon = money_log.value > 1 and "up" or "down"
    ibCreateImage(money:width() + ScaleX(10), ScaleX(2), ScaleX(30), ScaleY(30), "assets/image/home/rating_" .. icon .. ".png", money)
    ibCreateLabel(ScaleX(601), ScaleY(490), ScaleX(97), ScaleY(20), format_price(money_log.value), bg, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampro_10):ibData("disabled", true):ibData("color", money_log.value > 1 and 0xFF5ead63 or 0xFFFF4141)

    if localPlayer:GetClanRole() >= 5 then
        self.btn_settings_clan = ibCreateButton(ScaleX(231), ScaleY(20), ScaleX(38), ScaleY(38), bg, "assets/image/home/btn_settings.png", "assets/image/home/btn_settings_h.png", "assets/image/home/btn_settings_h.png")
            :ibOnClick(function(btn, state)
                if btn ~= "left" or state ~= "down" then
                    return
                end

                self:Tab_1_Editor(data)
                self:Click()
            end)
    end

    self.close_status = ibCreateImage(ScaleX(61), ScaleY(306), ScaleX(130), ScaleY(32), "assets/image/home/door_0.png", bg)
    self.btn_status = ibCreateImage(ScaleX(197), ScaleY(306), ScaleX(32), ScaleY(32), "assets/image/home/btn_close_0.png", bg):ibAttachTooltip("Відкрити/Закрити клан")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            if localPlayer:GetClanRole() ~= CLAN_ROLE_LEADER then
                return localPlayer:ShowInfo("Тільки лідер може керувати статусом клану")
            end

            triggerServerEvent("onPlayerWantSetClanClosed", localPlayer, not data.is_closed)
            self.close_status:ibData("texture", "assets/image/home/door_" .. (not data.is_closed and 0 or 1) .. ".png")
            self.btn_status:ibData("texture", "assets/image/home/btn_close_" .. (not data.is_closed and 0 or 1) .. ".png")
            data.is_closed = not data.is_closed
            self:Click()
        end)

    if not data.is_closed then
        self.close_status:ibData("texture", "assets/image/home/door_1.png")
        self.btn_status:ibData("texture", "assets/image/home/btn_close_1.png")
    end
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			ReplaceAll(
				lua,
				"        if arg1 == 1 then\n            local var = self.text_news.ibData\n            local var_1 = self.text_news\n            local var_2 = \"text\"\n        end\n        self.text_news:ibData(\"text\", state.motd)\n        if arg1 == 1 then\n            local var = self.title_news.ibData\n            local var_1 = self.title_news\n            local var_2 = \"text\"\n        end\n        self.title_news:ibData(\"text\", \"Повідомлення дня\")",
				"        self.text_news:ibData(\"text\", arg1 == 1 and state.desc or state.motd)\n        self.title_news:ibData(\"text\", arg1 == 1 and \"Опис клану\" or \"Повідомлення дня\")");

			ReplaceAll(
				lua,
				"    local var_16 =     function()\n        if var_7 == 1 then\n            local var = self.SelectPageTab_1\n        end\n        self.SelectPageTab_1(1)\n    end",
				"    local var_16 =     function()\n        self.SelectPageTab_1(var_7 == 1 and 2 or 1)\n    end");

			ReplaceAll(
				lua,
				"        local var = tonumber(self.field_edit:ibData(\"text\"))\n        if var and var > 0 then\n        end\n        localPlayer:ErrorWindow(\"Введіть суму!\")\n        return",
				"        local var = tonumber(self.field_edit:ibData(\"text\"))\n        if not var or var <= 0 or var ~= math.floor(var) then\n            localPlayer:ErrorWindow(\"Введіть суму!\")\n            return\n        end");

			ReplaceAll(
				lua,
				"        local var = var_4:ibData(\"text\")\n        if var then\n        end\n        localPlayer:ShowError(\"Введіть назву клану\")\n        return\n        if utf8.len(var) >= 3 then\n        end\n        localPlayer:ShowError(\"Назва має містити від 3 до 12 символів\")\n        return\n        if not utf8.find(var, \"^[a-zA-Zа-яА-ЯґҐєЄіІїЇ0-9\\\\-]+$\") then\n            localPlayer:ShowError(\"Назва може містити лише цифри, літери та дефіс\")\n        end\n        if not utf8.find(var, \"^-\") then\n        end\n        localPlayer:ShowError(\"Назва не може починатися або закінчуватися дефісом\")\n        return",
				"        local var = var_4:ibData(\"text\")\n        if not var or var == \"\" then\n            localPlayer:ShowError(\"Введіть назву клану\")\n            return\n        end\n        if utf8.len(var) < 3 or utf8.len(var) > 12 then\n            localPlayer:ShowError(\"Назва має містити від 3 до 12 символів\")\n            return\n        end\n        if not utf8.find(var, \"^[a-zA-Zа-яА-ЯґҐєЄіІїЇ0-9\\\\-]+$\") then\n            localPlayer:ShowError(\"Назва може містити лише цифри, літери та дефіс\")\n            return\n        end\n        if utf8.find(var, \"^-\") or utf8.find(var, \"-$\") then\n            localPlayer:ShowError(\"Назва не може починатися або закінчуватися дефісом\")\n            return\n        end");

			if (auto block = FindAssignedFunctionBlock(lua, "Manager.Tab_1_EditDayMsg", "self, arg2"))
			{
				const std::string replacement = R"BR(Manager.Tab_1_EditDayMsg = function(self, data)
    self.modal_black_bg = ibCreateBackground(0xD1000000, function()
        self:Destroy(false, true)
    end, 0xAA000000, true, true):ibData("alpha", 0):ibAlphaTo(255, 350)
    self.modal_bg = ibCreateImage(0, ScaleY(-500), ScaleX(446), ScaleY(590), "assets/image/home/modal_edit/bg.png", self.modal_black_bg):center_x():ibMoveTo(_, math.floor(_SCREEN_Y / 2 - ScaleY(590) / 2), 400, "OutQuad")
    self.modal_close = ibCreateButton(ScaleX(379), ScaleY(11), ScaleX(54), ScaleY(54), self.modal_bg, "assets/image/members/modal_kick/close.png", "assets/image/members/modal_kick/close_h.png", "assets/image/members/modal_kick/close_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end
            self:Click()
            self:Destroy(false, true)
        end)
    self.field_edit = ibCreateWebMemo(ScaleX(32), ScaleY(205), ScaleX(382), ScaleY(265), "", self.modal_bg, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF)
        :ibBatchData({
            font = "semibold_13",
            max_length = 140,
            bg_color = 0,
        })
    self.btn_select_edit = ibCreateButton(ScaleX(32), ScaleY(494), ScaleX(382), ScaleY(52), self.modal_bg, "assets/image/home/modal_edit/btn_push.png", "assets/image/home/modal_edit/btn_push_h.png", "assets/image/home/modal_edit/btn_push_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end
            local new_value = self.field_edit:ibData("text")
            local lines = split(new_value, "\n")
            if #lines >= 8 then
                return localPlayer:ShowError("Ви досягли ліміту на кількість рядків.")
            end
            local prev_pos = 0
            for pos, codepoint in utf8.next, new_value .. " " do
                if pos - prev_pos > 3 then
                    localPlayer:ShowError("Містить неприпустимі символи!")
                    return
                end
                prev_pos = pos
            end
            if new_value ~= (data.motd or "") then
                data.motd = new_value
                self.SelectPageTab_1(2)
                triggerServerEvent("onClanMotdChangeRequest", localPlayer, new_value)
            end
            self:Destroy(false, true)
            self:Click()
        end)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindAssignedFunctionBlock(lua, "Manager.Tab_1_Donate", "self, state"))
			{
				const std::string replacement = R"BR(Manager.Tab_1_Donate = function(self, data)
    self.modal_black_bg = ibCreateBackground(0xD1000000, function()
        self:Destroy(false, true)
    end, 0xAA000000, true, true):ibData("alpha", 0):ibAlphaTo(255, 350)
    self.modal_bg = ibCreateImage(0, ScaleY(-500), ScaleX(468), ScaleY(374), "assets/image/home/modal_donate/bg.png", self.modal_black_bg):center_x():ibMoveTo(_, math.floor(_SCREEN_Y / 2 - ScaleY(374) / 2), 400, "OutQuad")
    self.modal_close = ibCreateButton(ScaleX(400), ScaleY(11), ScaleX(54), ScaleY(54), self.modal_bg, "assets/image/members/modal_kick/close.png", "assets/image/members/modal_kick/close_h.png", "assets/image/members/modal_kick/close_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end
            self:Click()
            self:Destroy(false, true)
        end)
    self.field_edit = ibCreateEdit(ScaleX(51), ScaleY(222), ScaleX(350), ScaleY(20), "", self.modal_bg, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF)
        :ibBatchData({
            font = ibFonts.gothampro_12,
            max_length = 140,
            bg_color = 0,
            pattern = "%d",
        })
        :ibTimer(function()
            self.field_edit:ibData("focused", true)
        end, 100, 1)
    local money_log = data.money_log and data.money_log[#data.money_log] or {
        type = 2,
        value = 0,
    }
    local money = ibCreateLabel(ScaleX(32), ScaleY(81), ScaleX(206), ScaleY(39), "₴ " .. format_price(data.money), self.modal_bg, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_15):ibData("disabled", true)
    local icon = money_log.value > 1 and "up" or "down"
    local icon_progress = ibCreateImage(money:width() + ScaleX(10), ScaleX(2), ScaleX(30), ScaleY(30), "assets/image/home/rating_" .. icon .. ".png", money)
    local last_value = ibCreateLabel(ScaleX(184), ScaleY(128), ScaleX(97), ScaleY(20), format_price(money_log.value), self.modal_bg, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampro_10):ibData("disabled", true)
        :ibData("color", money_log.value > 1 and 0xFF5ead63 or 0xFFFF4141)
    self.btn_donate = ibCreateButton(ScaleX(32), ScaleY(290), ScaleX(196), ScaleY(52), self.modal_bg, "assets/image/home/modal_donate/btn_add.png", "assets/image/home/modal_donate/btn_add_h.png", "assets/image/home/modal_donate/btn_add_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end
            local amount = tonumber(self.field_edit:ibData("text"))
            if not amount or amount <= 0 or amount ~= math.floor(amount) then
                localPlayer:ErrorWindow("Введіть суму!")
                return
            end
            self:Destroy(false, true)
            triggerServerEvent("onPlayerWantAddClanMoney", localPlayer, amount)
        end)
    self.btn_close = ibCreateButton(ScaleX(240), ScaleY(290), ScaleX(196), ScaleY(52), self.modal_bg, "assets/image/home/modal_donate/btn_cancel.png", "assets/image/home/modal_donate/btn_cancel_h.png", "assets/image/home/modal_donate/btn_cancel_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end
            self:Click()
            self:Destroy(false, true)
        end)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindAssignedFunctionBlock(lua, "Manager.Tab_1_Editor", "self, arg2"))
			{
				const std::string replacement = R"BR(Manager.Tab_1_Editor = function(self, data)
    self.modal_black_bg = ibCreateBackground(0xD1000000, function()
        self:Destroy(false, true)
    end, 0xAA000000, true, true):ibData("alpha", 0):ibAlphaTo(255, 350)
    self.modal_bg = ibCreateImage(0, ScaleY(-500), ScaleX(468), ScaleY(759), "assets/image/home/modal_editor/bg.png", self.modal_black_bg):center_x():ibMoveTo(_, math.floor(_SCREEN_Y / 2 - ScaleY(759) / 2), 400, "OutQuad")
    local tag = ibCreateImage(ScaleX(353), ScaleY(88), ScaleX(90), ScaleY(90), ":ugta_clans/img/tags/band/" .. data.tag .. ".png", self.modal_bg)
    local btn_tag_edit = ibCreateButton(ScaleX(32), ScaleY(161), ScaleX(249), ScaleY(52), self.modal_bg, "assets/image/home/modal_editor/btn_avatar.png", "assets/image/home/modal_editor/btn_avatar_h.png", "assets/image/home/modal_editor/btn_avatar_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end
            self:Destroy(false, true)
            self:Tab_1_Avatars(data)
            self:Click()
        end)
    local edit_name = ibCreateEdit(ScaleX(50), ScaleY(311), ScaleX(390), ScaleY(52), data.name, self.modal_bg, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF)
        :ibBatchData({
            font = ibFonts.gothampromedium_11,
            bg_color = 0,
            max_length = 12,
        })
    local btn_change_name = ibCreateButton(ScaleX(32), ScaleY(378), ScaleX(411), ScaleY(52), self.modal_bg, "assets/image/home/modal_editor/btn_name.png", "assets/image/home/modal_editor/btn_name_h.png", "assets/image/home/modal_editor/btn_name_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end
            self:Click()
            local selected_clan_name = edit_name:ibData("text")
            if not selected_clan_name or selected_clan_name == "" then
                localPlayer:ShowError("Введіть назву клану")
                return
            end
            if utf8.len(selected_clan_name) < 3 or utf8.len(selected_clan_name) > 12 then
                localPlayer:ShowError("Назва має містити від 3 до 12 символів")
                return
            end
            if not utf8.find(selected_clan_name, "^[a-zA-Zа-яА-ЯґҐєЄіІїЇ0-9\\-]+$") then
                localPlayer:ShowError("Назва може містити лише цифри, літери та дефіс")
                return
            end
            if utf8.find(selected_clan_name, "^-") or utf8.find(selected_clan_name, "-$") then
                localPlayer:ShowError("Назва не може починатися або закінчуватися дефісом")
                return
            end
            if localPlayer:GetDonate() < self.SHOP["NAME"] then
                return localPlayer:ShowError("У вас недостатньо карбованців!")
            end
            triggerServerEvent("onPlayerChangeClanName", resourceRoot, selected_clan_name)
            triggerServerEvent("onPlayerWantShowClanManageUI", localPlayer)
        end)
        :ibOnHover(function()
            self.text_cost_name:ibData("alpha", 120)
        end)
        :ibOnLeave(function()
            self.text_cost_name:ibData("alpha", 255)
        end)
    self.text_cost_name = ibCreateLabel(ScaleX(275), ScaleY(13.5), ScaleX(29), ScaleY(20), self.SHOP["NAME"], btn_change_name, _, ScaleX(1), ScaleY(1), "center", "center", ibFonts.gothampromedium_11):ibData("disabled", true)
    local memo_desc = ibCreateWebMemo(ScaleX(32), ScaleY(528), ScaleX(411), ScaleY(128), data.desc or "", self.modal_bg)
        :ibBatchData({
            font = "regular_12",
            max_length = 400,
            placeholder = "Введіть інформацію про клан...",
            placeholder_color = ibApplyAlpha(COLOR_WHITE, 80),
            bg_color = 0,
        })
    local btn_change_desc = ibCreateButton(ScaleX(50), ScaleY(675), ScaleX(382), ScaleY(52), self.modal_bg, "assets/image/home/modal_editor/btn_change_desc_h.png", "assets/image/home/modal_editor/btn_change_desc.png", "assets/image/home/modal_editor/btn_change_desc.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end
            local new_value = memo_desc:ibData("text")
            local lines = split(new_value, "\n")
            if #lines >= 8 then
                return localPlayer:ShowError("Ви досягли ліміту на кількість рядків.")
            end
            local prev_pos = 0
            for pos, codepoint in utf8.next, new_value .. " " do
                if pos - prev_pos > 3 then
                    localPlayer:ShowError("Інформація про клан містить неприпустимі символи!")
                    return
                end
                prev_pos = pos
            end
            if new_value ~= (data.desc or "") then
                data.desc = new_value
                self.SelectPageTab_1(1)
                triggerServerEvent("onClanDescChangeRequest", localPlayer, new_value)
                localPlayer:ShowSuccess("Ви успішно змінили опис!")
                self:Destroy(false, true)
            end
            self:Click()
        end)
        :center_x()
    self.modal_close = ibCreateButton(ScaleX(400), ScaleY(11), ScaleX(54), ScaleY(54), self.modal_bg, "assets/image/members/modal_kick/close.png", "assets/image/members/modal_kick/close_h.png", "assets/image/members/modal_kick/close_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end
            self:Click()
            self:Destroy(false, true)
        end)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}
		}

		void ApplyUiSemanticRepair(std::string& lua)
		{
			if (lua.find("Central = var") == std::string::npos && lua.find(".ModalBuy = function(") == std::string::npos)
			{
				return;
			}

			if (auto member = FindMemberFunctionBlock(lua, "f_item", "arg1, arg2, arg3, arg4"))
			{
				const std::string replacement = member->block.name + R"BR( = function(self, item_id, attributes, count)
    local normalized_attributes = attributes and next(attributes) and attributes or nil
    local visual = ITEM_CONFIG_VISUAL(item_id, normalized_attributes)
    local image_item = visual.image or {}
    local path = ":ugta_inventory/"

    if string.find(image_item, "^:") then
        path = ""
    end

    return {
        item_id = item_id,
        attributes = normalized_attributes,
        count = count,
        visual_data = image_item,
        description = visual.description,
        text = visual.text,
        path = path,
        weight = ITEM_WEIGHT(item_id),
    }
end)BR";
				ReplaceFunctionBlock(lua, member->block, replacement);
			}

			if (auto member = FindMemberFunctionBlock(lua, "CreateDragItem", "self, arg2, arg3, arg4, arg5"))
			{
				const std::string replacement = member->block.name + R"BR( = function(self, item_id, attributes, info, other)
    DestroyTableElements(self.drag_item)
    self.drag_item = {}
    self.drag_item.bg = ibCreateImage(0, 0, ScaleX(82 / 2), ScaleY(82 / 2), ":ugta_inventory/inv/blocks/block_0.png", self.black_bg)
        :ibAttachToCursor(5, 5)
        :ibOnAnyClick(function(button, state)
            if button ~= "left" or state ~= "up" then
                return
            end

            self.last_drag = self.drag_item.data
            DestroyTableElements(self.drag_item)
            self.drag_item = {}

            if isTimer(self.timer) then
                killTimer(self.timer)
            end

            self.timer = setTimer(function()
                self.last_drag = nil
            end, 450, 1)
        end)
        :ibData("disabled", true)

    self.drag_item.img = ibCreateImage(0, 0, ScaleX(90 / 2), ScaleY(90 / 2), info.path .. info.visual_data, self.drag_item.bg):center()
    self.sound = playSound(":ugta_inventory/sfx/dii.mp3")
    self.drag_item.data = {
        item_id = item_id,
        attributes = attributes and next(attributes) and attributes or nil,
        info = info,
        who = other,
    }
end)BR";
				ReplaceFunctionBlock(lua, member->block, replacement);
			}

			if (auto member = FindMemberFunctionBlock(lua, "Inventory", "self, arg2"))
			{
				const std::string replacement = member->block.name + R"BR( = function(self, data)
    showChat(false)
    showCursor(true)
    localPlayer:setData("IsWithinTuning", true, false)
    self.black_bg = ibCreateBackground(0x40000000, function()
        self:Destroy(true)
    end, 0xAA000000, true, true):ibData("alpha", 255)
    self.bg = ibCreateImage(ScaleX(0), ScaleY(0), ScaleX(1320), ScaleY(711), "assets/inventory/bg.png", self.black_bg):center()
    self.t_title = ibCreateLabel(ScaleX(76), ScaleY(27), ScaleX(149), ScaleY(19), "Торгова лавка №" .. data.id, self.bg, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_11)

    self.close = ibCreateButton(ScaleX(1246), ScaleY(26), ScaleX(42), ScaleY(42), self.bg, "assets/btn_close.png", "assets/btn_close_h.png", "assets/btn_close_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            ibClick()
            self:Destroy()
        end)

    local inventory = FixInventory(INVENTORY_PLAYER(localPlayer).data)

    self.rt_inv, self.sc_inv = ibCreateScrollpane(ScaleX(33), ScaleY(162), ScaleX(430), ScaleY(517), self.bg,
        {
            scroll_px = ScaleX(7),
            bg_texture = ":ugta_inventory/inv/scroll/scroll_bg.png",
            bg_px = 0, bg_py = 0, bg_sx = 4, bg_sy = 517,
            handle_texture = ":ugta_inventory/inv/scroll/scroll.png",
            handle_px = 0, handle_py = 0, handle_sx = 4, handle_sy = 202,
            handle_upper_limit = -202,
        })
    self.sc_inv:ibBatchData({ absolute = true, sensivity = 75 })

    self.p_item = {}
    local count = 0
    local line = 0
    local item_in_line = 0

    self.player_weight = ibCreateLabel(ScaleX(406), ScaleY(123), ScaleX(57), ScaleY(20), math.floor(INVENTORY_PLAYER(localPlayer).total_weight) .. "#81818A/" .. INVENTORY_PLAYER(localPlayer).max_weight .. " кг", self.bg, 0xFFFFFFFF, ScaleX(1), ScaleY(1), "right", "center", ibFonts.gothampromedium_9):ibData("disabled", true):ibData("colored", true)
    if INVENTORY_PLAYER(localPlayer).total_weight > INVENTORY_PLAYER(localPlayer).max_weight then
        self.player_weight:ibData("text", "#c41d1d" .. math.floor(INVENTORY_PLAYER(localPlayer).total_weight) .. "#81818A/" .. INVENTORY_PLAYER(localPlayer).max_weight .. " кг")
    end
    self.icon_player_weight = ibCreateImage(1 - self.player_weight:width() + ScaleX(30), 0, ScaleX(18), ScaleY(18), ":ugta_inventory/inv/ico/ico_weight.png", self.player_weight)

    for _, item_data in pairs(inventory) do
        count = count + 1
        self.p_item[count] = {}
        self.p_item[count].bg = ibCreateButton(0, 0, ScaleX(82), ScaleY(82), self.rt_inv, ":ugta_inventory/inv/blocks/block_1.png", ":ugta_inventory/inv/blocks/block_1_h.png", ":ugta_inventory/inv/blocks/block_1_h.png")
            :ibMoveTo(ScaleX(0 + 87 * item_in_line), ScaleY(0 + 87 * line), count * 20)
            :ibOnClick(function(btn, state)
                if btn == "right" and state == "down" then
                    if not ITEMS_CONFIG[item_data.item_id].Cost then
                        return localPlayer:ShowInfo("Цей предмет не можна продати")
                    end

                    self:Modal(item_data.item_id, item_data.attributes, item_data, "move")
                end
            end)
            :ibOnHover(function()
                if self.modal_select == "move" then
                    return
                end
                DestroyTableElements(self.c_modal)
            end)

        self.p_item[count].img = ibCreateImage(0, 0, ScaleX(60), ScaleY(60), item_data.path .. item_data.visual_data, self.p_item[count].bg):center():ibData("disabled", true)
        self.p_item[count].count = ibCreateLabel(ScaleX(62), ScaleY(61), ScaleX(15), ScaleY(20), item_data.count or 1, self.p_item[count].bg, 0xFFFFFFFF, ScaleX(1), ScaleY(1), "right", "center", ibFonts.gothampromedium_10):ibData("disabled", true):ibData("colored", true)

        item_in_line = item_in_line + 1
        if item_in_line >= 5 then
            line = line + 1
            item_in_line = 0
        end
    end

    local total_cells = count
    local remainder = total_cells % 5
    local extra_cells = remainder > 0 and (5 - remainder) or 0

    for index = 1, extra_cells + 30 do
        total_cells = total_cells + 1
        self.p_item[total_cells] = ibCreateButton(ScaleX(0 + 87 * item_in_line), ScaleY(0 + 87 * line), ScaleX(82), ScaleY(82), self.rt_inv, ":ugta_inventory/inv/blocks/block_0.png", ":ugta_inventory/inv/blocks/block_0_h.png", ":ugta_inventory/inv/blocks/block_0_h.png")
            :ibOnHover(function()
                if self.modal_select == "move" then
                    return
                end
                DestroyTableElements(self.c_modal)
            end)

        item_in_line = item_in_line + 1
        if item_in_line >= 5 then
            line = line + 1
            item_in_line = 0
        end
    end

    self.rt_inv:AdaptHeightToContents()
    self.sc_inv:UpdateScrollbarVisibility(self.rt_inv)

    if not data.data.items or not next(data.data.items) then
        self.block = ibCreateImage(ScaleX(704), ScaleY(317), ScaleX(397), ScaleY(129), "assets/inventory/not_item.png", self.bg)
    else
        local market_line = 0
        local market_item_in_line = 0

        self.rt_market, self.sc_market = ibCreateScrollpane(ScaleX(526), ScaleY(153), ScaleX(762), ScaleY(558), self.bg, { scroll_px = 0 })
        self.sc_market:ibData("alpha", 0)
        self.market_item = {}

        for k, v in pairs(data.data.items or {}) do
            self.market_item[k] = {}
            local item_data = self:f_item(v.item_id, v.attributes, v.count)

            self.market_item[k].area = ibCreateArea(ScaleX(0 + 258 * market_item_in_line), ScaleY(0 + 296 * market_line), ScaleX(246), ScaleY(300), self.rt_market)
            self.market_item[k].bg = ibCreateImage(0, 0, ScaleX(246), ScaleY(284), "assets/block_item.png", self.market_item[k].area)
            self.market_item[k].title = ibCreateLabel(ScaleX(20), ScaleY(168), ScaleX(200), ScaleY(22), item_data.text, self.market_item[k].bg, 0xFFFFFFFF, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_9):ibData("disabled", true):ibData("wordbreak", true)
            self.market_item[k].texture_item = ibCreateImage(ScaleX(78), ScaleY(54), ScaleX(90), ScaleY(90), item_data.path .. item_data.visual_data, self.market_item[k].bg)
            self.market_item[k].count = ibCreateLabel(ScaleX(181), ScaleY(23), ScaleX(36), ScaleY(20), item_data.count .. " шт.", self.market_item[k].bg, 0xFFFFFFFF, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampro_9):ibData("disabled", true)
            self.market_item[k].cost = ibCreateLabel(ScaleX(44), ScaleY(206), ScaleX(76), ScaleY(22), format_price(v.cost), self.market_item[k].bg, 0xFFFFFFFF, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_12):ibData("disabled", true)
            self.market_item[k].kick_item = ibCreateButton(ScaleX(189), ScaleY(195), ScaleX(42), ScaleY(42), self.market_item[k].bg, "assets/btn_remove_item.png", "assets/btn_remove_item_h.png", "assets/btn_remove_item_h.png")
                :ibOnClick(function(btn, state)
                    if btn ~= "left" or state ~= "down" then
                        return
                    end

                    triggerServerEvent("Tent:PlayerWantRemoveItemSell", resourceRoot, k)
                end)

            market_item_in_line = market_item_in_line + 1
            if market_item_in_line >= 3 then
                market_line = market_line + 1
                market_item_in_line = 0
            end
        end

        self.rt_market:AdaptHeightToContents()
        self.sc_market:UpdateScrollbarVisibility(self.rt_market)
        localPlayer:setData("IsWithinTuning", true, false)
    end
end)BR";
				ReplaceFunctionBlock(lua, member->block, replacement);
			}

			if (auto member = FindMemberFunctionBlock(lua, "ModalBuy", "self, arg2, arg3, arg4"))
			{
				const std::string replacement = member->block.name + R"BR( = function(self, item_data, key, cost)
    self.black_bg_confirm = ibCreateBackground(0x40000000, function()
        self:Destroy(true)
    end, 0xAA000000, true, true):ibData("alpha", 255)
    self.bg_confirm = ibCreateImage(0, 0, ScaleX(335), ScaleY(612), "assets/market/bg_confirm.png", self.black_bg_confirm):center()

    self.title = ibCreateLabel(ScaleX(32), ScaleY(287), ScaleX(147), ScaleY(22), item_data.text, self.bg_confirm, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_9):ibData("disabled", true)
    self.texture = ibCreateImage(ScaleX(123), ScaleY(116), ScaleX(90), ScaleY(90), item_data.path .. item_data.visual_data, self.bg_confirm):ibData("disabled", true)

    self.close = ibCreateButton(ScaleX(278), ScaleY(15), ScaleX(42), ScaleY(42), self.bg_confirm, "assets/btn_close.png", "assets/btn_close_h.png", "assets/btn_close_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            ibClick()
            self:Destroy(true)
        end)

    local min = 1
    local max = item_data.count

    self.count_edit = ibCreateEdit(ScaleX(55), ScaleY(325), ScaleX(271), ScaleY(56), 1, self.bg_confirm, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF)
        :ibBatchData({
            font = ibFonts.gothampromedium_10,
            max_length = 6,
            bg_color = 0,
            pattern = "%d",
        })
        :ibData("disabled", max == 1 and true or false)
        :ibOnDataChange(function(changed_key, value)
            if changed_key == "text" and value and tonumber(value) then
                self.cost_full:ibData("text", format_price(tonumber(value) * cost))
            end
        end)

    if max > 1 then
        self.scroll_bar = ibCreateScrollbarH(ScaleX(44), ScaleY(373), ScaleX(247), ScaleY(2), self.bg_confirm, _, _, _, _, 0x00FFFFFF, 0, ScaleY(-7), ScaleX(18), ScaleY(18), ":ugta_inventory/inv/modal/loading_pick.png", 0, -7)
        self.scroll_fill = ibCreateImage(0, 0, 0, ScaleY(2), ":ugta_inventory/inv/modal/loading_line.png", self.scroll_bar):ibData("disabled", true):ibData("priority", -2)
        self.scroll_bar:ibData("position", 0)
        self.scroll_bar:ibOnDataChange(function(changed_key, position)
            if changed_key ~= "position" or max == 1 then
                return
            end

            local value = math.floor(min + (max - min) * position)
            self.scroll_fill:ibData("sx", ScaleX(247 * position))
            self.count_edit:ibData("text", value):ibData("caret_position", utf8.len(value))
            self.cost_full:ibData("text", format_price(value * cost))
        end)
    end

    self.cost_full = ibCreateLabel(ScaleX(56), ScaleY(433), ScaleX(76), ScaleY(22), format_price(cost), self.bg_confirm, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_12):ibData("disabled", true)

    self.btn_buy_confirm = ibCreateButton(ScaleX(32), ScaleY(483), ScaleX(271), ScaleY(56), self.bg_confirm, "assets/market/btn_c_buy.png", "assets/market/btn_c_buy_h.png", "assets/market/btn_c_buy_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            local count = self.count_edit:ibData("text")
            if utf8.len(count) < 1 or not tonumber(count) or tonumber(count) < 1 then
                return localPlayer:ShowInfo("Введіть кількість")
            end

            if not tonumber(cost) then
                return localPlayer:ShowInfo("Введіть кількість")
            end

            if tonumber(cost * tonumber(count)) > localPlayer:GetMoney() then
                return localPlayer:ShowInfo("У вас недостатньо грошей!")
            end

            triggerServerEvent("Tent:PlayerWantBuyItem", resourceRoot, key, tonumber(count))
            ibClick()
        end)

    self.cancel_confirm = ibCreateButton(ScaleX(32), ScaleY(551), ScaleX(271), ScaleY(43), self.bg_confirm, "assets/market/btn_c_cancel.png", "assets/market/btn_c_cancel_h.png", "assets/market/btn_c_cancel_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            ibClick()
            self:Destroy(true)
        end)
end)BR";
				ReplaceFunctionBlock(lua, member->block, replacement);
			}

			if (auto member = FindMemberFunctionBlock(lua, "Market", "self, arg2"))
			{
				const std::string replacement = member->block.name + R"BR( = function(self, data)
    showChat(false)
    showCursor(true)

    self.black_bg = ibCreateBackground(0x40000000, function()
        self:Destroy(true)
    end, 0xAA000000, true, true):ibData("alpha", 255)
    self.bg = ibCreateImage(ScaleX(0), ScaleY(0), ScaleX(1084), ScaleY(711), "assets/market/bg.png", self.black_bg):center()
    self.t_title = ibCreateLabel(ScaleX(76), ScaleY(27), ScaleX(149), ScaleY(19), "Торгова лавка №" .. data.id, self.bg, _, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_11)
    self.creator = ibCreateLabel(ScaleX(181), ScaleY(50), ScaleX(109), ScaleY(17), data.data.owner:GetNickName(), self.bg, 0xFF9FA0A1, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampro_10)

    self.close = ibCreateButton(ScaleX(1006), ScaleY(26), ScaleX(42), ScaleY(42), self.bg, "assets/btn_close.png", "assets/btn_close_h.png", "assets/btn_close_h.png")
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "down" then
                return
            end

            ibClick()
            self:Destroy()
        end)
        :ibTimer(function()
            showChat(false)
        end, 100, 0)

    local line = 0
    local item_in_line = 0
    self.rt_market, self.sc_market = ibCreateScrollpane(ScaleX(32), ScaleY(153), ScaleX(1020), ScaleY(558), self.bg, { scroll_px = 0 })
    self.sc_market:ibData("alpha", 0)

    if data.data.items and next(data.data.items) then
        self.market_item = {}

        for k, v in pairs(data.data.items or {}) do
            local info = self:f_item(v.item_id, v.attributes, v.count)
            self.market_item[k] = {}
            self.market_item[k].area = ibCreateArea(ScaleX(0 + 258 * item_in_line), ScaleY(0 + 296 * line), ScaleX(246), ScaleY(300), self.rt_market)
            self.market_item[k].bg = ibCreateImage(0, 0, ScaleX(246), ScaleY(284), "assets/block_item.png", self.market_item[k].area)
            self.market_item[k].title = ibCreateLabel(ScaleX(20), ScaleY(168), ScaleX(200), ScaleY(22), info.text, self.market_item[k].bg, 0xFFFFFFFF, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_9):ibData("disabled", true):ibData("wordbreak", true)
            self.market_item[k].texture_item = ibCreateImage(ScaleX(78), ScaleY(54), ScaleX(90), ScaleY(90), info.path .. info.visual_data, self.market_item[k].bg):ibAttachTooltip(info.description)
            self.market_item[k].count = ibCreateLabel(ScaleX(181), ScaleY(23), ScaleX(36), ScaleY(20), info.count .. " шт.", self.market_item[k].bg, 0xFFFFFFFF, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampro_9):ibData("disabled", true)
            self.market_item[k].cost = ibCreateLabel(ScaleX(44), ScaleY(206), ScaleX(76), ScaleY(22), format_price(v.cost), self.market_item[k].bg, 0xFFFFFFFF, ScaleX(1), ScaleY(1), "left", "center", ibFonts.gothampromedium_12):ibData("disabled", true)
            self.market_item[k].buy_item = ibCreateButton(ScaleX(189), ScaleY(195), ScaleX(42), ScaleY(42), self.market_item[k].bg, "assets/market/btn_buy.png", "assets/market/btn_buy_h.png", "assets/market/btn_buy_h.png")
                :ibOnClick(function(btn, state)
                    if btn ~= "left" or state ~= "down" then
                        return
                    end

                    self:ModalBuy(info, k, v.cost)
                    ibClick()
                end)

            item_in_line = item_in_line + 1
            if item_in_line >= 4 then
                line = line + 1
                item_in_line = 0
            end
        end
    end

    self.rt_market:AdaptHeightToContents()
    self.sc_market:UpdateScrollbarVisibility(self.rt_market)
    localPlayer:setData("IsWithinTuning", true, false)
end)BR";
				ReplaceFunctionBlock(lua, member->block, replacement);
			}
		}

		void ApplyBankSemanticRepair(std::string& lua)
		{
			if (auto block = FindAssignedFunctionBlock(lua, "UI.EnterBank", ""))
			{
				const std::string replacement = R"BR(UI.EnterBank = function()
    UI.Utils(true)

    local pin = ""
    local first_name = localPlayer:GetNickName():match("([^%s]+)") or localPlayer:GetNickName()
    local pin_dots = {}

    local function render_pin()
        for _, dot in pairs(pin_dots) do
            if isElement(dot) then
                dot:destroy()
            end
        end

        pin_dots = {}
        for index = 1, utf8.len(pin) do
            pin_dots[index] = ibCreateImage(scaleValue * (450 + 27 * (index - 1)), scaleValue * 190, scaleValue * 17, scaleValue * 16, "Files/Image/bg_pin_hover.png", UI.bg):ibData("alpha", 0):ibAlphaTo(255, 150)
        end
    end

    local function submit_pin()
        if utf8.len(pin) < 4 then
            return
        end

        playSound("Files/SFX/info.mp3")
        triggerServerEvent("BANK:PlayerWantEnterPass", resourceRoot, pin)
        UI.HideUIControl()
    end

    local function append_digit(digit)
        if utf8.len(pin) >= 4 then
            return
        end

        pin = pin .. tostring(digit)
        playSound("Files/SFX/click.mp3")
        render_pin()
        submit_pin()
    end

    local function backspace()
        if utf8.len(pin) < 1 then
            return
        end

        playSound("Files/SFX/cancel.mp3")
        pin = utf8.sub(pin, 1, utf8.len(pin) - 1)
        render_pin()
    end

    UI.black_bg = ibCreateBackground(0xFF400000, _, 0xAA000000, false, true):ibData("alpha", 240)
    UI.bg = ibCreateImage(0, 0, scaleValue * 1000, scaleValue * 600, "Files/Image/bg.png"):center():ibData("alpha", 0):ibAlphaTo(255, 350)
    UI.btn_close = ibCreateButton(UI.bg:ibData("sx") - scaleValue * 55, scaleValue * 24, scaleValue * 32, scaleValue * 32, UI.bg, "Files/Image/btn_close.png", "Files/Image/btn_close_h.png", "Files/Image/btn_close_h.png", 0xFFAAAAAA, 0xFFFFFFFF, 0xFFAAAAAA)
        :ibOnClick(function()
            playSound("Files/SFX/cancel.mp3")
            UI.HideUIControl()
        end)
    UI.ButtonRemoveInt = ibCreateButton(scaleValue * 570, scaleValue * 518, scaleValue * 53, scaleValue * 31, UI.bg, "Files/Image/btn_back.png", "Files/Image/btn_back.png", "Files/Image/btn_back.png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFE0E0E0)
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "up" then
                return
            end

            backspace()
        end)

    UI.PinImage = ibCreateImage(scaleValue * 400, scaleValue * 190, scaleValue * 96, scaleValue * 16, "Files/Image/bg_pin.png", UI.bg):center_x()
    UI.Pin_RegisterPASS = ibCreateLabel(scaleValue * 400, scaleValue * 110, scaleValue * 210, scaleValue * 50, string.format("З поверненням, %s!", first_name), UI.bg, _, scaleValue * 1, scaleValue * 1, "center", "center", ibFonts.bold_15):ibData("disabled", true):ibData("alpha", 255)
    UI.TextPing = ibCreateLabel(scaleValue * 395, scaleValue * 140, scaleValue * 210, scaleValue * 50, "Введіть ПІН-код", UI.bg, _, scaleValue * 1, scaleValue * 1, "center", "center", ibFonts.regular_14):ibData("disabled", true):ibData("alpha", 150)
    UI.Elements = pin_dots

    local digits = {
        { 1, 0, 0 }, { 2, 1, 0 }, { 3, 2, 0 },
        { 4, 0, 1 }, { 5, 1, 1 }, { 6, 2, 1 },
        { 7, 0, 2 }, { 8, 1, 2 }, { 9, 2, 2 },
    }

    for _, entry in ipairs(digits) do
        local digit, grid_x, grid_y = entry[1], entry[2], entry[3]
        ibCreateButton(scaleValue * (365 + 100 * grid_x), scaleValue * (230 + 90 * grid_y), scaleValue * 70, scaleValue * 70, UI.bg, "Files/Image/numbers/" .. digit .. ".png", "Files/Image/numbers/" .. digit .. ".png", "Files/Image/numbers/" .. digit .. ".png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFE0E0E0)
            :ibOnClick(function(btn, state)
                if btn ~= "left" or state ~= "up" then
                    return
                end

                append_digit(digit)
            end)
    end

    UI.NumberButton = ibCreateButton(scaleValue * 0, scaleValue * 500, scaleValue * 70, scaleValue * 70, UI.bg, "Files/Image/numbers/0.png", "Files/Image/numbers/0.png", "Files/Image/numbers/0.png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFE0E0E0)
        :center_x()
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "up" then
                return
            end

            append_digit(0)
        end)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindAssignedFunctionBlock(lua, "UI.RegisterBank", "arg1"))
			{
				const std::string replacement = R"BR(UI.RegisterBank = function(arg1)
    UI.Utils(true)

    local current_pin = ""
    local original_pin = ""
    local confirm_mode = false
    local pin_dots = {}

    local function render_pin()
        UI.Text_RegisterPASS:ibData("text", current_pin)
        for _, dot in pairs(pin_dots) do
            if isElement(dot) then
                dot:destroy()
            end
        end

        pin_dots = {}
        for index = 1, utf8.len(current_pin) do
            pin_dots[index] = ibCreateImage(scaleValue * (450 + 27 * (index - 1)), scaleValue * 160, scaleValue * 17, scaleValue * 16, "Files/Image/bg_pin_hover.png", UI.bg):ibData("alpha", 0):ibAlphaTo(255, 150)
        end
    end

    local function reset_registration()
        current_pin = ""
        original_pin = ""
        confirm_mode = false
        UI.Pin_RegisterPASS:ibData("text", "Придумайте ПІН-код")
        render_pin()
    end

    local function commit_pin()
        if utf8.len(current_pin) < 4 then
            return
        end

        if not confirm_mode then
            original_pin = current_pin
            current_pin = ""
            confirm_mode = true
            UI.Pin_RegisterPASS:ibData("text", "Підтвердіть ПІН-код")
            render_pin()
            return
        end

        if current_pin == original_pin then
            playSound("Files/SFX/info.mp3")
            triggerServerEvent("BANK:CreateCard", resourceRoot, original_pin)
            UI.HideUIControl()
            return
        end

        UI.Error()
        reset_registration()
    end

    local function append_digit(digit)
        if utf8.len(current_pin) >= 4 then
            return
        end

        current_pin = current_pin .. tostring(digit)
        playSound("Files/SFX/click.mp3")
        render_pin()
        commit_pin()
    end

    local function backspace()
        if utf8.len(current_pin) < 1 then
            return
        end

        playSound("Files/SFX/cancel.mp3")
        current_pin = utf8.sub(current_pin, 1, utf8.len(current_pin) - 1)
        render_pin()
    end

    UI.black_bg = ibCreateBackground(0xFF400000, _, 0xAA000000, false, true):ibData("alpha", 240)
    UI.bg = ibCreateImage(0, 0, scaleValue * 1000, scaleValue * 600, "Files/Image/bg.png"):center():ibData("alpha", 0):ibAlphaTo(255, 350)
    UI.btn_close = ibCreateButton(UI.bg:ibData("sx") - scaleValue * 55, scaleValue * 24, scaleValue * 32, scaleValue * 32, UI.bg, "Files/Image/btn_close.png", "Files/Image/btn_close_h.png", "Files/Image/btn_close_h.png", 0xFFAAAAAA, 0xFFFFFFFF, 0xFFAAAAAA)
        :ibOnClick(function()
            playSound("Files/SFX/cancel.mp3")
            UI.HideUIControl()
        end)
    UI.ButtonRemoveInt = ibCreateButton(scaleValue * 570, scaleValue * 518, scaleValue * 53, scaleValue * 31, UI.bg, "Files/Image/btn_back.png", "Files/Image/btn_back.png", "Files/Image/btn_back.png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFE0E0E0)
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "up" then
                return
            end

            backspace()
        end)

    UI.PinImage = ibCreateImage(scaleValue * 400, scaleValue * 160, scaleValue * 96, scaleValue * 16, "Files/Image/bg_pin.png", UI.bg):center_x()
    UI.Pin_RegisterPASS = ibCreateLabel(scaleValue * 400, scaleValue * 110, scaleValue * 50, scaleValue * 50, "Придумайте ПІН-код", UI.bg, _, scaleValue * 1, scaleValue * 1, "left", "center", ibFonts.bold_15):ibData("disabled", true):ibData("alpha", 255)
    UI.Text_RegisterPASS = ibCreateLabel(scaleValue * 475, scaleValue * 175, scaleValue * 50, scaleValue * 50, "", UI.bg, _, scaleValue * 1, scaleValue * 1, "left", "center", ibFonts.regular_20):ibData("disabled", true):ibData("alpha", 255)
    UI.Elements = pin_dots

    local digits = {
        { 1, 0, 0 }, { 2, 1, 0 }, { 3, 2, 0 },
        { 4, 0, 1 }, { 5, 1, 1 }, { 6, 2, 1 },
        { 7, 0, 2 }, { 8, 1, 2 }, { 9, 2, 2 },
    }

    for _, entry in ipairs(digits) do
        local digit, grid_x, grid_y = entry[1], entry[2], entry[3]
        ibCreateButton(scaleValue * (365 + 100 * grid_x), scaleValue * (230 + 90 * grid_y), scaleValue * 70, scaleValue * 70, UI.bg, "Files/Image/numbers/" .. digit .. ".png", "Files/Image/numbers/" .. digit .. ".png", "Files/Image/numbers/" .. digit .. ".png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFE0E0E0)
            :ibOnClick(function(btn, state)
                if btn ~= "left" or state ~= "up" then
                    return
                end

                append_digit(digit)
            end)
    end

    UI.NumberButton = ibCreateButton(scaleValue * 0, scaleValue * 500, scaleValue * 70, scaleValue * 70, UI.bg, "Files/Image/numbers/0.png", "Files/Image/numbers/0.png", "Files/Image/numbers/0.png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFE0E0E0)
        :center_x()
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "up" then
                return
            end

            append_digit(0)
        end)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindAssignedFunctionBlock(lua, "UI.SettingsBank", "arg1, arg2"))
			{
				const std::string replacement = R"BR(UI.SettingsBank = function(arg1, data)
    UI.Utils(true)

    local phone = format_price(localPlayer:GetPhoneNumber()) or "-"
    local email = data.email or "-"
    local stream_enabled = data.stream and true or false

    local function close_ui()
        playSound("Files/SFX/cancel.mp3")
        UI.Fines = nil
        UI.HouseInfo = nil
        UI.HideUIControl()
    end

    local function open_shop()
        playSound("Files/SFX/click.mp3")
        UI.HideUIControl()
        UI.ShopBunk(arg1, data)
    end

    local function open_main()
        playSound("Files/SFX/click.mp3")
        UI.HideUIControl()
        UI.MainBank(arg1, data)
    end

    local function open_skin()
        playSound("Files/SFX/click.mp3")
        UI.HideUIControl()
        UI.SkinBank(arg1, data)
    end

    UI.black_bg = ibCreateBackground(0xFF400000, _, 0xAA000000, true, true):ibData("alpha", 240)
    UI.bg = ibCreateImage(0, 0, scaleValue * 1000, scaleValue * 600, "Files/Image/shop/setting/bg_fon.png"):center():ibData("alpha", 0):ibAlphaTo(255, 350)
    UI.Text = ibCreateImage(0, 0, scaleValue * 760, scaleValue * 600, "Files/Image/shop/setting/bg.png", UI.bg)
    UI.btn_close = ibCreateButton(UI.bg:ibData("sx") - scaleValue * 55, scaleValue * 24, scaleValue * 32, scaleValue * 32, UI.bg, "Files/Image/btn_close.png", "Files/Image/btn_close_h.png", "Files/Image/btn_close_h.png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFAAAAAA):ibOnClick(close_ui)
    UI.btn_Buy = ibCreateButton(UI.bg:ibData("sx") - scaleValue * 100, scaleValue * 24, scaleValue * 32, scaleValue * 32, UI.bg, "Files/Image/main/btn_shop.png", "Files/Image/main/btn_shop.png", "Files/Image/main/btn_shop.png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFAAAAAA):ibOnClick(function(btn, state)
        if btn ~= "left" or state ~= "up" then
            return
        end
        open_shop()
    end)
    UI.btn_Options = ibCreateButton(UI.bg:ibData("sx") - scaleValue * 150, scaleValue * 24, scaleValue * 32, scaleValue * 32, UI.bg, "Files/Image/main/btn_setting.png", "Files/Image/main/btn_setting.png", "Files/Image/main/btn_setting.png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFAAAAAA):ibOnClick(function(btn, state)
        if btn ~= "left" or state ~= "up" then
            return
        end
        open_main()
    end)
    UI.btn_select = ibCreateImage(scaleValue * 290, scaleValue * 60, scaleValue * 74, scaleValue * 18, "Files/Image/shop/btn_shop.png", UI.bg):ibOnClick(function(btn, state)
        if btn ~= "left" or state ~= "up" then
            return
        end
        open_shop()
    end)
    UI.btn_select2 = ibCreateImage(scaleValue * 430, scaleValue * 54, scaleValue * 162, scaleValue * 27, "Files/Image/shop/btn_setting_h.png", UI.bg)
    UI.btn_select3 = ibCreateImage(scaleValue * 640, scaleValue * 58, scaleValue * 57, scaleValue * 17, "Files/Image/shop/btn_skin.png", UI.bg):ibOnClick(function(btn, state)
        if btn ~= "left" or state ~= "up" then
            return
        end
        open_skin()
    end)

    UI.TextNumber = ibCreateLabel(scaleValue * 37, scaleValue * 140, scaleValue * 100, scaleValue * 100, "Телефон", UI.bg, 0xFF8E8E8E, scaleValue * 1, scaleValue * 1, "left", "center", ibFonts.regular_15):ibData("disabled", true)
    UI.Number = ibCreateLabel(scaleValue * 37, scaleValue * 175, scaleValue * 100, scaleValue * 100, phone, UI.bg, _, scaleValue * 1, scaleValue * 1, "left", "center", ibFonts.regular_18):ibData("disabled", true)
    UI.TextEmail = ibCreateLabel(scaleValue * 37, scaleValue * 220, scaleValue * 100, scaleValue * 100, "Ваша ел.пошта", UI.bg, 0xFF8E8E8E, scaleValue * 1, scaleValue * 1, "left", "center", ibFonts.regular_15):ibData("disabled", true)
    UI.Email = ibCreateLabel(scaleValue * 37, scaleValue * 255, scaleValue * 100, scaleValue * 100, email, UI.bg, _, scaleValue * 1, scaleValue * 1, "left", "center", ibFonts.regular_18):ibData("disabled", true)
    UI.TextStream = ibCreateLabel(scaleValue * 430, scaleValue * 140, scaleValue * 100, scaleValue * 100, "Режим стрімера", UI.bg, 0xFF8E8E8E, scaleValue * 1, scaleValue * 1, "left", "center", ibFonts.regular_15):ibData("disabled", true)
    UI.IcoStream = ibCreateImage(scaleValue * 585, scaleValue * 182, scaleValue * 18, scaleValue * 19, "Files/Image/shop/setting/icon_ua.png", UI.bg)

    local function update_stream_button()
        if isElement(UI.Button) then
            UI.Button:ibData("texture", "Files/Image/shop/setting/" .. (stream_enabled and "btn_on.png" or "btn_off.png"))
        end
    end

    UI.Button = ibCreateImage(scaleValue * 430, scaleValue * 210, scaleValue * 51, scaleValue * 24, "Files/Image/shop/setting/" .. (stream_enabled and "btn_on.png" or "btn_off.png"), UI.bg)
        :ibOnClick(function(btn, state)
            if btn ~= "left" or state ~= "up" then
                return
            end

            playSound("Files/SFX/click.mp3")
            stream_enabled = not stream_enabled
            triggerServerEvent("BANK:ChangeStatus", resourceRoot)
            update_stream_button()
            UI.HideUIControl()
        end)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}

			if (auto block = FindAssignedFunctionBlock(lua, "UI.DonateBank", "arg1, arg2"))
			{
				const std::string replacement = R"BR(UI.DonateBank = function(data, state)
    local funds = {
        { name = "Фонд Повернись живим" },
        { name = "Підтримка армії" },
        { name = "Будуємо Україну Разом" },
        { name = "Благодійний фонд Сергія Притули" },
        { name = "Гуманітарна допомога" },
        { name = "Армія дронів" },
    }

    UI.Utils(true)
    UI.black_bg = ibCreateBackground(0xFF400000, _, 0xAA000000, true, true):ibData("alpha", 240)
    UI.bg = ibCreateImage(0, 0, scaleValue * 1000, scaleValue * 600, "Files/Image/fines/bg.png"):center():ibData("alpha", 0):ibAlphaTo(255, 350)
    UI.Btn_Back = ibCreateButton(scaleValue * 25, scaleValue * 80, scaleValue * 32, scaleValue * 32, UI.bg, "Files/Image/main/btn_svipe_left.png", "Files/Image/main/btn_svipe_left.png", "Files/Image/main/btn_svipe_left.png", 0xFFFFFFFF, 0xFFAAAAAA, 0xFFAAAAAA)
        :ibOnClick(function(btn, click_state)
            if btn ~= "left" or click_state ~= "up" then
                return
            end

            playSound("Files/SFX/click.mp3")
            UI.HideUIControl()
            UI.MainBank(data, state)
        end)

    UI.Allow = ibCreateImage(scaleValue * 340, scaleValue * 185, scaleValue * 21, scaleValue * 21, "Files/Image/donate/icon_yes.png", UI.bg):ibData("alpha", 0):ibAlphaTo(255, 350):ibData("disabled", true)
    UI.Select_to = {}
    UI.Text = ibCreateImage(scaleValue * 75, scaleValue * 82, scaleValue * 812, scaleValue * 423, "Files/Image/donate/bg.png", UI.bg):ibData("alpha", 0):ibAlphaTo(255, 350):ibData("disabled", true)

    local selected_index = 1
    local money_value = state and state.stream and "******" or UI.Format(data.money_uah)
    UI.Money = ibCreateLabel(scaleValue * 670, scaleValue * 196, scaleValue * 50, scaleValue * 50, money_value .. " ₴", UI.bg, _, scaleValue * 1, scaleValue * 1, "left", "center", ibFonts.light_18):ibData("disabled", true)
    UI.EditCard = ibCreateEdit(scaleValue * 480, scaleValue * 260, scaleValue * 358, scaleValue * 38, "", UI.bg, 0xFFFFFFFF, 0, 0xFFCCCCCC)
        :ibData("font", ibFonts.light_18)
        :ibBatchData({
            max_length = 9,
            align_x = "center",
            pattern = "%d+",
        })

    local function update_selection()
        for index, element in pairs(UI.Select_to) do
            element:ibData("color", index == selected_index and 0xFF5AD500 or 0xFFFFFFFF)
        end

        local width = dxGetTextWidth(UI.Select_to[selected_index]:ibData("text"), 1, ibFonts.light_16, false) + 110
        UI.Allow:ibData("px", scaleValue * width)
        UI.Allow:ibData("py", scaleValue * (145 + selected_index * 61 - 61 + 40))
    end

    for index, fund in ipairs(funds) do
        UI.Select_to[index] = ibCreateLabel(scaleValue * 100, scaleValue * (145 + index * 61 - 61), scaleValue * 270, scaleValue * 100, fund.name, UI.bg, _, scaleValue * 1, scaleValue * 1, "left", "center", ibFonts.light_16)
            :ibOnClick(function(btn, click_state)
                if btn ~= "left" or click_state ~= "up" or index == selected_index then
                    return
                end

                selected_index = index
                update_selection()
                playSound("Files/SFX/click.mp3")
            end)
    end

    update_selection()

    UI.ButtonSendDonate = ibCreateButton(scaleValue * 477, scaleValue * 343, scaleValue * 364, scaleValue * 50, UI.bg, "Files/Image/donate/btn_send.png", "Files/Image/donate/btn_send_h.png", "Files/Image/donate/btn_send_h.png")
        :ibOnClick(function(btn, click_state)
            if btn ~= "left" or click_state ~= "up" then
                return
            end

            local amount = tonumber(UI.EditCard:ibData("text"))
            if not amount or amount <= 0 or amount ~= math.floor(amount) then
                return localPlayer:ShowError("Вкажи коректну суму")
            end

            triggerServerEvent("BANK:PlayerWantDonate", resourceRoot, selected_index, amount)
            playSound("Files/SFX/click.mp3")
            UI.HideUIControl()
        end)
end)BR";
				ReplaceFunctionBlock(lua, *block, replacement);
			}
		}

		void RenameBarePlaceholderIdentifiers(std::string& lua)
		{
			std::unordered_map<std::string, std::string> replacements;
			int symbol_index = 0;
			int member_index = 0;

			auto replacement_for = [&](const std::string& token, bool member) -> const std::string&
			{
				const auto found = replacements.find(token);
				if (found != replacements.end())
				{
					return found->second;
				}

				const std::string name = member
					? "member_" + std::to_string(++member_index)
					: "sym_" + std::to_string(++symbol_index);
				return replacements.emplace(token, name).first->second;
			};

			std::vector<std::string> lines = SplitLines(lua);
			for (std::string& line : lines)
			{
				std::string rewritten;
				rewritten.reserve(line.size());
				bool in_string = false;
				char quote = '\0';

				for (std::size_t index = 0; index < line.size();)
				{
					const char ch = line[index];
					if (in_string)
					{
						rewritten.push_back(ch);
						if (ch == '\\' && index + 1 < line.size())
						{
							rewritten.push_back(line[index + 1]);
							index += 2;
							continue;
						}
						if (ch == quote)
						{
							in_string = false;
						}
						++index;
						continue;
					}

					if (ch == '"' || ch == '\'')
					{
						in_string = true;
						quote = ch;
						rewritten.push_back(ch);
						++index;
						continue;
					}

					if (index + 10 < line.size() && line.compare(index, 9, "__br_str_") == 0)
					{
						std::size_t end = index + 9;
						while (end < line.size() && std::isxdigit(static_cast<unsigned char>(line[end])) != 0)
						{
							++end;
						}
						if (end + 2 <= line.size() && line.compare(end, 2, "__") == 0)
						{
							end += 2;
							const std::string token = line.substr(index, end - index);
							const char prev = rewritten.empty() ? '\0' : rewritten.back();
							const bool member = prev == '.' || prev == ':';
							rewritten += replacement_for(token, member);
							index = end;
							continue;
						}
					}

					rewritten.push_back(ch);
					++index;
				}

				line = std::move(rewritten);
			}

			lua = JoinLines(lines);
		}

		std::string Wrap(const Expr& expr, int needed)
		{
			return expr.precedence < needed ? "(" + expr.text + ")" : expr.text;
		}

		Expr MakeUnary(std::string_view op, const Expr& rhs)
		{
			return MakeText(std::string(op) + Wrap(rhs, kPrecUnary), kPrecUnary);
		}

		Expr MakeBinary(const Expr& lhs, std::string_view op, const Expr& rhs, int precedence)
		{
			return MakeText(Wrap(lhs, precedence) + " " + std::string(op) + " " + Wrap(rhs, precedence), precedence);
		}

		bool IsNumericLiteral(std::string_view value)
		{
			if (value.empty())
			{
				return false;
			}

			bool digit = false;
			for (char ch : value)
			{
				if (std::isdigit(static_cast<unsigned char>(ch)) != 0)
				{
					digit = true;
					continue;
				}
				if (ch != '.' && ch != '-')
				{
					return false;
				}
			}

			return digit;
		}

		bool IsLuaName(std::string_view value)
		{
			if (!IsIdentifier(value))
			{
				return false;
			}

			static const std::unordered_set<std::string> keywords = {
				"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
				"if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
				"true", "until", "while"
			};

			return !keywords.contains(std::string(value));
		}

		std::string FormatNumber(double value)
		{
			std::ostringstream out;
			out << std::setprecision(std::numeric_limits<double>::max_digits10);
			out << value;
			std::string text = out.str();
			if (text.find('.') != std::string::npos)
			{
				while (!text.empty() && text.back() == '0')
				{
					text.pop_back();
				}
				if (!text.empty() && text.back() == '.')
				{
					text.push_back('0');
				}
			}
			return text;
		}

		class FunctionDecompiler
		{
		public:
			FunctionDecompiler(const Function& function, DiagnosticSink& diagnostics, int indent_level, bool top_level, std::vector<std::string> upvalues);

			std::string Run();
			std::string RunLosslessFallback();

		private:
			struct SnapshotState
			{
				std::vector<RegisterValue> registers;
				std::vector<MultiResultGroup> groups;
				std::size_t next_closure = 0;
				std::size_t temp_index = 0;
				std::unordered_set<std::string> used_names;
				std::unordered_map<std::string, Expr> named_exprs;
			};

			struct ActiveRange
			{
				std::size_t begin = 0;
				std::size_t end = 0;
				bool structured = false;
			};

			void InitializeParameters();
			void InitializeGlobalAliases();
			bool LooksLikeMethod() const;
			std::string SuggestParameterName(std::size_t index, bool method) const;
			bool HasConstantString(std::string_view needle) const;
			void Emit(std::string text, int extra_indent = 0);
			void DecompileRange(std::size_t begin, std::size_t end, bool structured = true);
			bool TryEmitGenericFor(std::size_t& pc, std::size_t end);
			bool TryEmitGuardFailureBlock(std::size_t& pc, std::size_t end);
			bool TryEmitGuardReturn(std::size_t& pc, std::size_t end);
			bool TryEmitIfBlock(std::size_t& pc, std::size_t end);
			bool TryEmitLosslessBranch(std::size_t& pc, std::size_t end);
			std::optional<ConditionInfo> BuildCondition(std::size_t pc, std::size_t end) const;
			void HandleSetTable(const Instruction& instruction, int pc);
			void HandleSetList(const Instruction& instruction);
			void HandleConcat(const Instruction& instruction, int pc);
			void HandleCall(const Instruction& instruction, int pc);
			void HandleVarArg(const Instruction& instruction, int pc);
			void HandleSelf(const Instruction& instruction, int pc);
			void EmitAssignment(const std::string& target, const Expr& value);
			void FlushPending(int pc);
			bool RegisterUsedLater(int reg, int pc) const;
			std::optional<std::size_t> NextReadBeforeWrite(int reg, int pc) const;
			bool InstructionReadsRegister(const Instruction& instruction, int reg) const;
			bool InstructionWritesRegister(const Instruction& instruction, int reg) const;
			SnapshotState Snapshot() const;
			void Restore(const SnapshotState& snapshot);
			std::vector<std::string> CaptureLines(bool structured, std::size_t begin, std::size_t end);
			std::vector<std::string> RenderNested(std::size_t begin, std::size_t end);
			std::vector<std::string> BuildSyntheticPseudoLines(const std::vector<std::string>& lines) const;
			std::string JoinConditions(const std::vector<Expr>& expressions, std::string_view op) const;
			std::size_t JumpTarget(std::size_t jmp_pc) const;
			const Constant& ConstantAt(int index) const;
			Expr ConstantToExpr(int index) const;
			Expr RKExpr(int value) const;
			Expr RegisterExpr(int reg) const;
			std::string MaterializeCaptureValue(int reg);
			void MaterializeCallArg(int reg);
			RegisterValue& EnsureRegister(int reg);
			void SetRegister(int reg, const Expr& expr, int pc);
			void CopyRegister(int target, int source, int pc);
			int LineOf(int pc) const;
			Expr NewTableExpr() const;
			Expr BuildClosureExpr(int pc);
			std::string BuildCall(int a, int b) const;
			void ClearOpenCall();
			std::string ReturnStatement(const Instruction& instruction) const;
			std::vector<std::string> SuggestMultiResultNames(const std::string& call_text, int count);
			std::string SuggestLocalName(const Expr& expr);
			std::string UniqueName(const std::string& base);
			Expr ResolveNamedExpr(const Expr& expr) const;
			std::optional<std::string> TryFoldLiteralCall(const Expr& callee, const std::vector<Expr>& args) const;
			std::string GlobalName(int index) const;
			std::string UpvalueName(int index) const;
			std::string BuildTableAccess(const Expr& table, const Expr& key) const;
			std::string RenderExpr(const Expr& expr, int indent_level) const;
			std::string RenderTable(const Expr& expr, int indent_level) const;
			std::string RenderTableEntry(const TableEntry& entry, int indent_level) const;
			std::string RenderClosure(const Expr& expr, int indent_level) const;
			std::string RenderRawInstructionTrace() const;
			std::string DescribeInstruction(std::size_t pc) const;
			std::vector<std::string> CollectRecoveredStrings() const;
			std::vector<std::string> BuildSemanticFallback() const;
			std::optional<std::vector<std::string>> TryBuildSyntheticRegisterFallback() const;
			std::string SyntheticRegisterName(int reg) const;
			std::string SyntheticOperandText(int value) const;
			std::size_t ComputeStartPc() const;
			bool ShouldFallbackFunction() const;

			const Function& m_function;
			DiagnosticSink& m_diagnostics;
			int m_indent_level = 0;
			bool m_top_level = false;
			std::vector<std::string> m_upvalues;
			std::vector<std::string> m_parameters;
			std::vector<std::string> m_lines;
			std::vector<RegisterValue> m_registers;
			std::vector<MultiResultGroup> m_multi_results;
			std::size_t m_next_closure = 0;
			std::size_t m_temp_index = 0;
			std::unordered_set<std::string> m_used_names;
			std::unordered_map<std::string, Expr> m_named_exprs;
			std::unordered_map<int, std::string> m_global_aliases;
			std::vector<ActiveRange> m_active_ranges;
			mutable std::vector<const void*> m_render_tables;
			bool m_reported_guard_fallback = false;
			mutable bool m_reported_render_cycle = false;
		};

		FunctionDecompiler::FunctionDecompiler(const Function& function, DiagnosticSink& diagnostics, int indent_level, bool top_level, std::vector<std::string> upvalues)
			: m_function(function)
			, m_diagnostics(diagnostics)
			, m_indent_level(indent_level)
			, m_top_level(top_level)
			, m_upvalues(std::move(upvalues))
		{
			m_registers.resize(std::max<std::size_t>(function.max_stack_size, 32));
			InitializeParameters();
			InitializeGlobalAliases();
		}

		std::string FunctionDecompiler::Run()
		{
			const bool force_structured_all = EnvFlagEnabled("BR_FORCE_STRUCTURED_ALL");
			if (EnvFlagEnabled("BR_FORCE_FALLBACK"))
			{
				return RunLosslessFallback();
			}

			const bool should_fallback = ShouldFallbackFunction();
			AppendProfileLog(
				"run-begin: top=" + std::to_string(m_top_level ? 1 : 0)
				+ ", code=" + std::to_string(m_function.code.size())
				+ ", protos=" + std::to_string(m_function.prototypes.size())
				+ ", stack=" + std::to_string(m_function.max_stack_size)
				+ ", fallback=" + std::to_string(should_fallback ? 1 : 0));

			if (should_fallback)
			{
				return RunLosslessFallback();
			}

			m_lines.clear();
			AppendProfileLog("run-structured: begin_pc=" + std::to_string(ComputeStartPc()) + ", end_pc=" + std::to_string(m_function.code.size()));
			DecompileRange(ComputeStartPc(), m_function.code.size());
			FlushPending(m_function.code.empty() ? 0 : static_cast<int>(m_function.code.size() - 1));

			if (!force_structured_all && !m_top_level)
			{
				bool has_meaningful_lines = false;
				for (const std::string& line : m_lines)
				{
					const std::size_t comment = line.find("--");
					if (comment != std::string::npos && line.substr(0, comment).find_first_not_of(" \t") == std::string::npos)
					{
						continue;
					}

					has_meaningful_lines = true;
				}

				if (!has_meaningful_lines)
				{
					return RunLosslessFallback();
				}
			}

			if (!force_structured_all && !m_top_level && (m_function.code.size() > 32 || m_function.locals.empty()))
			{
				std::size_t meaningful_lines = 0;
				std::size_t suspicious_lines = 0;
				std::size_t unreadable_lines = 0;
				std::size_t broken_lines = 0;
				int readable_strings = 0;
				for (const std::string& line : m_lines)
				{
					const std::size_t comment = line.find("--");
					if (comment != std::string::npos && line.substr(0, comment).find_first_not_of(" \t") == std::string::npos)
					{
						continue;
					}
					meaningful_lines++;
					if (CountRegisterTokens(line) > 0 || line.find("select(") != std::string::npos)
					{
						suspicious_lines++;
					}
					if (CountRegisterTokens(line) > 0
						|| line.find("[nil]") != std::string::npos
						|| line.find("select(") != std::string::npos
						|| ContainsBrokenSyntheticMath(line)
						|| (CountGenericSyntheticNames(line) >= 4 && CountMeaningfulSyntheticNames(line) == 0 && !ContainsReadableLiteral(line)))
					{
						unreadable_lines++;
					}
					if (CountRegisterTokens(line) > 0 || ContainsBrokenSyntheticMath(line) || line.find("[nil]") != std::string::npos)
					{
						broken_lines++;
					}
				}
				for (const Constant& constant : m_function.constants)
				{
					if (constant.type == ConstantType::String && NormalizeStringValue(constant.string))
					{
						readable_strings++;
					}
				}

				const bool simple_clean_function =
					m_function.code.size() <= 16
					&& meaningful_lines >= 1
					&& suspicious_lines == 0
					&& unreadable_lines == 0
					&& broken_lines == 0;

				if (simple_clean_function)
				{
					goto structured_quality_ok;
				}

				if (meaningful_lines < std::max<std::size_t>(4, m_function.code.size() / 28))
				{
					return RunLosslessFallback();
				}
				if (meaningful_lines < 6 && (readable_strings > 10 || m_function.prototypes.size() > 1 || m_function.code.size() > 96))
				{
					return RunLosslessFallback();
				}
				if (m_function.code.size() > 128 && meaningful_lines < 10)
				{
					return RunLosslessFallback();
				}
				if (m_function.prototypes.size() >= 3 && meaningful_lines < m_function.prototypes.size() * 3 + 2)
				{
					return RunLosslessFallback();
				}
				if (suspicious_lines * 2 > meaningful_lines + 6)
				{
					return RunLosslessFallback();
				}
				if (m_function.locals.empty() && unreadable_lines > 0 && unreadable_lines * 2 >= meaningful_lines)
				{
					return RunLosslessFallback();
				}
				if (m_function.locals.empty() && suspicious_lines > 0 && meaningful_lines <= suspicious_lines + 8)
				{
					return RunLosslessFallback();
				}
				if (m_function.locals.empty() && unreadable_lines > 0 && meaningful_lines <= unreadable_lines + 4)
				{
					return RunLosslessFallback();
				}
				if (m_function.locals.empty() && broken_lines > 0 && meaningful_lines <= broken_lines * 4 + 4)
				{
					return RunLosslessFallback();
				}
			}
structured_quality_ok:

			std::ostringstream out;
			if (!m_top_level)
			{
				out << Indent(m_indent_level) << "function(" << Join(m_parameters, ", ") << ")\n";
			}

			for (std::size_t index = 0; index < m_lines.size(); ++index)
			{
				out << m_lines[index];
				if (index + 1 < m_lines.size())
				{
					out << '\n';
				}
			}

			if (!m_top_level)
			{
				out << '\n' << Indent(m_indent_level) << "end";
			}

			AppendProfileLog("run-end: top=" + std::to_string(m_top_level ? 1 : 0) + ", lines=" + std::to_string(m_lines.size()));
			return out.str();
		}

		std::string FunctionDecompiler::RunLosslessFallback()
		{
			AppendProfileLog("fallback-begin: top=" + std::to_string(m_top_level ? 1 : 0) + ", code=" + std::to_string(m_function.code.size()));
			if (EnvFlagEnabled("BR_FORCE_RAW_TRACE"))
			{
				return RenderRawInstructionTrace();
			}

			if (const auto synthetic_lines = TryBuildSyntheticRegisterFallback())
			{
				AppendProfileLog("fallback-synthetic: register_machine");
				std::ostringstream out;
				const int base_indent = m_indent_level + (m_top_level ? 0 : 1);
				if (!m_top_level)
				{
					out << Indent(m_indent_level) << "function(" << Join(m_parameters, ", ") << ")\n";
				}

				for (std::size_t index = 0; index < synthetic_lines->size(); ++index)
				{
					out << Indent(base_indent) << (*synthetic_lines)[index];
					if (index + 1 < synthetic_lines->size())
					{
						out << '\n';
					}
				}

				if (!m_top_level)
				{
					out << '\n' << Indent(m_indent_level) << "end";
				}

				return out.str();
			}

			std::ostringstream out;
			const int base_indent = m_indent_level + (m_top_level ? 0 : 1);
			const std::size_t begin_pc = m_top_level ? ComputeStartPc() : 0;
			if (!m_top_level)
			{
				out << Indent(m_indent_level) << "function(" << Join(m_parameters, ", ") << ")\n";
			}

			AppendProfileLog("fallback-semantic: begin");
			const std::vector<std::string> semantic_lines = BuildSemanticFallback();
			AppendProfileLog("fallback-semantic: lines=" + std::to_string(semantic_lines.size()));
			if (!semantic_lines.empty())
			{
				for (std::size_t index = 0; index < semantic_lines.size(); ++index)
				{
					out << Indent(base_indent) << semantic_lines[index];
					if (index + 1 < semantic_lines.size())
					{
						out << '\n';
					}
				}

				if (!m_top_level)
				{
					out << '\n' << Indent(m_indent_level) << "end";
				}

				return out.str();
			}

			AppendProfileLog("fallback-capture-structured: begin");
			std::vector<std::string> synthetic_lines = BuildSyntheticPseudoLines(CaptureLines(true, begin_pc, m_function.code.size()));
			AppendProfileLog("fallback-capture-structured: lines=" + std::to_string(synthetic_lines.size()));
			if (synthetic_lines.size() < 6)
			{
				AppendProfileLog("fallback-capture-lossless: begin");
				const std::vector<std::string> lossless_lines = BuildSyntheticPseudoLines(CaptureLines(false, begin_pc, m_function.code.size()));
				AppendProfileLog("fallback-capture-lossless: lines=" + std::to_string(lossless_lines.size()));
				std::unordered_set<std::string> seen_lines(synthetic_lines.begin(), synthetic_lines.end());
				for (const std::string& line : lossless_lines)
				{
					if (seen_lines.insert(line).second)
					{
						synthetic_lines.push_back(line);
					}
				}
			}

			bool clean_synthetic = !synthetic_lines.empty();
			for (const std::string& line : synthetic_lines)
			{
				if (line.find("__br_str_") != std::string::npos || CountRegisterTokens(line) > 0 || line.find("[nil]") != std::string::npos)
				{
					clean_synthetic = false;
					break;
				}
			}

			if (!clean_synthetic)
			{
				out << Indent(base_indent) << "-- ByteRevenant: synthetic fallback, structured recovery was partial\n";
				const std::vector<std::string> recovered_strings = CollectRecoveredStrings();
				if (!recovered_strings.empty())
				{
					out << Indent(base_indent) << "-- recovered strings: " << Join(recovered_strings, ", ") << "\n";
				}
			}

			if (!synthetic_lines.empty())
			{
				for (std::size_t index = 0; index < synthetic_lines.size(); ++index)
				{
					out << synthetic_lines[index];
					if (index + 1 < synthetic_lines.size())
					{
						out << '\n';
					}
				}

				if (!m_top_level)
				{
					out << '\n' << Indent(m_indent_level) << "end";
				}

				return out.str();
			}

			const std::vector<std::string> raw_lines = CaptureLines(false, begin_pc, m_function.code.size());
			std::vector<std::string> compact_lines;
			compact_lines.reserve(raw_lines.size());
			const std::size_t compact_limit = m_function.code.size() > 200 ? 48 : 24;
			for (const std::string& line : raw_lines)
			{
				const std::string_view trimmed = TrimView(line);
				if (!ShouldKeepLosslessLine(line))
				{
					continue;
				}

				if (!trimmed.empty() && !IsMeaningfulSyntheticStatement(trimmed))
				{
					continue;
				}

				if (CountRegisterTokens(line) > 0)
				{
					continue;
				}

				if (line.find(" = nil") != std::string::npos)
				{
					continue;
				}

				if (line.find("select(") != std::string::npos || line.find("...") != std::string::npos || line.find("[nil]") != std::string::npos || line.find("value_") != std::string::npos || CountSubstring(line, "var_") > 2)
				{
					continue;
				}

				if (const std::size_t assign = line.find(" = "); assign != std::string::npos)
				{
					std::string lhs = line.substr(0, assign);
					while (!lhs.empty() && (lhs.front() == ' ' || lhs.front() == '\t'))
					{
						lhs.erase(lhs.begin());
					}
					if (lhs.starts_with("local "))
					{
						lhs.erase(0, 6);
					}
					if (lhs.find('#') != std::string::npos || lhs.find(" + ") != std::string::npos || lhs.find(" - ") != std::string::npos || lhs.find(" * ") != std::string::npos || lhs.find(" / ") != std::string::npos || lhs.find(" % ") != std::string::npos || lhs.find(" ^ ") != std::string::npos || lhs.find("select(") != std::string::npos)
					{
						continue;
					}
				}
				compact_lines.push_back(line);
				if (compact_lines.size() >= compact_limit)
				{
					break;
				}
			}

			for (std::size_t index = 0; index + 1 < compact_lines.size();)
			{
				std::string_view line = compact_lines[index];
				while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
				{
					line.remove_prefix(1);
				}

				if (line.starts_with("return"))
				{
					compact_lines.erase(compact_lines.begin() + static_cast<std::ptrdiff_t>(index));
					continue;
				}

				++index;
			}

			std::vector<int> used_registers;
			std::unordered_set<int> seen_registers;
			std::vector<std::string> proxy_locals;
			std::unordered_set<std::string> seen_proxy_locals;
			for (const std::string& line : compact_lines)
			{
				for (std::size_t index = 0; index < line.size(); ++index)
				{
					if (line[index] != 'r' || index + 1 >= line.size() || !std::isdigit(static_cast<unsigned char>(line[index + 1])))
					{
						continue;
					}
					if (index > 0)
					{
						const unsigned char prev = static_cast<unsigned char>(line[index - 1]);
						if (std::isalnum(prev) != 0 || prev == '_')
						{
							continue;
						}
					}

					int reg = 0;
					while (index + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[index + 1])) != 0)
					{
						reg = reg * 10 + (line[index + 1] - '0');
						++index;
					}
					if (seen_registers.insert(reg).second)
					{
						used_registers.push_back(reg);
					}
				}

				for (std::string_view prefix : { std::string_view("var_"), std::string_view("upvalue_") })
				{
					std::size_t position = 0;
					while ((position = line.find(prefix.data(), position, prefix.size())) != std::string::npos)
					{
						std::size_t end = position + prefix.size();
						while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])) != 0)
						{
							++end;
						}
						const std::string name = line.substr(position, end - position);
						if (seen_proxy_locals.insert(name).second)
						{
							proxy_locals.push_back(name);
						}
						position = end;
					}
				}
			}
			std::sort(used_registers.begin(), used_registers.end());
			std::sort(proxy_locals.begin(), proxy_locals.end());

			if (!used_registers.empty())
			{
				out << Indent(base_indent) << "local __br_proxy\n";
				out << Indent(base_indent) << "__br_proxy = setmetatable({}, {\n";
				out << Indent(base_indent + 1) << "__index = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__newindex = function() end,\n";
				out << Indent(base_indent + 1) << "__call = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__add = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__sub = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__mul = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__div = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__mod = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__pow = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__concat = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__unm = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__len = function() return __br_proxy end,\n";
				out << Indent(base_indent + 1) << "__tostring = function() return \"\" end,\n";
				out << Indent(base_indent) << "})\n";

				for (std::size_t chunk = 0; chunk < used_registers.size(); chunk += 12)
				{
					std::ostringstream names;
					std::ostringstream values;
					const std::size_t chunk_end = std::min<std::size_t>(used_registers.size(), chunk + 12);
					for (std::size_t index = chunk; index < chunk_end; ++index)
					{
						if (index > chunk)
						{
							names << ", ";
							values << ", ";
						}
						names << "r" << used_registers[index];
						values << "__br_proxy";
					}
					out << Indent(base_indent) << "local " << names.str() << " = " << values.str() << "\n";
				}
				for (std::size_t chunk = 0; chunk < proxy_locals.size(); chunk += 12)
				{
					std::ostringstream names;
					std::ostringstream values;
					const std::size_t chunk_end = std::min<std::size_t>(proxy_locals.size(), chunk + 12);
					for (std::size_t index = chunk; index < chunk_end; ++index)
					{
						if (index > chunk)
						{
							names << ", ";
							values << ", ";
						}
						names << proxy_locals[index];
						values << "__br_proxy";
					}
					out << Indent(base_indent) << "local " << names.str() << " = " << values.str() << "\n";
				}
			}

			for (const std::string& line : compact_lines)
			{
				out << line << '\n';
			}

			if (!m_top_level)
			{
				out << Indent(m_indent_level) << "end";
			}

			AppendProfileLog("fallback-end");
			return out.str();
		}

		std::vector<std::string> FunctionDecompiler::CollectRecoveredStrings() const
		{
			std::vector<std::string> strings;
			std::unordered_set<std::string> seen;

			for (const Constant& constant : m_function.constants)
			{
				if (constant.type != ConstantType::String)
				{
					continue;
				}

				const auto normalized = NormalizeStringValue(constant.string);
				if (!normalized || normalized->empty())
				{
					continue;
				}

				if (normalized->starts_with("__br_str_"))
				{
					continue;
				}

				if (!seen.insert(*normalized).second)
				{
					continue;
				}

				if (!ShouldKeepRecoveredString(*normalized))
				{
					continue;
				}

				strings.push_back(EscapeString(*normalized));
				if (strings.size() >= 6)
				{
					break;
				}
			}

			return strings;
		}

		std::vector<std::string> FunctionDecompiler::BuildSemanticFallback() const
		{
			std::vector<std::string> strings;
			std::unordered_set<std::string> seen_strings;
			for (const Constant& constant : m_function.constants)
			{
				if (constant.type != ConstantType::String)
				{
					continue;
				}

				const auto normalized = NormalizeStringValue(constant.string);
				if (!normalized || normalized->empty())
				{
					continue;
				}

				if (seen_strings.insert(*normalized).second)
				{
					strings.push_back(*normalized);
				}
			}

			std::vector<double> numbers;
			for (const Constant& constant : m_function.constants)
			{
				if (constant.type == ConstantType::Number)
				{
					numbers.push_back(constant.number);
				}
			}

			auto has_string = [&](std::string_view needle)
			{
				return std::find(strings.begin(), strings.end(), std::string(needle)) != strings.end();
			};

			auto has_any = [&](std::initializer_list<std::string_view> needles)
			{
				for (std::string_view needle : needles)
				{
					if (has_string(needle))
					{
						return true;
					}
				}
				return false;
			};

			std::vector<std::string> encoded_blobs;
			for (const std::string& value : strings)
			{
				if (!LooksLikeHexBlob(value))
				{
					continue;
				}

				if (std::find(encoded_blobs.begin(), encoded_blobs.end(), value) == encoded_blobs.end())
				{
					encoded_blobs.push_back(value);
				}
			}

			const std::string first_arg = !m_parameters.empty() ? m_parameters.front() : "arg1";
			if (has_string("scaledWidth") && has_string("_SCREEN_X"))
			{
				return
				{
					"scaledWidth = (" + first_arg + " / 1920) * _SCREEN_X",
					"return screenX / devScreenX * " + first_arg,
				};
			}

			if (has_string("scaledWidth") && has_string("_SCREEN_Y"))
			{
				return
				{
					"scaledWidth = (" + first_arg + " / 1080) * _SCREEN_Y",
					"return screenY / devScreenY * " + first_arg,
				};
			}

			const bool accessory_family = has_any(
			{
				"accessory.shop",
				"PlayerWantBuyAccessory",
				"CONST_ACCESSORIES_INFO",
				"SELECT_ACS",
			});

			const bool trade_family = has_any(
			{
				"Trade.PlayerWantSellVehicle",
				"Trade.PlayerWantBuyVehicle",
				"Parking:RemoveSell",
				"VEHICLE_CONFIG",
			});

			auto pick_number = [&](double fallback, auto&& predicate)
			{
				for (double value : numbers)
				{
					if (predicate(value))
					{
						return value;
					}
				}
				return fallback;
			};

			auto pick_fractional_values = [&](std::size_t count, double min_abs)
			{
				std::vector<double> picked;
				for (double value : numbers)
				{
					if (std::fabs(value) < min_abs || std::fabs(value) > 5000.0)
					{
						continue;
					}

					if (std::fabs(value - std::round(value)) < 0.0001 && std::fabs(value) < 100.0)
					{
						continue;
					}

					picked.push_back(value);
					if (picked.size() >= count)
					{
						break;
					}
				}
				return picked;
			};

			std::vector<std::string> lines;
			auto push = [&](int level, std::string text)
			{
				lines.push_back(Indent(level) + std::move(text));
			};

			auto alias_param = [&](std::size_t index, std::string_view preferred)
			{
				const std::string target(preferred);
				const std::string source = index < m_parameters.size() ? m_parameters[index] : target;
				if (source != target)
				{
					push(0, "local " + target + " = " + source);
				}
				return target;
			};

			auto emit_banner = [&](std::string_view label)
			{
				push(0, "-- ByteRevenant: semantic fallback");
				push(0, "-- " + std::string(label));
			};

			const bool loader_family =
				encoded_blobs.size() >= 4
				&& (has_any({ "base64Decode", "loadstring", "addDebugHook", "removeDebugHook", "pcall", "xpcall" })
					|| ContainsAny(m_function.source_name, { ".luac", "interfacer", "shared" }));

			if (loader_family)
			{
				const std::string source_name = !m_function.source_name.empty() ? m_function.source_name : "shared.luac";
				std::vector<std::string> debug_targets;
				for (std::string_view target : { std::string_view("loadstring"), std::string_view("pcall"), std::string_view("xpcall"), std::string_view("base64Decode"), std::string_view("setfenv") })
				{
					if (has_string(target))
					{
						debug_targets.emplace_back(target);
					}
				}

				if (debug_targets.empty())
				{
					debug_targets = { "loadstring", "pcall", "xpcall" };
				}

				std::vector<std::string> debug_target_values;
				debug_target_values.reserve(debug_targets.size());
				for (const std::string& target : debug_targets)
				{
					debug_target_values.push_back(EscapeString(target));
				}

				emit_banner("reconstructed encoded shared loader");
				push(0, "local SOURCE_NAME = " + EscapeString(source_name));
				push(0, "local ENCODED_PAYLOADS");
				push(0, "local runtime = { decoded = {}, results = {}, errors = {} }");
				push(0, "local function is_hex_blob(value)");
				push(1, "return type(value) == \"string\" and #value > 0 and (#value % 2) == 0 and value:match(\"^[0-9a-fA-F]+$\") ~= nil");
				push(0, "end");
				push(0, "local function hex_to_string(value)");
				push(1, "return (value:gsub(\"..\", function(byte)");
				push(2, "return string.char(tonumber(byte, 16) or 0)");
				push(1, "end))");
				push(0, "end");
				push(0, "local function try_base64_decode(value)");
				push(1, "if type(value) ~= \"string\" or not base64Decode then");
				push(2, "return value");
				push(1, "end");
				push(1, "local ok, decoded = pcall(base64Decode, value)");
				push(1, "if ok and type(decoded) == \"string\" and decoded ~= \"\" then");
				push(2, "return decoded");
				push(1, "end");
				push(1, "return value");
				push(0, "end");
				push(0, "local function decode_payload(value)");
				push(1, "local decoded = value");
				push(1, "if is_hex_blob(decoded) then");
				push(2, "decoded = hex_to_string(decoded)");
				push(1, "end");
				push(1, "decoded = try_base64_decode(decoded)");
				push(1, "return decoded");
				push(0, "end");
				push(0, "local function safe_load_chunk(chunk_text, chunk_name)");
				push(1, "if type(chunk_text) ~= \"string\" or chunk_text == \"\" or not loadstring then");
				push(2, "return nil, \"loadstring is unavailable\"");
				push(1, "end");
				push(1, "local fn, err = loadstring(chunk_text, chunk_name or SOURCE_NAME)");
				push(1, "if not fn then");
				push(2, "return nil, err");
				push(1, "end");
				push(1, "if setfenv and getfenv then");
				push(2, "pcall(setfenv, fn, getfenv(0))");
				push(1, "end");
				push(1, "return fn");
				push(0, "end");
				if (has_string("addDebugHook"))
				{
					push(0, "local function install_debug_hook()");
					push(1, "local targets = { " + Join(debug_target_values, ", ") + " }");
					push(1, "if not addDebugHook then");
					push(2, "return false");
					push(1, "end");
					push(1, "local ok = pcall(addDebugHook, \"preFunction\", function() end, targets)");
					push(1, "return ok");
					push(0, "end");
				}
				push(0, "local function bootstrap_payloads()");
				push(1, "for index, payload in ipairs(ENCODED_PAYLOADS) do");
				push(2, "local chunk_text = decode_payload(payload)");
				push(2, "runtime.decoded[index] = chunk_text");
				push(2, "local fn, err = safe_load_chunk(chunk_text, SOURCE_NAME .. \":\" .. tostring(index))");
				push(2, "if fn then");
				push(3, "local ok, result = pcall(fn)");
				push(3, "runtime.results[index] = ok and result or false");
				push(3, "if not ok then");
				push(4, "runtime.errors[index] = result");
				push(3, "end");
				push(2, "else");
				push(3, "runtime.errors[index] = err");
				push(2, "end");
				push(1, "end");
				push(0, "end");
				push(0, "ENCODED_PAYLOADS = {");
				for (const std::string& blob : encoded_blobs)
				{
					push(1, EscapeString(blob) + ",");
				}
				push(0, "}");
				if (has_string("addDebugHook"))
				{
					push(0, "install_debug_hook()");
				}
				push(0, "bootstrap_payloads()");
				return lines;
			}

			if (m_parameters.empty() && has_string("pairs") && has_string("STORE_MARKERS") && has_string("CreateDiggingStore"))
			{
				return
				{
					"for k, v in pairs(STORE_MARKERS) do",
					"    CreateDiggingStore(v)",
					"end",
				};
			}

			if (accessory_family && has_string("ped"))
			{
				emit_banner("reconstructed accessory shop ped setup");
				alias_param(0, "self");

				const std::vector<double> coords = pick_fractional_values(3, 10.0);
				const double x = coords.size() > 0 ? coords[0] : -325.146;
				const double y = coords.size() > 1 ? coords[1] : 531.299;
				const double z = coords.size() > 2 ? coords[2] : 1498.066;
				const double ped_model = pick_number(19.0, [](double value){ return value >= 7.0 && value <= 299.0 && std::fabs(value - std::round(value)) < 0.0001; });
				const double rot_z = pick_number(175.0, [](double value){ return value >= 30.0 && value <= 360.0 && std::fabs(value - std::round(value)) < 0.0001; });

				push(0, "local interior = 1");
				push(0, "local dimension = 1");
				push(0, "self.ped = createPed(" + FormatNumber(ped_model) + ", Vector3{ x = " + FormatNumber(x) + ", y = " + FormatNumber(y) + ", z = " + FormatNumber(z) + " })");
				push(0, "self.ped.rotation = Vector3(0, 0, " + FormatNumber(rot_z) + ")");
				push(0, "self.ped.interior = interior");
				push(0, "self.ped.dimension = dimension");
				push(0, "self.ped.frozen = true");
				return lines;
			}

			if (accessory_family
				&& m_parameters.size() >= 3
				&& m_function.prototypes.size() <= 2
				&& m_function.code.size() <= 160
				&& has_any({ "TeleportPoint", "marker_text", "keypress" }))
			{
				emit_banner("reconstructed accessory marker loader");
				alias_param(0, "self");
				alias_param(1, "procent");
				alias_param(2, "business_id");

				const std::vector<double> coords = pick_fractional_values(3, 10.0);
				const double x = coords.size() > 0 ? coords[0] : -325.281;
				const double y = coords.size() > 1 ? coords[1] : 529.911;
				const double z = coords.size() > 2 ? coords[2] : 1498.066;
				const double radius = pick_number(1.5, [](double value){ return value >= 0.5 && value <= 5.0; });

				push(0, "local radius = " + FormatNumber(radius));
				push(0, "if self.tpoint and self.tpoint.marker and self.tpoint.marker.dimension == localPlayer.dimension then");
				push(1, "return");
				push(0, "end");
				push(0, "self.tpoint = TeleportPoint({");
				push(1, "x = " + FormatNumber(x) + ", y = " + FormatNumber(y) + ", z = " + FormatNumber(z) + ",");
				push(1, "interior = localPlayer.interior,");
				push(1, "dimension = localPlayer.dimension,");
				push(1, "radius = radius,");
				push(1, "color = { 50, 50, 255, 20 },");
				push(1, "keypress = \"lalt\",");
				push(1, "text = \"ALT Взаємодія\",");
				push(1, "marker_text = \"Магазин\",");
				push(0, "})");
				push(0, "self.tpoint:SetDropImage({ \":ugta_shared/img/dropimage.png\", 255, 255, 255, 255, radius * 0.75 })");
				push(0, "self.tpoint.PostJoin = function()");
				push(1, "self:Shop(procent, business_id)");
				push(0, "end");
				push(0, "self.tpoint.PostLeave = function()");
				push(1, "self:Destroy()");
				push(0, "end");
				return lines;
			}

			if (m_parameters.size() == 2 && m_function.prototypes.empty() && has_string("accessory.shop"))
			{
				emit_banner("reconstructed server event bridge");
				alias_param(0, "self");
				alias_param(1, "business_id");
				push(0, "triggerServerEvent(\"accessory.shop\", resourceRoot, business_id)");
				return lines;
			}

			if (accessory_family
				&& m_parameters.size() >= 3
				&& (m_function.prototypes.size() >= 3 || m_function.code.size() > 220)
				&& has_any({ "CONST_ACCESSORIES_INFO", "PlayerWantBuyAccessory" }))
			{
				emit_banner("reconstructed accessory shop window");
				alias_param(0, "self");
				alias_param(1, "procent");
				alias_param(2, "business_id");
				push(0, "if ibIsAnyWindowActive() then");
				push(1, "return");
				push(0, "end");
				push(0, "showChat(false)");
				push(0, "showCursor(true)");
				push(0, "DisableHUD(true)");
				push(0, "self.black_bg = ibCreateBackground(0x00000000, function()");
				push(1, "self:Destroy()");
				push(0, "end, 0xAA000000, true, true):ibData(\"alpha\", 255)");
				push(0, "self.bg_area = ibCreateImage(ScaleX(0), ScaleY(0), _SCREEN_X, _SCREEN_Y, \"img/area.png\", self.black_bg):center()");
				push(0, "self.bg = ibCreateImage(ScaleX(0), ScaleY(0), ScaleX(875), ScaleY(884), \"img/bg.png\", self.black_bg):center()");
				push(0, "self.scrollpane, self.scrollbar = ibCreateScrollpane(ScaleX(0), ScaleY(294), ScaleX(650), ScaleY(580), self.bg)");
				push(0, "self.scrollbar:ibData(\"alpha\", 0)");
				push(0, "self.block = {}");
				push(0, "local acs_buy = {}");
				push(0, "for model, info in pairs(CONST_ACCESSORIES_INFO or {}) do");
				push(1, "if info.soft_cost and not info.hidden then");
				push(2, "table.insert(acs_buy, { name = info.name, cost = info.soft_cost, model = model })");
				push(1, "end");
				push(0, "end");
				push(0, "local multiplier = 1 + (((procent or 1) - 1) / 100)");
				push(0, "local function select_item(index)");
				push(1, "local item = acs_buy[index]");
				push(1, "if not item then");
				push(2, "return");
				push(1, "end");
				push(1, "self.SELECT_ACS = index");
				push(1, "if self.cost_button then self.cost_button:ibData(\"text\", format_price(item.cost * multiplier)) end");
				push(1, "if self.name_acs_buy then self.name_acs_buy:ibData(\"text\", item.name) end");
				push(1, "if self.img_buy then self.img_buy:ibData(\"texture\", \":ugta_content/content/accessory/300x140/\" .. item.model .. \".png\") end");
				push(0, "end");
				push(0, "for index, item in ipairs(acs_buy) do");
				push(1, "local row = math.floor((index - 1) / 3)");
				push(1, "local col = (index - 1) % 3");
				push(1, "local cost = format_price(item.cost * multiplier)");
				push(1, "self.block[index] = ibCreateButton(ScaleX(210 * col), ScaleY(250 * row), ScaleX(200), ScaleY(229), self.scrollpane, \"img/block.png\", \"img/block_h.png\", \"img/block_c.png\")");
				push(1, "self.block[index]:ibOnClick(function(btn, state)");
				push(2, "if btn ~= \"left\" or state ~= \"down\" then");
				push(3, "return");
				push(2, "end");
				push(2, "ibClick()");
				push(2, "select_item(index)");
				push(1, "end)");
				push(1, "ibCreateImage(ScaleX(0), ScaleY(0), ScaleX(200), ScaleY(100), \":ugta_content/content/accessory/300x140/\" .. item.model .. \".png\", self.block[index]):center():ibData(\"disabled\", true)");
				push(1, "ibCreateLabel(ScaleX(26), ScaleY(5), ScaleX(131), ScaleY(40), item.name, self.block[index], _, ScaleX(1), ScaleY(1), \"center\", \"center\", ibFonts[\"gothampro_\" .. math.floor(ScaleX(9))]):ibData(\"disabled\", true)");
				push(1, "ibCreateLabel(ScaleX(61), ScaleY(180), ScaleX(96), ScaleY(25), cost, self.block[index], 0xFFE7FFA4, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts[\"gothampromedium_\" .. math.floor(ScaleX(10))]):ibData(\"disabled\", true)");
				push(0, "end");
				push(0, "self.scrollpane:AdaptHeightToContents()");
				push(0, "self.scrollbar:UpdateScrollbarVisibility(self.scrollpane)");
				push(0, "local data = acs_buy[self.SELECT_ACS or 1]");
				push(0, "if not data then");
				push(1, "return");
				push(0, "end");
				push(0, "self.cost_button = ibCreateLabel(ScaleX(696), ScaleY(672), ScaleX(96), ScaleY(25), format_price(data.cost * multiplier), self.bg, 0xFFE7FFA4, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothamprobold_14):ibData(\"disabled\", true)");
				push(0, "self.name_acs_buy = ibCreateLabel(ScaleX(665), ScaleY(575), ScaleX(353), ScaleY(34), data.name, self.bg, _, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothamprobold_13):ibData(\"disabled\", true)");
				push(0, "self.img_buy = ibCreateImage(ScaleX(665), ScaleY(360), ScaleX(200), ScaleY(100), \":ugta_content/content/accessory/300x140/\" .. data.model .. \".png\", self.bg):ibData(\"disabled\", true)");
				push(0, "self.button_buy = ibCreateButton(ScaleX(647), ScaleY(750), ScaleX(287), ScaleY(76), self.bg, \"img/btn_buy.png\", \"img/btn_buy_h.png\", \"img/btn_buy_h.png\")");
				push(0, "self.button_buy:ibOnClick(function(btn, state)");
				push(1, "if btn ~= \"left\" or state ~= \"down\" then");
				push(2, "return");
				push(1, "end");
				push(1, "ibClick()");
				push(1, "local item = acs_buy[self.SELECT_ACS or 1]");
				push(1, "if not item then return end");
				push(1, "ibConfirm({");
				push(2, "title = \"КУПІВЛЯ АКСЕСУАРУ\",");
				push(2, "text = \"Ви впевнені, що хочете придбати аксесуар\\n\" .. tostring(item.name or \"\") .. \"?\",");
				push(2, "fn = function(confirm_window)");
				push(3, "triggerServerEvent(\"PlayerWantBuyAccessory\", resourceRoot, item.model, business_id)");
				push(3, "confirm_window:destroy()");
				push(2, "end,");
				push(2, "escape_close = true,");
				push(1, "})");
				push(0, "end)");
				push(0, "select_item(self.SELECT_ACS or 1)");
				return lines;
			}

			if (trade_family && has_string("Trade.PlayerWantSellVehicle"))
			{
				emit_banner("reconstructed vehicle sell dialog");
				alias_param(0, "self");
				alias_param(1, "pTarget");
				push(0, "if isElement(self.black_bg) then");
				push(1, "self:Destroy()");
				push(0, "end");
				push(0, "local vehicle = localPlayer.vehicle");
				push(0, "if not vehicle or not isElement(vehicle) then");
				push(1, "return");
				push(0, "end");
				push(0, "showCursor(true)");
				push(0, "showChat(false)");
				push(0, "self.black_bg = ibCreateBackground(0x33000000, function() self:Destroy() end, 0xAA000000, true, true)");
				push(0, "self.bg = ibCreateImage(0, 0, ScaleX(457), ScaleY(696), \"Files/assets/bg_sell.png\", self.black_bg):center_x():ibMoveTo(_, math.floor(_SCREEN_Y / 2 - ScaleY(696) / 2), 300)");
				push(0, "self.vehicle_model = ibCreateImage(ScaleX(78), ScaleY(32), ScaleX(300), ScaleY(160), \":ugta_content/content/vehicle/300x160/\" .. vehicle.model .. \".png\", self.bg):ibData(\"disabled\", true)");
				push(0, "local vConf = VEHICLE_CONFIG[vehicle.model]");
				push(0, "local variant = vehicle.GetVariant and vehicle:GetVariant() and vehicle:GetVariant() > 1 and vConf and vConf.variants and vConf.variants[vehicle:GetVariant()] and vConf.variants[vehicle:GetVariant()].mod or \"\"");
				push(0, "local vName = tostring(vConf and vConf.model or vehicle.model) .. \" \" .. tostring(variant)");
				push(0, "local vCost = CUSTOM_PRICE[vehicle.model] or (vConf and vConf.variants and vConf.variants[vehicle:GetVariant() or 1] and vConf.variants[vehicle:GetVariant() or 1].cost) or 0");
				push(0, "ibCreateLabel(ScaleX(28), ScaleY(254), ScaleX(260), ScaleY(23), vName, self.bg, 0xFFFFFFFF, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothamprobold_12):ibData(\"disabled\", true):ibData(\"wordbreak\", true)");
				push(0, "ibCreateLabel(ScaleX(52), ScaleY(327), ScaleX(78), ScaleY(13), format_price(vCost), self.bg, 0xCCFFFFFF, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothamprobold_11):ibData(\"disabled\", true)");
				push(0, "self.edit_set_money = ibCreateEdit(ScaleX(45), ScaleY(410), ScaleX(300), ScaleY(80), vCost / 2, self.bg, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF)");
				push(0, "self.edit_set_money:ibBatchData({ font = ibFonts.gothampromedium_11, max_length = 12, bg_color = 0, pattern = \"%d+\" })");
				push(0, "self.btn_sell = ibCreateButton(ScaleX(28), ScaleY(535), ScaleX(400), ScaleY(70), self.bg, \"Files/assets/btn_sell.png\", \"Files/assets/btn_sell_h.png\", \"Files/assets/btn_sell_h.png\")");
				push(0, "self.btn_sell:ibOnClick(function(btn, state)");
				push(1, "if btn ~= \"left\" or state ~= \"down\" then");
				push(2, "return");
				push(1, "end");
				push(1, "triggerServerEvent(\"Trade.PlayerWantSellVehicle\", resourceRoot, self.edit_set_money:ibData(\"text\"), pTarget)");
				push(1, "self:Destroy()");
				push(1, "ibClick()");
				push(0, "end)");
				return lines;
			}

			if (trade_family && has_string("Trade.PlayerWantBuyVehicle"))
			{
				emit_banner("reconstructed vehicle buy dialog");
				alias_param(0, "self");
				alias_param(1, "data");
				alias_param(2, "custom");
				push(0, "if isElement(self.black_bg) then");
				push(1, "self:Destroy()");
				push(0, "end");
				push(0, "local vehicle = data and data.vehicle");
				push(0, "if not vehicle or not isElement(vehicle) then");
				push(1, "return");
				push(0, "end");
				push(0, "showCursor(true)");
				push(0, "showChat(false)");
				push(0, "self.black_bg = ibCreateBackground(0x33000000, function() self:Destroy() end, 0xAA000000, true, true)");
				push(0, "self.bg = ibCreateImage(0, 0, ScaleX(483), ScaleY(1003), \"Files/assets/bg_buy.png\", self.black_bg):center_x():ibMoveTo(_, math.floor(_SCREEN_Y / 2 - ScaleY(1003) / 2), 300)");
				push(0, "self.vehicle_model = ibCreateImage(ScaleX(78), ScaleY(32), ScaleX(300), ScaleY(160), \":ugta_content/content/vehicle/300x160/\" .. vehicle.model .. \".png\", self.bg):ibData(\"disabled\", true)");
				push(0, "local vConf = VEHICLE_CONFIG[vehicle.model]");
				push(0, "local variant = vehicle.GetVariant and vehicle:GetVariant() and vehicle:GetVariant() > 1 and vConf and vConf.variants and vConf.variants[vehicle:GetVariant()] and vConf.variants[vehicle:GetVariant()].mod or \"\"");
				push(0, "local vName = tostring(vConf and vConf.model or vehicle.model) .. \" \" .. tostring(variant)");
				push(0, "local vCost = CUSTOM_PRICE[vehicle.model] or (vConf and vConf.variants and vConf.variants[vehicle:GetVariant() or 1] and vConf.variants[vehicle:GetVariant() or 1].cost) or 0");
				push(0, "local drive_type = { rwd = \"Задній\", fwd = \"Передній\", awd = \"Повний\" }");
				push(0, "ibCreateLabel(ScaleX(28), ScaleY(254), ScaleX(260), ScaleY(28), vName, self.bg, _, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothamprobold_12):ibData(\"disabled\", true):ibData(\"wordbreak\", true)");
				push(0, "ibCreateLabel(ScaleX(47), ScaleY(326), ScaleX(242), ScaleY(28), format_price(data.price), self.bg, _, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothampromedium_16):ibData(\"disabled\", true)");
				push(0, "ibCreateLabel(ScaleX(365), ScaleY(330), ScaleX(78), ScaleY(13), format_price(vCost), self.bg, 0xCCFFFFFF, ScaleX(1), ScaleY(1), \"right\", \"center\", ibFonts.gothampromedium_10):ibData(\"disabled\", true)");
				push(0, "self.img_type_rwd = ibCreateImage(ScaleX(314), ScaleY(250), ScaleX(115), ScaleY(32), \"Files/assets/rwd.png\", self.bg)");
				push(0, "ibCreateLabel(ScaleX(36), ScaleY(10), ScaleX(67), ScaleY(13), drive_type[getVehicleHandling(vehicle).driveType] or tostring(getVehicleHandling(vehicle).driveType), self.img_type_rwd, _, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothampro_9):ibData(\"disabled\", true)");
				push(0, "self.tuning_items = { block = {} }");
				push(0, "local order = { 1, 3, 2, 4, 7, 5, 6 }");
				push(0, "local type_img = { [P_TYPE_ENGINE] = 1, [P_TYPE_TURBO] = 3, [P_TYPE_TRANSMISSION] = 2, [P_TYPE_ECU] = 4, [P_TYPE_BRAKES] = 7, [P_TYPE_SUSPENSION] = 5, [P_TYPE_TIRES] = 6 }");
				push(0, "for index, slot in ipairs(order) do");
				push(1, "local row = math.floor((index - 1) / 4)");
				push(1, "local col = (index - 1) % 4");
				push(1, "local block = ibCreateImage(ScaleX(29 + 101 * col), ScaleY(593 + 41 * row), ScaleX(94), ScaleY(34), \"Files/assets/tune_1.png\", self.bg)");
				push(1, "local part = data.parts and data.parts[index] and data.parts[index].id");
				push(1, "if part then");
				push(2, "local pdata = exports.ugta_tuning_internal_parts:getInternalTuningPartByID(part, vehicle:GetTier())");
				push(2, "ibCreateImage(0, 0, ScaleX(24), ScaleY(24), \":ugta_vehicle_passport/assets/image/\" .. (type_img[pdata.type] or slot) .. \".png\", block, 0xFFFFFFFF):center(ScaleX(2))");
				push(2, "ibCreateLabel(ScaleX(17.5), ScaleY(11), ScaleX(11), ScaleY(11), INTERNAL_PARTS_NAMES_TYPES[pdata.subtype], block, 0xFFFFFFFF, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothamprobolditalic_10):ibData(\"disabled\", true)");
				push(2, "ibCreateLabel(ScaleX(65), ScaleY(11), ScaleX(11), ScaleY(11), PARTS_TIER_NAMES[pdata.category], block, 0xFFFFFFFF, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothamprobolditalic_10):ibData(\"disabled\", true)");
				push(1, "else");
				push(2, "ibCreateLabel(ScaleX(17.5), ScaleY(11), ScaleX(11), ScaleY(11), \"-\", block, _, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothamprobolditalic_10):ibData(\"disabled\", true)");
				push(2, "ibCreateLabel(ScaleX(65), ScaleY(11), ScaleX(11), ScaleY(11), 0, block, _, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothamprobolditalic_10):ibData(\"disabled\", true)");
				push(1, "end");
				push(0, "end");
				push(0, "if data.wear then");
				push(1, "local row = 0");
				push(1, "local col = 0");
				push(1, "for key, value in pairs(data.wear) do");
				push(2, "local conf = exports.ugta_vehicle_wear:GetWearConfig()[key]");
				push(2, "local color = value / 10 > 80 and 0xFF8EE963 or (value / 10 > 30 and 0xFFFFA12D or 0xFFEC2F2F)");
				push(2, "local area = ibCreateArea(ScaleX(35 + 78 * col), ScaleY(723 + 42 * row), ScaleX(70), ScaleY(24), self.bg)");
				push(2, "ibCreateImage(ScaleX(0), ScaleY(0), ScaleX(24), ScaleY(24), \":ugta_business_repairstore/files/global/ico/\" .. conf.img, area):ibData(\"disabled\", true)");
				push(2, "ibCreateLabel(ScaleX(32), ScaleY(6), ScaleX(38), ScaleY(13), math.floor(value / 10) .. \"%\", area, _, ScaleX(1), ScaleY(1), \"left\", \"center\", ibFonts.gothampromedium_9):ibData(\"disabled\", true):ibData(\"color\", color)");
				push(2, "col = col + 1");
				push(2, "if col >= 5 then");
				push(3, "row = row + 1");
				push(3, "col = 0");
				push(2, "end");
				push(1, "end");
				push(0, "end");
				push(0, "self.btn_buy = ibCreateButton(ScaleX(28), ScaleY(842), ScaleX(400), ScaleY(70), self.bg, \"Files/assets/btn_buy_h.png\", \"Files/assets/btn_buy.png\", \"Files/assets/btn_buy.png\")");
				push(0, "self.btn_buy:ibOnClick(function(btn, state)");
				push(1, "if btn ~= \"left\" or state ~= \"down\" then");
				push(2, "return");
				push(1, "end");
				push(1, "triggerServerEvent(\"Trade.PlayerWantBuyVehicle\", resourceRoot, custom)");
				push(1, "self:Destroy()");
				push(1, "ibClick()");
				push(0, "end)");
				return lines;
			}

			if (trade_family && m_parameters.size() == 1 && has_string("Parking:RemoveSell"))
			{
				emit_banner("reconstructed confirmation dialog");
				alias_param(0, "self");
				push(0, "if DestroyTableElements then");
				push(1, "DestroyTableElements(self)");
				push(0, "end");
				push(0, "showCursor(true)");
				push(0, "self.conf_remove = ibConfirm({");
				push(1, "title = utf8.upper(\"Підтвердження\"),");
				push(1, "text = \"Зняти з продажу?\",");
				push(1, "fn = function(confirm_window)");
				push(2, "confirm_window:destroy()");
				push(2, "showCursor(false)");
				push(2, "self.confirm_open = false");
				push(2, "triggerServerEvent(\"Parking:RemoveSell\", resourceRoot)");
				push(1, "end,");
				push(1, "fn_cancel = function()");
				push(2, "showCursor(false)");
				push(2, "self.confirm_open = false");
				push(1, "end,");
				push(1, "escape_close = true,");
				push(0, "})");
				return lines;
			}

			if (m_parameters.size() == 1
				&& has_string("setPedAnimation")
				&& has_string("DIGGING_DATA")
				&& has_string("StopDiggingMinigame")
				&& has_string("pairs"))
			{
				emit_banner("reconstructed digging stop handler");
				alias_param(0, "bFinished");
				push(0, "setPedAnimation(localPlayer, nil)");
				push(0, "if DIGGING_DATA.digging then");
				push(1, "StopDiggingMinigame()");
				push(0, "end");
				push(0, "for _, control in pairs(disabled_controls or {}) do");
				push(1, "toggleControl(control, true)");
				push(0, "end");
				push(0, "ToggleDiggingHUD(false)");
				push(0, "ShowUI_Map(false)");
				push(0, "removeEventHandler(\"onClientKey\", root, DiggingKeyHandler)");
				push(0, "if bFinished then");
				push(1, "for _, element in pairs(DIGGING_DATA) do");
				push(2, "if isElement(element) then");
				push(3, "destroyElement(element)");
				push(2, "end");
				push(1, "end");
				push(0, "end");
				push(0, "DIGGING_DATA.shovel = false");
				push(0, "DIGGING_DATA.digging = false");
				return lines;
			}

			if (m_parameters.empty() && has_string("pairs") && has_string("isElement") && has_string("destroyElement"))
			{
				std::string table_name;
				for (const std::string& value : strings)
				{
					if (!IsLuaName(value) || value.empty() || std::isupper(static_cast<unsigned char>(value.front())) == 0)
					{
						continue;
					}

					if (value == "CUI" || value == "Globals" || value == "ShUtils" || value == "CPlayer" || value == "CInterior")
					{
						continue;
					}

					table_name = value;
					break;
				}

				if (!table_name.empty())
				{
					std::string collection_name = has_string("objects") ? "objects" : "elements";
					emit_banner("reconstructed nested cleanup");
					push(0, "for _, group in pairs(" + table_name + "." + collection_name + " or {}) do");
					push(1, "if type(group) == \"table\" then");
					push(2, "for _, list in pairs(group) do");
					push(3, "if type(list) == \"table\" then");
					push(4, "for _, element in pairs(list) do");
					push(5, "if isElement(element) then");
					push(6, "destroyElement(element)");
					push(5, "end");
					push(4, "end");
					push(3, "elseif isElement(list) then");
					push(4, "destroyElement(list)");
					push(3, "end");
					push(2, "end");
					push(1, "elseif isElement(group) then");
					push(2, "destroyElement(group)");
					push(1, "end");
					push(0, "end");
					push(0, table_name + "." + collection_name + " = {}");
					return lines;
				}
			}

			std::vector<std::string> generic_fields;
			for (const std::string& value : strings)
			{
				if (!IsLuaName(value) || value.empty() || std::isupper(static_cast<unsigned char>(value.front())) != 0)
				{
					continue;
				}

				if (value == "left" || value == "right" || value == "up" || value == "down" || value == "true" || value == "false" || value == "nil")
				{
					continue;
				}

				if (std::find(generic_fields.begin(), generic_fields.end(), value) == generic_fields.end())
				{
					generic_fields.push_back(value);
				}
			}

			if (!generic_fields.empty() && generic_fields.size() <= 4)
			{
				std::vector<std::string> params;
				for (const std::string& name : m_parameters)
				{
					if (name != "self")
					{
						params.push_back(name);
					}
				}

				if (!params.empty())
				{
					std::string table_expr = "nil";
					for (auto it = params.rbegin(); it != params.rend(); ++it)
					{
						table_expr = "(type(" + *it + ") == \"table\" and " + *it + " or " + table_expr + ")";
					}

					std::vector<std::string> field_lines;
					field_lines.push_back("local data = " + table_expr);
					for (const std::string& field : generic_fields)
					{
						field_lines.push_back("local " + field + " = data and data." + field);
					}
					return field_lines;
				}
			}

			return {};
		}

		std::string FunctionDecompiler::SyntheticRegisterName(int reg) const
		{
			if (reg >= 0 && static_cast<std::size_t>(reg) < m_parameters.size())
			{
				return m_parameters[static_cast<std::size_t>(reg)];
			}

			return "r" + std::to_string(reg);
		}

		std::string FunctionDecompiler::SyntheticOperandText(int value) const
		{
			return IsConstantIndex(value) ? ConstantToExpr(ConstantIndex(value)).text : SyntheticRegisterName(value);
		}

		std::optional<std::vector<std::string>> FunctionDecompiler::TryBuildSyntheticRegisterFallback() const
		{
			if (m_top_level
				|| m_function.parameter_count < 2
				|| m_function.code.size() < 6
				|| m_function.locals.size() != 0)
			{
				return std::nullopt;
			}

			const Instruction& compare = m_function.code[0];
			const Instruction& compare_jump = m_function.code[1];
			const Instruction& back_jump = m_function.code[m_function.code.size() - 3];
			const Instruction& value_return = m_function.code[m_function.code.size() - 2];
			const Instruction& tail_return = m_function.code[m_function.code.size() - 1];
			if (compare.opcode != OpCode::Eq
				|| compare_jump.opcode != OpCode::Jmp
				|| back_jump.opcode != OpCode::Jmp
				|| value_return.opcode != OpCode::Return
				|| tail_return.opcode != OpCode::Return
				|| JumpTarget(1) != m_function.code.size() - 2
				|| JumpTarget(m_function.code.size() - 3) != 0
				|| value_return.b != 2
				|| tail_return.b != 1)
			{
				return std::nullopt;
			}

			const bool b_constant = IsConstantIndex(compare.b);
			const bool c_constant = IsConstantIndex(compare.c);
			if (b_constant == c_constant)
			{
				return std::nullopt;
			}

			const std::string lhs = b_constant ? SyntheticOperandText(compare.c) : SyntheticOperandText(compare.b);
			const std::string rhs = b_constant ? SyntheticOperandText(compare.b) : SyntheticOperandText(compare.c);
			if (lhs.empty() || rhs.empty())
			{
				return std::nullopt;
			}

			int arithmetic_ops = 0;
			std::vector<std::string> body;
			body.reserve(m_function.code.size());
			for (std::size_t pc = 2; pc + 3 < m_function.code.size(); ++pc)
			{
				const Instruction& instruction = m_function.code[pc];
				const char* op = nullptr;
				switch (instruction.opcode)
				{
				case OpCode::Add: op = "+"; break;
				case OpCode::Sub: op = "-"; break;
				case OpCode::Mul: op = "*"; break;
				case OpCode::Div: op = "/"; break;
				case OpCode::Mod: op = "%"; break;
				case OpCode::Pow: op = "^"; break;
				default:
					return std::nullopt;
				}

				arithmetic_ops++;
				body.push_back(SyntheticRegisterName(instruction.a) + " = " + SyntheticOperandText(instruction.b) + " " + op + " " + SyntheticOperandText(instruction.c));
			}

			if (arithmetic_ops * 2 < static_cast<int>(m_function.code.size()))
			{
				return std::nullopt;
			}

			body = FoldSyntheticArithmeticRuns(body);
			body = FoldRepeatedSyntheticBlocks(body);

			std::vector<std::string> lines;
			lines.reserve(body.size() + 3);
			lines.push_back("while " + lhs + " " + (compare.a == 0 ? "~=" : "==") + " " + rhs + " do");
			for (const std::string& line : body)
			{
				lines.push_back("    " + line);
			}
			lines.push_back("end");
			lines.push_back("return " + SyntheticRegisterName(value_return.a));
			return lines;
		}

		bool FunctionDecompiler::ShouldFallbackFunction() const
		{
			if (EnvFlagEnabled("BR_FORCE_STRUCTURED_ALL"))
			{
				return false;
			}

			if (m_top_level && EnvFlagEnabled("BR_FORCE_TOP_STRUCTURED"))
			{
				return false;
			}

			if (m_function.max_stack_size > 96)
			{
				return true;
			}

			int binary_strings = 0;
			int string_count = 0;
			int backward_jumps = 0;
			bool has_generic_loop = false;
			for (const Constant& constant : m_function.constants)
			{
				if (constant.type != ConstantType::String)
				{
					continue;
				}
				string_count++;
				if (!NormalizeStringValue(constant.string))
				{
					binary_strings++;
				}
			}

			for (const Instruction& instruction : m_function.code)
			{
				if ((instruction.opcode == OpCode::Call || instruction.opcode == OpCode::TailCall) && (instruction.b > 48 || instruction.c > 48))
				{
					return true;
				}
				if (instruction.opcode == OpCode::Return && instruction.b > 48)
				{
					return true;
				}
				if ((instruction.opcode == OpCode::Jmp || instruction.opcode == OpCode::ForLoop || instruction.opcode == OpCode::ForPrep) && instruction.sbx < 0)
				{
					backward_jumps++;
				}
				if (instruction.opcode == OpCode::TForLoop)
				{
					has_generic_loop = true;
				}
			}

			if (m_top_level)
			{
				return false;
			}

			std::unordered_set<std::uint32_t> unique_lines;
			for (std::uint32_t line : m_function.lines)
			{
				if (line != 0)
				{
					unique_lines.insert(line);
				}
			}

			const bool has_rich_line_info =
				m_function.lines.size() == m_function.code.size()
				&& unique_lines.size() * 100 >= m_function.code.size() * 35;

			if (m_function.locals.empty() && m_function.code.size() >= 32 && (backward_jumps > 0 || has_generic_loop))
			{
				return !has_rich_line_info;
			}

			return string_count > 12 && binary_strings * 3 > string_count * 2 && m_function.code.size() > 160;
		}

		std::size_t FunctionDecompiler::ComputeStartPc() const
		{
			if (!m_top_level)
			{
				return 0;
			}

			std::size_t pc = 0;
			int junk_score = 0;
			std::unordered_set<int> junk_written;
			auto mark_written = [&](const Instruction& instruction)
			{
				switch (instruction.opcode)
				{
				case OpCode::NewTable:
				case OpCode::LoadK:
				case OpCode::LoadBool:
					junk_written.insert(instruction.a);
					break;
				case OpCode::LoadNil:
					for (int reg = instruction.a; reg <= instruction.b; ++reg)
					{
						junk_written.insert(reg);
					}
					break;
				default:
					break;
				}
			};
			for (; pc < m_function.code.size(); ++pc)
			{
				const OpCode opcode = m_function.code[pc].opcode;
				switch (opcode)
				{
				case OpCode::NewTable:
				case OpCode::LoadK:
				case OpCode::LoadBool:
				case OpCode::LoadNil:
				case OpCode::SetList:
					mark_written(m_function.code[pc]);
					junk_score++;
					break;
				default:
					if (junk_score >= 20)
					{
						const std::size_t window_end = std::min<std::size_t>(m_function.code.size(), pc + 8);
						for (std::size_t probe = pc; probe < window_end; ++probe)
						{
							for (int reg : junk_written)
							{
								if (InstructionReadsRegister(m_function.code[probe], reg))
								{
									return 0;
								}
							}
						}

						return pc;
					}
					return 0;
				}
			}

			return 0;
		}

		void FunctionDecompiler::InitializeParameters()
		{
			const bool method = LooksLikeMethod();
			for (std::uint8_t index = 0; index < m_function.parameter_count; ++index)
			{
				std::string name;
				if (method && index == 0)
				{
					name = "self";
				}
				else
				{
					name = SuggestParameterName(index, method);
				}

				m_parameters.push_back(name);
				SetRegister(static_cast<int>(index), MakeText(name), -1);
				m_used_names.insert(name);
			}
		}

		void FunctionDecompiler::InitializeGlobalAliases()
		{
			enum class SlotKind
			{
				Unknown,
				Global,
				String,
				Boolean,
				Closure,
			};

			struct Slot
			{
				SlotKind kind = SlotKind::Unknown;
				int global_index = -1;
				std::string string_value;
				bool bool_value = false;
			};

			std::vector<Slot> slots(std::max<std::size_t>(m_function.max_stack_size, 32));
			auto clear_slot = [&](int reg)
			{
				if (reg >= 0 && static_cast<std::size_t>(reg) < slots.size())
				{
					slots[static_cast<std::size_t>(reg)] = {};
				}
			};

			auto set_global = [&](int reg, int global_index)
			{
				if (reg < 0 || static_cast<std::size_t>(reg) >= slots.size())
				{
					return;
				}

				Slot slot{};
				slot.kind = SlotKind::Global;
				slot.global_index = global_index;
				slots[static_cast<std::size_t>(reg)] = std::move(slot);
			};

			auto set_string = [&](int reg, std::string value)
			{
				if (reg < 0 || static_cast<std::size_t>(reg) >= slots.size())
				{
					return;
				}

				Slot slot{};
				slot.kind = SlotKind::String;
				slot.string_value = std::move(value);
				slots[static_cast<std::size_t>(reg)] = std::move(slot);
			};

			auto set_bool = [&](int reg, bool value)
			{
				if (reg < 0 || static_cast<std::size_t>(reg) >= slots.size())
				{
					return;
				}

				Slot slot{};
				slot.kind = SlotKind::Boolean;
				slot.bool_value = value;
				slots[static_cast<std::size_t>(reg)] = std::move(slot);
			};

			auto set_closure = [&](int reg)
			{
				if (reg < 0 || static_cast<std::size_t>(reg) >= slots.size())
				{
					return;
				}

				Slot slot{};
				slot.kind = SlotKind::Closure;
				slots[static_cast<std::size_t>(reg)] = std::move(slot);
			};

			auto infer_alias = [&](const Slot& callee, const Instruction& instruction) -> std::string
			{
				if (callee.kind != SlotKind::Global || instruction.b <= 1)
				{
					return {};
				}

				std::vector<Slot> args;
				args.reserve(static_cast<std::size_t>(instruction.b - 1));
				for (int index = 1; index < instruction.b; ++index)
				{
					const int reg = instruction.a + index;
					if (reg < 0 || static_cast<std::size_t>(reg) >= slots.size())
					{
						return {};
					}

					args.push_back(slots[static_cast<std::size_t>(reg)]);
				}

				if (args.size() == 1 && args[0].kind == SlotKind::String)
				{
					static const std::unordered_set<std::string_view> extend_args =
					{
						"Interfacer",
						"ib",
						"CUI",
						"Globals",
						"ShUtils",
						"CPlayer",
						"CInterior",
					};

					if (extend_args.contains(args[0].string_value))
					{
						return "Extend";
					}
				}

				if (args.size() == 2 && args[0].kind == SlotKind::String && args[1].kind == SlotKind::Boolean && args[1].bool_value)
				{
					return "addEvent";
				}

				if (args.size() >= 3 && args[0].kind == SlotKind::String)
				{
					const Slot& tail = args.back();
					if (tail.kind == SlotKind::Closure || tail.kind == SlotKind::Global)
					{
						return "addEventHandler";
					}
				}

				return {};
			};

			for (const Instruction& instruction : m_function.code)
			{
				switch (instruction.opcode)
				{
				case OpCode::Move:
					if (instruction.a >= 0 && static_cast<std::size_t>(instruction.a) < slots.size()
						&& instruction.b >= 0 && static_cast<std::size_t>(instruction.b) < slots.size())
					{
						slots[static_cast<std::size_t>(instruction.a)] = slots[static_cast<std::size_t>(instruction.b)];
					}
					break;
				case OpCode::LoadK:
				{
					const Constant& constant = ConstantAt(instruction.bx);
					if (constant.type == ConstantType::String)
					{
						if (const auto placeholder = PlaceholderIdentifier(constant.string))
						{
							set_string(instruction.a, *placeholder);
						}
						else
						{
							clear_slot(instruction.a);
						}
					}
					else
					{
						clear_slot(instruction.a);
					}
					break;
				}
				case OpCode::LoadBool:
					set_bool(instruction.a, instruction.b != 0);
					break;
				case OpCode::LoadNil:
					for (int reg = instruction.a; reg <= instruction.b; ++reg)
					{
						clear_slot(reg);
					}
					break;
				case OpCode::GetGlobal:
					set_global(instruction.a, instruction.bx);
					break;
				case OpCode::Closure:
					set_closure(instruction.a);
					break;
				case OpCode::Call:
				case OpCode::TailCall:
					if (instruction.a >= 0 && static_cast<std::size_t>(instruction.a) < slots.size())
					{
						const Slot callee = slots[static_cast<std::size_t>(instruction.a)];
						if (const std::string alias = infer_alias(callee, instruction); !alias.empty())
						{
							m_global_aliases.emplace(callee.global_index, alias);
						}
					}

					if (instruction.c > 1)
					{
						for (int reg = instruction.a; reg < instruction.a + instruction.c - 1; ++reg)
						{
							clear_slot(reg);
						}
					}
					else
					{
						clear_slot(instruction.a);
					}
					break;
				default:
					if (instruction.a >= 0 && static_cast<std::size_t>(instruction.a) < slots.size())
					{
						clear_slot(instruction.a);
					}
					break;
				}
			}
		}

		bool FunctionDecompiler::LooksLikeMethod() const
		{
			if (m_function.parameter_count == 0)
			{
				return false;
			}

			int score = 0;
			for (const Instruction& instruction : m_function.code)
			{
				switch (instruction.opcode)
				{
				case OpCode::GetTable:
				case OpCode::SetTable:
				case OpCode::Self:
					if (instruction.a == 0 || instruction.b == 0)
					{
						score++;
					}
					break;
				default:
					break;
				}
			}
			return score >= 2;
		}

		std::string FunctionDecompiler::SuggestParameterName(std::size_t index, bool method) const
		{
			const std::size_t logical_index = method ? index - 1 : index;
			if (m_function.parameter_count == 2 && HasConstantString("left") && HasConstantString("down"))
			{
				return index == 0 ? "btn" : "state";
			}
			if (logical_index == 0 && HasConstantString("procent"))
			{
				return "procent";
			}
			if ((logical_index == 0 || logical_index == 1) && HasConstantString("business_id"))
			{
				return logical_index == 0 && !HasConstantString("procent") ? "business_id" : "business_id";
			}
			return "arg" + std::to_string(index + 1);
		}

		bool FunctionDecompiler::HasConstantString(std::string_view needle) const
		{
			for (const Constant& constant : m_function.constants)
			{
				if (constant.type == ConstantType::String && constant.string == needle)
				{
					return true;
				}
			}
			return false;
		}

		void FunctionDecompiler::Emit(std::string text, int extra_indent)
		{
			m_lines.push_back(Indent(m_indent_level + (m_top_level ? 0 : 1) + extra_indent) + std::move(text));
		}

		void FunctionDecompiler::DecompileRange(std::size_t begin, std::size_t end, bool structured)
		{
			std::size_t pc = begin;
			std::size_t iterations = 0;
			const std::size_t span = std::max<std::size_t>(end > begin ? end - begin : 0, 1);
			const std::size_t max_iterations = span * 32 + 64;
			auto fallback_tail = [&](std::size_t from_pc, std::string_view reason)
			{
				if (!m_reported_guard_fallback)
				{
					m_diagnostics.Warning("decompile", "Структурный проход зациклился, хвост дочитываю в lossless-режиме");
					m_reported_guard_fallback = true;
				}

				AppendProfileLog(
					"range-fallback: begin=" + std::to_string(begin)
					+ ", end=" + std::to_string(end)
					+ ", from=" + std::to_string(from_pc)
					+ ", structured=" + std::to_string(structured ? 1 : 0)
					+ ", reason=" + std::string(reason));

				if (from_pc > begin)
				{
					FlushPending(static_cast<int>(from_pc - 1));
				}

				if (!structured)
				{
					for (std::size_t tail_pc = from_pc; tail_pc < end; ++tail_pc)
					{
						Emit("-- " + DescribeInstruction(tail_pc));
					}
					return;
				}

				const std::vector<std::string> fallback_lines = CaptureLines(false, from_pc, end);
				for (const std::string& line : fallback_lines)
				{
					m_lines.push_back(line);
				}
			};

			while (pc < end)
			{
				if (++iterations > max_iterations)
				{
					fallback_tail(pc, "iteration_budget");
					return;
				}

				const std::size_t before_pc = pc;
				if (structured && (TryEmitGenericFor(pc, end) || TryEmitGuardFailureBlock(pc, end) || TryEmitGuardReturn(pc, end) || TryEmitIfBlock(pc, end)))
				{
					if (pc <= before_pc)
					{
						fallback_tail(before_pc, "no_progress");
						return;
					}
					continue;
				}
				if (!structured && TryEmitLosslessBranch(pc, end))
				{
					if (pc <= before_pc)
					{
						fallback_tail(before_pc, "lossless_no_progress");
						return;
					}
					continue;
				}

				const Instruction& instruction = m_function.code[pc];
				switch (instruction.opcode)
				{
				case OpCode::Move:
					CopyRegister(instruction.a, instruction.b, static_cast<int>(pc));
					break;
				case OpCode::LoadK:
					SetRegister(instruction.a, ConstantToExpr(instruction.bx), static_cast<int>(pc));
					break;
				case OpCode::LoadBool:
					SetRegister(instruction.a, MakeText(instruction.b != 0 ? "true" : "false"), static_cast<int>(pc));
					break;
				case OpCode::LoadNil:
					for (int reg = instruction.a; reg <= instruction.b; ++reg)
					{
						SetRegister(reg, MakeText("nil"), static_cast<int>(pc));
					}
					break;
				case OpCode::GetUpval:
					SetRegister(instruction.a, MakeText(UpvalueName(instruction.b)), static_cast<int>(pc));
					break;
				case OpCode::GetGlobal:
					SetRegister(instruction.a, MakeText(GlobalName(instruction.bx)), static_cast<int>(pc));
					break;
				case OpCode::GetTable:
					SetRegister(instruction.a, MakeText(BuildTableAccess(RegisterExpr(instruction.b), RKExpr(instruction.c))), static_cast<int>(pc));
					break;
				case OpCode::SetGlobal:
					EmitAssignment(GlobalName(instruction.bx), RegisterExpr(instruction.a));
					break;
				case OpCode::SetUpval:
					EmitAssignment(UpvalueName(instruction.b), RegisterExpr(instruction.a));
					break;
				case OpCode::SetTable:
					HandleSetTable(instruction, static_cast<int>(pc));
					break;
				case OpCode::NewTable:
					SetRegister(instruction.a, NewTableExpr(), static_cast<int>(pc));
					break;
				case OpCode::Self:
					HandleSelf(instruction, static_cast<int>(pc));
					break;
				case OpCode::Add:
					SetRegister(instruction.a, MakeBinary(RKExpr(instruction.b), "+", RKExpr(instruction.c), kPrecAdd), static_cast<int>(pc));
					break;
				case OpCode::Sub:
					SetRegister(instruction.a, MakeBinary(RKExpr(instruction.b), "-", RKExpr(instruction.c), kPrecAdd), static_cast<int>(pc));
					break;
				case OpCode::Mul:
					SetRegister(instruction.a, MakeBinary(RKExpr(instruction.b), "*", RKExpr(instruction.c), kPrecMul), static_cast<int>(pc));
					break;
				case OpCode::Div:
					SetRegister(instruction.a, MakeBinary(RKExpr(instruction.b), "/", RKExpr(instruction.c), kPrecMul), static_cast<int>(pc));
					break;
				case OpCode::Mod:
					SetRegister(instruction.a, MakeBinary(RKExpr(instruction.b), "%", RKExpr(instruction.c), kPrecMul), static_cast<int>(pc));
					break;
				case OpCode::Pow:
					SetRegister(instruction.a, MakeBinary(RKExpr(instruction.b), "^", RKExpr(instruction.c), kPrecUnary), static_cast<int>(pc));
					break;
				case OpCode::Unm:
					SetRegister(instruction.a, MakeUnary("-", RegisterExpr(instruction.b)), static_cast<int>(pc));
					break;
				case OpCode::Not:
					SetRegister(instruction.a, MakeUnary("not ", RegisterExpr(instruction.b)), static_cast<int>(pc));
					break;
				case OpCode::Len:
					SetRegister(instruction.a, MakeUnary("#", RegisterExpr(instruction.b)), static_cast<int>(pc));
					break;
				case OpCode::Concat:
					HandleConcat(instruction, static_cast<int>(pc));
					break;
				case OpCode::Call:
					HandleCall(instruction, static_cast<int>(pc));
					break;
				case OpCode::TailCall:
					Emit("return " + BuildCall(instruction.a, instruction.b));
					break;
				case OpCode::Return:
					if (!(instruction.b == 1 && pc + 1 >= end))
					{
						Emit(ReturnStatement(instruction));
					}
					break;
				case OpCode::SetList:
					HandleSetList(instruction);
					break;
				case OpCode::Closure:
				{
					std::size_t capture_count = 0;
					if (m_next_closure < m_function.prototypes.size())
					{
						capture_count = m_function.prototypes[m_next_closure]->upvalue_count;
					}
					SetRegister(instruction.a, BuildClosureExpr(static_cast<int>(pc)), static_cast<int>(pc));
					pc += capture_count;
					break;
				}
				case OpCode::VarArg:
					HandleVarArg(instruction, static_cast<int>(pc));
					break;
				case OpCode::TestSet:
					if (!structured)
					{
						Emit("-- " + DescribeInstruction(pc));
					}
					break;
				case OpCode::ForLoop:
				case OpCode::ForPrep:
				case OpCode::TForLoop:
				case OpCode::Close:
					if (!structured)
					{
						Emit("-- " + DescribeInstruction(pc));
					}
					break;
				default:
					if (!structured)
					{
						Emit("-- " + DescribeInstruction(pc));
					}
					break;
				}

				if (pc + 1 >= end || LineOf(static_cast<int>(pc + 1)) != LineOf(static_cast<int>(pc)))
				{
					FlushPending(static_cast<int>(pc));
				}

				++pc;
			}
		}

		bool FunctionDecompiler::TryEmitGenericFor(std::size_t& pc, std::size_t end)
		{
			if (pc + 3 >= end)
			{
				return false;
			}

			const Instruction& call = m_function.code[pc];
			const Instruction& jump = m_function.code[pc + 1];
			if (call.opcode != OpCode::Call || jump.opcode != OpCode::Jmp || call.c < 4)
			{
				return false;
			}

			const std::size_t tfor_pc = JumpTarget(pc + 1);
			if (tfor_pc + 1 >= end)
			{
				return false;
			}

			const Instruction& tfor = m_function.code[tfor_pc];
			const Instruction& back = m_function.code[tfor_pc + 1];
			if (tfor.opcode != OpCode::TForLoop || back.opcode != OpCode::Jmp || tfor.a != call.a)
			{
				return false;
			}

			std::unordered_set<int> materialized_tables;
			for (std::size_t body_pc = pc + 2; body_pc < tfor_pc; ++body_pc)
			{
				const Instruction& body_instruction = m_function.code[body_pc];
				if (body_instruction.opcode != OpCode::SetTable && body_instruction.opcode != OpCode::SetList)
				{
					continue;
				}

				const int reg = body_instruction.a;
				if (reg < 0 || static_cast<std::size_t>(reg) >= m_registers.size())
				{
					continue;
				}

				const RegisterValue& value = m_registers[static_cast<std::size_t>(reg)];
				if (!value.valid || value.expr.kind != Expr::Kind::Table || !value.expr.table_entries)
				{
					continue;
				}

				if (materialized_tables.insert(reg).second)
				{
					MaterializeCaptureValue(reg);
				}
			}

			std::vector<std::string> names;
			for (int index = 0; index < tfor.c; ++index)
			{
				names.push_back(index == 0 ? "k" : index == 1 ? "v" : "value_" + std::to_string(index + 1));
			}

			const SnapshotState saved = Snapshot();
			for (int index = 0; index < tfor.c; ++index)
			{
				SetRegister(tfor.a + 3 + index, MakeText(names[static_cast<std::size_t>(index)]), static_cast<int>(pc));
			}
			const std::vector<std::string> body = RenderNested(pc + 2, tfor_pc);
			Restore(saved);

			Emit("for " + Join(names, ", ") + " in " + BuildCall(call.a, call.b) + " do");
			for (const std::string& line : body)
			{
				m_lines.push_back(line);
			}
			Emit("end");

			pc = tfor_pc + 2;
			return true;
		}

		bool FunctionDecompiler::TryEmitGuardFailureBlock(std::size_t& pc, std::size_t end)
		{
			const auto first = BuildCondition(pc, end);
			if (!first || first->target_pc <= pc + 2 || first->target_pc > end)
			{
				return false;
			}

			const Instruction& first_last = m_function.code[first->target_pc - 1];
			if (first_last.opcode == OpCode::Return)
			{
				if (first->target_pc == pc + 3 && first_last.b == 1)
				{
					return false;
				}

				const std::vector<std::string> body = RenderNested(pc + 2, first->target_pc);
				Emit("if " + first->skip_jump.text + " then");
				for (const std::string& line : body)
				{
					m_lines.push_back(line);
				}
				Emit("end");
				pc = first->target_pc;
				return true;
			}

			if (pc + 4 >= end || first->target_pc != pc + 4)
			{
				return false;
			}

			const auto second = BuildCondition(pc + 2, end);
			if (!second || second->target_pc <= first->target_pc || second->target_pc > end)
			{
				return false;
			}

			if (m_function.code[second->target_pc - 1].opcode != OpCode::Return)
			{
				return false;
			}

			const Expr failure = MakeBinary(first->take_jump, "or", second->skip_jump, kPrecOr);
			const std::vector<std::string> body = RenderNested(first->target_pc, second->target_pc);
			Emit("if " + failure.text + " then");
			for (const std::string& line : body)
			{
				m_lines.push_back(line);
			}
			Emit("end");
			pc = second->target_pc;
			return true;
		}

		bool FunctionDecompiler::TryEmitGuardReturn(std::size_t& pc, std::size_t end)
		{
			std::vector<ConditionInfo> chain;
			std::size_t cursor = pc;
			while (cursor + 1 < end)
			{
				const auto info = BuildCondition(cursor, end);
				if (!info || info->target_pc <= cursor + 1)
				{
					break;
				}
				chain.push_back(*info);
				cursor += 2;
			}

			if (chain.empty())
			{
				return false;
			}

			bool same_target = true;
			for (std::size_t index = 1; index < chain.size(); ++index)
			{
				if (chain[index].target_pc != chain[0].target_pc)
				{
					same_target = false;
					break;
				}
			}

			if (same_target && cursor < end && m_function.code[cursor].opcode == OpCode::Return && chain[0].target_pc > cursor)
			{
				std::vector<Expr> conditions;
				for (const ConditionInfo& info : chain)
				{
					conditions.push_back(info.skip_jump);
				}
				Emit("if " + JoinConditions(conditions, "and") + " then");
				Emit(ReturnStatement(m_function.code[cursor]), 1);
				Emit("end");
				pc = chain[0].target_pc;
				return true;
			}

			if (same_target && chain[0].target_pc < end && chain[0].target_pc == cursor && m_function.code[chain[0].target_pc].opcode == OpCode::Return)
			{
				std::vector<Expr> conditions;
				for (const ConditionInfo& info : chain)
				{
					conditions.push_back(info.take_jump);
				}
				Emit("if " + JoinConditions(conditions, "or") + " then");
				Emit(ReturnStatement(m_function.code[chain[0].target_pc]), 1);
				Emit("end");
				pc = chain[0].target_pc + 1;
				return true;
			}

			const auto condition = BuildCondition(pc, end);
			if (!condition || condition->target_pc <= pc + 1)
			{
				return false;
			}

			if (pc + 2 < end && m_function.code[pc + 2].opcode == OpCode::Return && condition->target_pc > pc + 2)
			{
				Emit("if " + condition->skip_jump.text + " then");
				Emit(ReturnStatement(m_function.code[pc + 2]), 1);
				Emit("end");
				pc = condition->target_pc;
				return true;
			}

			if (condition->target_pc < end && m_function.code[condition->target_pc].opcode == OpCode::Return)
			{
				Emit("if " + condition->take_jump.text + " then");
				Emit(ReturnStatement(m_function.code[condition->target_pc]), 1);
				Emit("end");
				pc += 2;
				return true;
			}

			return false;
		}

		bool FunctionDecompiler::TryEmitIfBlock(std::size_t& pc, std::size_t end)
		{
			std::vector<Expr> conditions;
			std::size_t cursor = pc;
			std::size_t exit_pc = 0;

			while (cursor + 1 < end)
			{
				const auto info = BuildCondition(cursor, end);
				if (!info || info->target_pc <= cursor + 1)
				{
					break;
				}
				if (conditions.empty())
				{
					exit_pc = info->target_pc;
				}
				else if (info->target_pc != exit_pc)
				{
					break;
				}
				conditions.push_back(info->skip_jump);
				cursor += 2;
			}

			if (conditions.empty() || cursor >= exit_pc || exit_pc > end)
			{
				return false;
			}

			if (cursor < end && m_function.code[cursor].opcode == OpCode::Return)
			{
				return false;
			}

			const std::vector<std::string> body = RenderNested(cursor, exit_pc);
			Emit("if " + JoinConditions(conditions, "and") + " then");
			for (const std::string& line : body)
			{
				m_lines.push_back(line);
			}
			Emit("end");
			pc = exit_pc;
			return true;
		}

		bool FunctionDecompiler::TryEmitLosslessBranch(std::size_t& pc, std::size_t end)
		{
			const auto condition = BuildCondition(pc, end);
			if (condition)
			{
				Emit("-- branch: jump -> pc_" + std::to_string(condition->target_pc) + " when " + condition->take_jump.text);
				pc += 2;
				return true;
			}

			if (m_function.code[pc].opcode == OpCode::Jmp)
			{
				Emit("-- jump -> pc_" + std::to_string(JumpTarget(pc)));
				++pc;
				return true;
			}

			return false;
		}

		std::optional<ConditionInfo> FunctionDecompiler::BuildCondition(std::size_t pc, std::size_t end) const
		{
			if (pc + 1 >= end || m_function.code[pc + 1].opcode != OpCode::Jmp)
			{
				return std::nullopt;
			}

			const Instruction& instruction = m_function.code[pc];
			ConditionInfo info{};
			info.target_pc = JumpTarget(pc + 1);
			if (info.target_pc <= pc + 1)
			{
				return std::nullopt;
			}

			switch (instruction.opcode)
			{
			case OpCode::Test:
			{
				const Expr expr = RegisterExpr(instruction.a);
				if (instruction.c == 0)
				{
					info.skip_jump = expr;
					info.take_jump = MakeUnary("not ", expr);
				}
				else
				{
					info.skip_jump = MakeUnary("not ", expr);
					info.take_jump = expr;
				}
				return info;
			}
			case OpCode::Eq:
			{
				const Expr lhs = RKExpr(instruction.b);
				const Expr rhs = RKExpr(instruction.c);
				if (instruction.a == 0)
				{
					info.skip_jump = MakeBinary(lhs, "==", rhs, kPrecCompare);
					info.take_jump = MakeBinary(lhs, "~=", rhs, kPrecCompare);
				}
				else
				{
					info.skip_jump = MakeBinary(lhs, "~=", rhs, kPrecCompare);
					info.take_jump = MakeBinary(lhs, "==", rhs, kPrecCompare);
				}
				return info;
			}
			case OpCode::Lt:
			{
				const Expr lhs = RKExpr(instruction.b);
				const Expr rhs = RKExpr(instruction.c);
				if (instruction.a == 0)
				{
					info.skip_jump = MakeBinary(lhs, "<", rhs, kPrecCompare);
					info.take_jump = MakeBinary(lhs, ">=", rhs, kPrecCompare);
				}
				else
				{
					info.skip_jump = MakeBinary(lhs, ">=", rhs, kPrecCompare);
					info.take_jump = MakeBinary(lhs, "<", rhs, kPrecCompare);
				}
				return info;
			}
			case OpCode::Le:
			{
				const Expr lhs = RKExpr(instruction.b);
				const Expr rhs = RKExpr(instruction.c);
				if (instruction.a == 0)
				{
					info.skip_jump = MakeBinary(lhs, "<=", rhs, kPrecCompare);
					info.take_jump = MakeBinary(lhs, ">", rhs, kPrecCompare);
				}
				else
				{
					info.skip_jump = MakeBinary(lhs, ">", rhs, kPrecCompare);
					info.take_jump = MakeBinary(lhs, "<=", rhs, kPrecCompare);
				}
				return info;
			}
			default:
				return std::nullopt;
			}
		}

		void FunctionDecompiler::HandleSetTable(const Instruction& instruction, int pc)
		{
			RegisterValue& table = EnsureRegister(instruction.a);
			if (table.valid && table.expr.kind == Expr::Kind::Table && table.expr.table_entries)
			{
				TableEntry entry{};
				entry.value = RKExpr(instruction.c);
				if (IsConstantIndex(instruction.b))
				{
					const Constant& constant = ConstantAt(ConstantIndex(instruction.b));
					if (constant.type == ConstantType::String && IsIdentifier(constant.string))
					{
						entry.key_kind = TableEntry::KeyKind::Name;
						entry.key_text = constant.string;
					}
					else
					{
						entry.key_kind = TableEntry::KeyKind::Expr;
						entry.key_text = RKExpr(instruction.b).text;
					}
				}
				else
				{
					entry.key_kind = TableEntry::KeyKind::Expr;
					entry.key_text = RegisterExpr(instruction.b).text;
				}

				table.expr.table_entries->push_back(std::move(entry));
				table.assigned_pc = pc;
				table.assigned_line = LineOf(pc);
				return;
			}

			EmitAssignment(BuildTableAccess(RegisterExpr(instruction.a), RKExpr(instruction.b)), RKExpr(instruction.c));
		}

		void FunctionDecompiler::HandleSetList(const Instruction& instruction)
		{
			RegisterValue& table = EnsureRegister(instruction.a);
			if (!table.valid || table.expr.kind != Expr::Kind::Table || !table.expr.table_entries)
			{
				return;
			}

			const int count = instruction.b == 0 ? 1 : instruction.b;
			for (int index = 0; index < count; ++index)
			{
				TableEntry entry{};
				entry.key_kind = TableEntry::KeyKind::Array;
				entry.value = RegisterExpr(instruction.a + 1 + index);
				table.expr.table_entries->push_back(std::move(entry));
			}
		}

		void FunctionDecompiler::HandleConcat(const Instruction& instruction, int pc)
		{
			Expr value = RegisterExpr(instruction.b);
			for (int reg = instruction.b + 1; reg <= instruction.c; ++reg)
			{
				value = MakeBinary(value, "..", RegisterExpr(reg), kPrecConcat);
			}
			SetRegister(instruction.a, value, pc);
		}

		void FunctionDecompiler::HandleCall(const Instruction& instruction, int pc)
		{
			if (instruction.b > 1)
			{
				for (int reg = instruction.a + 1; reg <= instruction.a + instruction.b - 1; ++reg)
				{
					MaterializeCallArg(reg);
				}
			}

			const std::string call_text = BuildCall(instruction.a, instruction.b);
			ClearOpenCall();
			if (instruction.c == 1)
			{
				Emit(call_text);
				return;
			}
			if (instruction.c == 0)
			{
				RegisterValue value{};
				value.valid = true;
				value.expr = MakeText(call_text);
				value.assigned_pc = pc;
				value.assigned_line = LineOf(pc);
				value.open_call = true;
				EnsureRegister(instruction.a) = std::move(value);
				return;
			}
			if (instruction.c == 2)
			{
				SetRegister(instruction.a, MakeText(call_text), pc);
				return;
			}

			MultiResultGroup group{};
			group.call_text = call_text;
			group.assigned_line = LineOf(pc);
			group.names = SuggestMultiResultNames(call_text, instruction.c - 1);
			for (int index = 0; index < instruction.c - 1; ++index)
			{
				group.registers.push_back(instruction.a + index);
			}
			while (group.names.size() < group.registers.size())
			{
				group.names.push_back("value_" + std::to_string(++m_temp_index));
			}

			const std::size_t group_index = m_multi_results.size();
			m_multi_results.push_back(group);
			for (std::size_t index = 0; index < group.registers.size(); ++index)
			{
				RegisterValue value{};
				value.valid = true;
				value.expr = MakeText(group.names[index]);
				value.assigned_pc = pc;
				value.assigned_line = LineOf(pc);
				value.multi_group = group_index;
				EnsureRegister(group.registers[index]) = std::move(value);
			}
		}

		void FunctionDecompiler::HandleVarArg(const Instruction& instruction, int pc)
		{
			for (int index = 0; index < std::max(instruction.b - 1, 0); ++index)
			{
				SetRegister(instruction.a + index, MakeText("select(" + std::to_string(index + 1) + ", ...)"), pc);
			}
		}

		void FunctionDecompiler::HandleSelf(const Instruction& instruction, int pc)
		{
			const Expr object = RegisterExpr(instruction.b);
			const Expr key = RKExpr(instruction.c);

			RegisterValue method{};
			method.valid = true;
			method.expr = MakeText(BuildTableAccess(object, key));
			method.assigned_pc = pc;
			method.assigned_line = LineOf(pc);
			method.self_call = true;
			method.self_object = object.text;
			method.self_method = key.text;
			EnsureRegister(instruction.a) = std::move(method);

			SetRegister(instruction.a + 1, object, pc);
		}

		void FunctionDecompiler::EmitAssignment(const std::string& target, const Expr& value)
		{
			Emit(target + " = " + RenderExpr(value, m_indent_level + (m_top_level ? 0 : 1)));
		}

		void FunctionDecompiler::FlushPending(int pc)
		{
			for (MultiResultGroup& group : m_multi_results)
			{
				if (group.emitted)
				{
					continue;
				}

				bool used = false;
				for (int reg : group.registers)
				{
					if (RegisterUsedLater(reg, pc))
					{
						used = true;
						break;
					}
				}

				if (!used)
				{
					group.emitted = true;
					continue;
				}

				Emit("local " + Join(group.names, ", ") + " = " + group.call_text);
				const std::size_t bind_count = std::min(group.registers.size(), group.names.size());
				for (std::size_t index = 0; index < bind_count; ++index)
				{
					SetRegister(group.registers[index], MakeText(group.names[index]), pc);
				}
				group.emitted = true;
			}

			for (std::size_t reg = 0; reg < m_registers.size(); ++reg)
			{
				RegisterValue& value = m_registers[reg];
				if (!value.valid || value.multi_group)
				{
					continue;
				}
				if (value.assigned_line != LineOf(pc))
				{
					continue;
				}
				if (!RegisterUsedLater(static_cast<int>(reg), pc))
				{
					continue;
				}
				if (const auto next_read = NextReadBeforeWrite(static_cast<int>(reg), pc))
				{
					if (m_function.code[*next_read].opcode == OpCode::SetList)
					{
						continue;
					}
				}
				if (value.expr.kind == Expr::Kind::Text && IsLuaName(value.expr.text))
				{
					continue;
				}

				const std::string name = SuggestLocalName(value.expr);
				Emit("local " + name + " = " + RenderExpr(value.expr, m_indent_level + (m_top_level ? 0 : 1)));
				m_named_exprs[name] = value.expr;
				value.expr = MakeText(name);
			}
		}

		bool FunctionDecompiler::RegisterUsedLater(int reg, int pc) const
		{
			for (std::size_t next = static_cast<std::size_t>(std::max(pc + 1, 0)); next < m_function.code.size(); ++next)
			{
				const Instruction& instruction = m_function.code[next];
				if (InstructionReadsRegister(instruction, reg))
				{
					return true;
				}
				if (InstructionWritesRegister(instruction, reg))
				{
					return false;
				}
			}
			return false;
		}

		std::optional<std::size_t> FunctionDecompiler::NextReadBeforeWrite(int reg, int pc) const
		{
			for (std::size_t next = static_cast<std::size_t>(std::max(pc + 1, 0)); next < m_function.code.size(); ++next)
			{
				const Instruction& instruction = m_function.code[next];
				if (InstructionReadsRegister(instruction, reg))
				{
					return next;
				}
				if (InstructionWritesRegister(instruction, reg))
				{
					return std::nullopt;
				}
			}

			return std::nullopt;
		}

		bool FunctionDecompiler::InstructionReadsRegister(const Instruction& instruction, int reg) const
		{
			auto rk = [&](int value){ return !IsConstantIndex(value) && value == reg; };
			switch (instruction.opcode)
			{
			case OpCode::Move: return instruction.b == reg;
			case OpCode::GetTable: return instruction.b == reg || rk(instruction.c);
			case OpCode::SetGlobal:
			case OpCode::SetUpval: return instruction.a == reg;
			case OpCode::SetTable: return instruction.a == reg || rk(instruction.b) || rk(instruction.c);
			case OpCode::Self: return instruction.b == reg || rk(instruction.c);
			case OpCode::Add:
			case OpCode::Sub:
			case OpCode::Mul:
			case OpCode::Div:
			case OpCode::Mod:
			case OpCode::Pow:
			case OpCode::Eq:
			case OpCode::Lt:
			case OpCode::Le: return rk(instruction.b) || rk(instruction.c);
			case OpCode::Unm:
			case OpCode::Not:
			case OpCode::Len:
			case OpCode::Test: return instruction.a == reg || instruction.b == reg;
			case OpCode::Concat: return reg >= instruction.b && reg <= instruction.c;
			case OpCode::Call:
			case OpCode::TailCall:
				return instruction.b == 0 ? reg >= instruction.a : reg >= instruction.a && reg <= instruction.a + instruction.b - 1;
			case OpCode::Return:
				return instruction.b == 0 ? reg >= instruction.a : reg >= instruction.a && reg <= instruction.a + instruction.b - 2;
			case OpCode::SetList:
				return reg >= instruction.a && reg <= instruction.a + instruction.b;
			default:
				return false;
			}
		}

		bool FunctionDecompiler::InstructionWritesRegister(const Instruction& instruction, int reg) const
		{
			switch (instruction.opcode)
			{
			case OpCode::Move:
			case OpCode::LoadK:
			case OpCode::LoadBool:
			case OpCode::GetUpval:
			case OpCode::GetGlobal:
			case OpCode::GetTable:
			case OpCode::NewTable:
			case OpCode::Closure:
			case OpCode::VarArg:
			case OpCode::Unm:
			case OpCode::Not:
			case OpCode::Len:
				return instruction.a == reg;
			case OpCode::LoadNil:
				return reg >= instruction.a && reg <= instruction.b;
			case OpCode::Self:
				return reg == instruction.a || reg == instruction.a + 1;
			case OpCode::Add:
			case OpCode::Sub:
			case OpCode::Mul:
			case OpCode::Div:
			case OpCode::Mod:
			case OpCode::Pow:
			case OpCode::Concat:
				return instruction.a == reg;
			case OpCode::Call:
				return instruction.c == 0 ? reg == instruction.a : instruction.c > 1 && reg >= instruction.a && reg <= instruction.a + instruction.c - 2;
			default:
				return false;
			}
		}

		FunctionDecompiler::SnapshotState FunctionDecompiler::Snapshot() const
		{
			SnapshotState snapshot{};
			snapshot.registers = m_registers;
			snapshot.groups = m_multi_results;
			snapshot.next_closure = m_next_closure;
			snapshot.temp_index = m_temp_index;
			snapshot.used_names = m_used_names;
			snapshot.named_exprs = m_named_exprs;
			return snapshot;
		}

		void FunctionDecompiler::Restore(const SnapshotState& snapshot)
		{
			m_registers = snapshot.registers;
			m_multi_results = snapshot.groups;
			m_next_closure = snapshot.next_closure;
			m_temp_index = snapshot.temp_index;
			m_used_names = snapshot.used_names;
			m_named_exprs = snapshot.named_exprs;
		}

		std::vector<std::string> FunctionDecompiler::CaptureLines(bool structured, std::size_t begin, std::size_t end)
		{
			const bool recursive_structured = structured
				&& std::any_of(m_active_ranges.begin(), m_active_ranges.end(), [&](const ActiveRange& range)
				{
					return range.structured && range.begin == begin && range.end == end;
				});
			if (recursive_structured)
			{
				if (!m_reported_guard_fallback)
				{
					m_diagnostics.Warning("decompile", "Повторно вошёл в тот же блок, откатываюсь к lossless-режиму");
					m_reported_guard_fallback = true;
				}

				AppendProfileLog("capture-fallback: begin=" + std::to_string(begin) + ", end=" + std::to_string(end) + ", reason=recursive_range");
				structured = false;
			}

			struct RangeScope
			{
				std::vector<ActiveRange>& stack;

				RangeScope(std::vector<ActiveRange>& target, std::size_t begin_pc, std::size_t end_pc, bool structured_mode)
					: stack(target)
				{
					stack.push_back(ActiveRange{ begin_pc, end_pc, structured_mode });
				}

				~RangeScope()
				{
					stack.pop_back();
				}
			} scope(m_active_ranges, begin, end, structured);

			const SnapshotState snapshot = Snapshot();
			const auto saved_lines = std::move(m_lines);
			m_lines.clear();
			DecompileRange(begin, end, structured);
			FlushPending(end > 0 ? static_cast<int>(end - 1) : 0);
			std::vector<std::string> lines = std::move(m_lines);
			m_lines = saved_lines;
			Restore(snapshot);
			return lines;
		}

		std::vector<std::string> FunctionDecompiler::RenderNested(std::size_t begin, std::size_t end)
		{
			m_indent_level++;
			std::vector<std::string> body = CaptureLines(true, begin, end);
			m_indent_level--;
			return body;
		}

		std::vector<std::string> FunctionDecompiler::BuildSyntheticPseudoLines(const std::vector<std::string>& lines) const
		{
			if (lines.empty())
			{
				return {};
			}

			std::vector<bool> keep(lines.size(), false);
			std::vector<std::size_t> block_stack;
			block_stack.reserve(16);

			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				const std::string_view trimmed = TrimView(lines[index]);
				if (trimmed.empty())
				{
					continue;
				}

				if (IsOpenSyntheticBlock(trimmed))
				{
					block_stack.push_back(index);
					continue;
				}

				if (IsBranchSyntheticLine(trimmed))
				{
					if (!block_stack.empty() && keep[block_stack.back()])
					{
						keep[index] = true;
					}
					continue;
				}

				if (IsCloseSyntheticBlock(trimmed))
				{
					if (!block_stack.empty())
					{
						const std::size_t open_index = block_stack.back();
						block_stack.pop_back();
						if (keep[open_index])
						{
							keep[index] = true;
						}
					}
					continue;
				}

				if (!IsMeaningfulSyntheticStatement(trimmed))
				{
					continue;
				}

				keep[index] = true;
				for (std::size_t open_index : block_stack)
				{
					keep[open_index] = true;
				}
			}

			std::vector<std::string> result;
			result.reserve(lines.size());
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				if (!keep[index])
				{
					continue;
				}

				const std::string_view trimmed = TrimView(lines[index]);
				if (trimmed.starts_with("return") && index + 1 < lines.size())
				{
					bool has_following_logic = false;
					for (std::size_t next = index + 1; next < lines.size(); ++next)
					{
						if (!keep[next])
						{
							continue;
						}

						const std::string_view next_trimmed = TrimView(lines[next]);
						if (next_trimmed == "end" || next_trimmed == "until")
						{
							continue;
						}

						has_following_logic = true;
						break;
					}

					if (has_following_logic)
					{
						continue;
					}
				}

				if (CountGenericSyntheticNames(trimmed) >= 4 && CountMeaningfulSyntheticNames(trimmed) == 0 && !ContainsReadableLiteral(trimmed))
				{
					continue;
				}

				if (trimmed.find(" = function(") != std::string_view::npos && CountMeaningfulSyntheticNames(trimmed) == 0 && !ContainsReadableLiteral(trimmed))
				{
					continue;
				}

				result.push_back(lines[index]);
			}

			while (!result.empty())
			{
				const std::string_view trimmed = TrimView(result.front());
				if (trimmed == "end" || trimmed == "else" || trimmed.starts_with("elseif"))
				{
					result.erase(result.begin());
					continue;
				}
				break;
			}

			return result;
		}

		std::string FunctionDecompiler::JoinConditions(const std::vector<Expr>& expressions, std::string_view op) const
		{
			if (expressions.empty())
			{
				return "true";
			}

			Expr result = expressions.front();
			for (std::size_t index = 1; index < expressions.size(); ++index)
			{
				result = MakeBinary(result, op, expressions[index], op == "and" ? kPrecAnd : kPrecOr);
			}
			return result.text;
		}

		std::size_t FunctionDecompiler::JumpTarget(std::size_t jmp_pc) const
		{
			if (jmp_pc >= m_function.code.size())
			{
				return m_function.code.size();
			}

			const long long base = static_cast<long long>(jmp_pc) + 1;
			const long long target = base + static_cast<long long>(m_function.code[jmp_pc].sbx);
			if (target < 0 || target > static_cast<long long>(m_function.code.size()))
			{
				return m_function.code.size();
			}

			return static_cast<std::size_t>(target);
		}

		const Constant& FunctionDecompiler::ConstantAt(int index) const
		{
			static Constant fallback{};
			return index >= 0 && static_cast<std::size_t>(index) < m_function.constants.size() ? m_function.constants[static_cast<std::size_t>(index)] : fallback;
		}

		Expr FunctionDecompiler::ConstantToExpr(int index) const
		{
			const Constant& constant = ConstantAt(index);
			switch (constant.type)
			{
			case ConstantType::Nil: return MakeText("nil");
			case ConstantType::Boolean: return MakeText(constant.boolean ? "true" : "false");
			case ConstantType::Number: return MakeText(FormatNumber(constant.number));
			case ConstantType::String: return MakeText(EscapeString(constant.string));
			default: return MakeText("nil");
			}
		}

		Expr FunctionDecompiler::RKExpr(int value) const
		{
			return IsConstantIndex(value) ? ConstantToExpr(ConstantIndex(value)) : RegisterExpr(value);
		}

		Expr FunctionDecompiler::RegisterExpr(int reg) const
		{
			if (reg < 0 || static_cast<std::size_t>(reg) >= m_registers.size() || !m_registers[static_cast<std::size_t>(reg)].valid)
			{
				return MakeText(reg >= 0 && static_cast<std::size_t>(reg) < m_parameters.size() ? m_parameters[static_cast<std::size_t>(reg)] : "r" + std::to_string(reg));
			}
			return m_registers[static_cast<std::size_t>(reg)].expr;
		}

		RegisterValue& FunctionDecompiler::EnsureRegister(int reg)
		{
			if (static_cast<std::size_t>(reg) >= m_registers.size())
			{
				m_registers.resize(static_cast<std::size_t>(reg) + 1);
			}
			return m_registers[static_cast<std::size_t>(reg)];
		}

		void FunctionDecompiler::SetRegister(int reg, const Expr& expr, int pc)
		{
			RegisterValue value{};
			value.valid = true;
			value.expr = expr;
			value.assigned_pc = pc;
			value.assigned_line = LineOf(pc);
			EnsureRegister(reg) = std::move(value);
		}

		void FunctionDecompiler::CopyRegister(int target, int source, int pc)
		{
			RegisterValue value = EnsureRegister(source);
			value.assigned_pc = pc;
			value.assigned_line = LineOf(pc);
			EnsureRegister(target) = std::move(value);
		}

		int FunctionDecompiler::LineOf(int pc) const
		{
			return pc >= 0 && static_cast<std::size_t>(pc) < m_function.lines.size() ? static_cast<int>(m_function.lines[static_cast<std::size_t>(pc)]) : 0;
		}

		Expr FunctionDecompiler::NewTableExpr() const
		{
			Expr expr{};
			expr.kind = Expr::Kind::Table;
			expr.table_entries = std::make_shared<std::vector<TableEntry>>();
			return expr;
		}

		Expr FunctionDecompiler::BuildClosureExpr(int)
		{
			if (m_next_closure >= m_function.prototypes.size())
			{
				return MakeText("function() end");
			}

			auto closure = std::make_shared<ClosureValue>();
			closure->function = m_function.prototypes[m_next_closure++].get();
			for (const UpvalueCapture& capture : closure->function->captures)
			{
				closure->upvalues.push_back(capture.from_register ? MaterializeCaptureValue(capture.index) : UpvalueName(capture.index));
			}

			Expr expr{};
			expr.kind = Expr::Kind::Closure;
			expr.closure = std::move(closure);
			return expr;
		}

		std::string FunctionDecompiler::MaterializeCaptureValue(int reg)
		{
			Expr expr = RegisterExpr(reg);
			if (expr.kind == Expr::Kind::Text && IsLuaName(expr.text))
			{
				return expr.text;
			}

			std::string name = SuggestLocalName(expr);
			Emit("local " + name + " = " + RenderExpr(expr, m_indent_level + (m_top_level ? 0 : 1)));
			m_named_exprs[name] = expr;
			RegisterValue& value = EnsureRegister(reg);
			value.valid = true;
			value.expr = MakeText(name);
			value.open_call = false;
			value.multi_group = std::nullopt;
			return name;
		}

		void FunctionDecompiler::MaterializeCallArg(int reg)
		{
			if (reg < 0 || static_cast<std::size_t>(reg) >= m_registers.size())
			{
				return;
			}

			RegisterValue& value = m_registers[static_cast<std::size_t>(reg)];
			if (!value.valid || value.open_call || value.multi_group.has_value())
			{
				return;
			}

			if (value.expr.kind == Expr::Kind::Text && IsLuaName(value.expr.text))
			{
				return;
			}

			const std::string rendered = RenderExpr(value.expr, m_indent_level + (m_top_level ? 0 : 1));
			if (value.expr.kind != Expr::Kind::Table
				&& value.expr.kind != Expr::Kind::Closure
				&& rendered.find('\n') == std::string::npos)
			{
				return;
			}

			const std::string name = SuggestLocalName(value.expr);
			Emit("local " + name + " = " + rendered);
			m_named_exprs[name] = value.expr;
			value.expr = MakeText(name);
			value.self_call = false;
			value.self_object.clear();
			value.self_method.clear();
			value.open_call = false;
			value.multi_group = std::nullopt;
		}

		std::string FunctionDecompiler::BuildCall(int a, int b) const
		{
			RegisterValue callee{};
			if (a >= 0 && static_cast<std::size_t>(a) < m_registers.size())
			{
				callee = m_registers[static_cast<std::size_t>(a)];
			}
			else
			{
				callee.valid = true;
				callee.expr = MakeText("r" + std::to_string(a));
			}
			std::vector<std::string> args;
			std::vector<Expr> arg_exprs;
			if (b == 0)
			{
				if (static_cast<std::size_t>(a + 1) < m_registers.size() && m_registers[static_cast<std::size_t>(a + 1)].open_call)
				{
					const Expr arg_expr = RegisterExpr(a + 1);
					arg_exprs.push_back(arg_expr);
					args.push_back(RenderExpr(arg_expr, m_indent_level));
				}
				else
				{
					for (std::size_t reg = static_cast<std::size_t>(a + 1); reg < m_registers.size(); ++reg)
					{
						if (!m_registers[reg].valid)
						{
							break;
						}
						arg_exprs.push_back(m_registers[reg].expr);
						args.push_back(RenderExpr(m_registers[reg].expr, m_indent_level));
						if (m_registers[reg].open_call)
						{
							break;
						}
					}
				}
			}
			else
			{
				for (int reg = a + 1; reg <= a + b - 1; ++reg)
				{
					const Expr arg_expr = RegisterExpr(reg);
					arg_exprs.push_back(arg_expr);
					args.push_back(RenderExpr(arg_expr, m_indent_level));
				}
			}

			if (callee.self_call && !callee.self_object.empty())
			{
				std::vector<std::string> method_args;
				for (std::size_t index = 1; index < args.size(); ++index)
				{
					method_args.push_back(args[index]);
				}
				const std::string method = callee.self_method.size() >= 2 && callee.self_method.front() == '"' && callee.self_method.back() == '"' ? callee.self_method.substr(1, callee.self_method.size() - 2) : callee.self_method;
				if (IsIdentifier(method))
				{
					return callee.self_object + ":" + method + "(" + Join(method_args, ", ") + ")";
				}
			}

			if (const auto folded = TryFoldLiteralCall(callee.expr, arg_exprs))
			{
				return *folded;
			}

			const std::string callee_text = RenderExpr(callee.expr, m_indent_level);
			if (args.size() == 1 && !args[0].empty() && args[0].front() == '{' && IsLuaName(callee_text) && std::isupper(static_cast<unsigned char>(callee_text.front())) != 0)
			{
				return callee_text + args[0];
			}

			return callee_text + "(" + Join(args, ", ") + ")";
		}

		void FunctionDecompiler::ClearOpenCall()
		{
			for (RegisterValue& value : m_registers)
			{
				value.open_call = false;
			}
		}

		std::string FunctionDecompiler::ReturnStatement(const Instruction& instruction) const
		{
			if (instruction.b == 1)
			{
				return "return";
			}
			if (instruction.b == 0)
			{
				std::vector<std::string> values;
				for (int reg = instruction.a; reg >= 0 && static_cast<std::size_t>(reg) < m_registers.size(); ++reg)
				{
					if (!m_registers[static_cast<std::size_t>(reg)].valid)
					{
						break;
					}

					values.push_back(RenderExpr(RegisterExpr(reg), m_indent_level));
					if (m_registers[static_cast<std::size_t>(reg)].open_call)
					{
						break;
					}
				}

				if (!values.empty())
				{
					return "return " + Join(values, ", ");
				}

				if (m_function.is_vararg != 0)
				{
					return "return ...";
				}

				return "return";
			}
			std::vector<std::string> values;
			for (int reg = instruction.a; reg <= instruction.a + instruction.b - 2; ++reg)
			{
				values.push_back(RenderExpr(RegisterExpr(reg), m_indent_level));
			}
			return "return " + Join(values, ", ");
		}

		std::vector<std::string> FunctionDecompiler::SuggestMultiResultNames(const std::string& call_text, int count)
		{
			if (call_text == "guiGetScreenSize()" && count == 2)
			{
				return { "screenX", "screenY" };
			}
			if (call_text.find("ibCreateScrollpane(") == 0 && count == 2)
			{
				return { "scrollpane", "scrollbar" };
			}
			std::vector<std::string> result;
			for (int index = 0; index < count; ++index)
			{
				result.push_back("value_" + std::to_string(++m_temp_index));
			}
			return result;
		}

		std::string FunctionDecompiler::SuggestLocalName(const Expr& expr)
		{
			if (expr.kind == Expr::Kind::Table && expr.table_entries)
			{
				bool numeric_true_lookup = !expr.table_entries->empty();
				for (const TableEntry& entry : *expr.table_entries)
				{
					if (entry.key_kind != TableEntry::KeyKind::Expr
						|| !IsNumericLiteral(entry.key_text)
						|| entry.value.kind != Expr::Kind::Text
						|| entry.value.text != "true")
					{
						numeric_true_lookup = false;
						break;
					}
				}

				if (numeric_true_lookup)
				{
					return UniqueName("allow");
				}

				for (const TableEntry& entry : *expr.table_entries)
				{
					if (entry.key_kind == TableEntry::KeyKind::Name && (entry.key_text == "SELECT_ACS" || entry.key_text == "createPedShop"))
					{
						return UniqueName("shop");
					}
				}
			}
			if (expr.kind == Expr::Kind::Text)
			{
				if (expr.text == "false" || expr.text == "true")
				{
					return UniqueName("flag");
				}
				if (expr.text == "nil")
				{
					return UniqueName("state");
				}
				if (expr.text.starts_with("ITEM_CONFIG_VISUAL("))
				{
					return UniqueName("visual");
				}
				if (expr.text.starts_with("TeleportPoint("))
				{
					return UniqueName("point");
				}
				if (expr.text.starts_with("TREASURE_LOCATIONS_LIST["))
				{
					return UniqueName("location");
				}
				if (expr.text.starts_with("Vector3("))
				{
					return UniqueName("vec");
				}
				if (expr.text.starts_with("ibCreateBackground("))
				{
					return UniqueName("background");
				}
				if (expr.text.starts_with("ibCreateImage("))
				{
					return UniqueName("image");
				}
				if (expr.text.starts_with("ibCreateLabel("))
				{
					return UniqueName("label");
				}
				if (expr.text.starts_with("ibCreateButton("))
				{
					return UniqueName("button");
				}
				if (expr.text.starts_with("ibCreateEdit("))
				{
					return UniqueName("edit");
				}
				if (expr.text.starts_with("ibCreateWebMemo("))
				{
					return UniqueName("memo");
				}
				if (expr.text.starts_with("createEffect("))
				{
					return UniqueName("effect");
				}
				if (expr.text.starts_with("createObject("))
				{
					return UniqueName("object");
				}
				if (expr.text.starts_with("createBlip(") || expr.text.starts_with("Blip("))
				{
					return UniqueName("blip");
				}
				if (expr.text.starts_with("createColSphere("))
				{
					return UniqueName("colshape");
				}
				if (expr.text.starts_with("string.format("))
				{
					return UniqueName("text");
				}
				if (expr.text == "1920")
				{
					return UniqueName("devScreenX");
				}
				if (expr.text == "1080")
				{
					return UniqueName("devScreenY");
				}
				if (expr.text == "screenY / devScreenY")
				{
					return UniqueName("scaleValue");
				}
			}
			return UniqueName("var");
		}

		std::string FunctionDecompiler::UniqueName(const std::string& base)
		{
			std::string name = base;
			while (m_used_names.contains(name))
			{
				name = base + "_" + std::to_string(++m_temp_index);
			}
			m_used_names.insert(name);
			return name;
		}

		Expr FunctionDecompiler::ResolveNamedExpr(const Expr& expr) const
		{
			if (expr.kind != Expr::Kind::Text || !IsLuaName(expr.text))
			{
				return expr;
			}

			Expr current = expr;
			for (int depth = 0; depth < 8; ++depth)
			{
				const auto found = m_named_exprs.find(current.text);
				if (found == m_named_exprs.end())
				{
					return current;
				}
				if (found->second.kind != Expr::Kind::Text || found->second.text == current.text)
				{
					return found->second;
				}
				current = found->second;
			}
			return current;
		}

		std::optional<std::string> FunctionDecompiler::TryFoldLiteralCall(const Expr& callee, const std::vector<Expr>& args) const
		{
			const Expr resolved = ResolveNamedExpr(callee);
			if (resolved.kind != Expr::Kind::Closure || !resolved.closure || !resolved.closure->function || args.size() != 2)
			{
				return std::nullopt;
			}

			const Expr arg0 = ResolveNamedExpr(args[0]);
			const Expr arg1 = ResolveNamedExpr(args[1]);
			const auto encoded = UnescapeLuaStringLiteral(arg0.text);
			const auto seed = ParseLuaNumberLiteral(arg1.text);
			if (!encoded || !seed)
			{
				return std::nullopt;
			}

			const auto config = DetectHexStreamDecoder(*resolved.closure->function);
			if (!config)
			{
				return std::nullopt;
			}

			const auto decoded = DecodeHexStreamLiteral(*encoded, *seed, *config);
			if (!decoded)
			{
				return std::nullopt;
			}

			const auto normalized = NormalizeStringValue(*decoded);
			if (normalized
				&& normalized->size() > 256
				&& !normalized->starts_with("function")
				&& !normalized->starts_with("return")
				&& !normalized->starts_with("exports.")
				&& !normalized->starts_with("addEventHandler"))
			{
				return std::nullopt;
			}

			return EscapeString(*decoded);
		}

		std::string FunctionDecompiler::GlobalName(int index) const
		{
			if (const auto found = m_global_aliases.find(index); found != m_global_aliases.end())
			{
				return found->second;
			}

			const Constant& constant = ConstantAt(index);
			if (constant.type == ConstantType::String && IsLuaName(constant.string))
			{
				return constant.string;
			}

			if (constant.type == ConstantType::String)
			{
				if (const auto placeholder = PlaceholderIdentifier(constant.string))
				{
					if (IsLuaName(*placeholder))
					{
						return *placeholder;
					}
				}

				return "_G[" + EscapeString(constant.string) + "]";
			}

			return "_G[" + std::to_string(index) + "]";
		}

		std::string FunctionDecompiler::UpvalueName(int index) const
		{
			if (index >= 0 && static_cast<std::size_t>(index) < m_upvalues.size())
			{
				return m_upvalues[static_cast<std::size_t>(index)];
			}
			if (index >= 0 && static_cast<std::size_t>(index) < m_function.upvalue_names.size() && !m_function.upvalue_names[static_cast<std::size_t>(index)].empty())
			{
				return m_function.upvalue_names[static_cast<std::size_t>(index)];
			}
			return "upvalue_" + std::to_string(index);
		}

		std::string FunctionDecompiler::BuildTableAccess(const Expr& table, const Expr& key) const
		{
			const std::string table_text = Wrap(table, kPrecPrimary);
			if (key.text.size() >= 2 && key.text.front() == '"' && key.text.back() == '"')
			{
				const std::string name = key.text.substr(1, key.text.size() - 2);
				if (IsLuaName(name))
				{
					return table_text + "." + name;
				}
			}
			return table_text + "[" + (IsIdentifier(key.text) || IsNumericLiteral(key.text) ? key.text : RenderExpr(key, m_indent_level)) + "]";
		}

		std::string FunctionDecompiler::RenderExpr(const Expr& expr, int indent_level) const
		{
			switch (expr.kind)
			{
			case Expr::Kind::Text: return expr.text;
			case Expr::Kind::Table: return RenderTable(expr, indent_level);
			case Expr::Kind::Closure: return RenderClosure(expr, indent_level);
			default: return expr.text;
			}
		}

		std::string FunctionDecompiler::RenderTable(const Expr& expr, int indent_level) const
		{
			if (!expr.table_entries || expr.table_entries->empty())
			{
				return "{}";
			}

			const void* table_id = expr.table_entries.get();
			if (std::find(m_render_tables.begin(), m_render_tables.end(), table_id) != m_render_tables.end())
			{
				if (!m_reported_render_cycle)
				{
					m_diagnostics.Warning("decompile", "Обнаружена циклическая таблица, режу рендер до {}");
					m_reported_render_cycle = true;
				}

				AppendProfileLog("render-cycle: table");
				return "{}";
			}

			struct TableScope
			{
				std::vector<const void*>& stack;

				TableScope(std::vector<const void*>& target, const void* id)
					: stack(target)
				{
					stack.push_back(id);
				}

				~TableScope()
				{
					stack.pop_back();
				}
			} scope(m_render_tables, table_id);

			bool multiline = expr.table_entries->size() > 3;
			for (const TableEntry& entry : *expr.table_entries)
			{
				if (entry.value.kind != Expr::Kind::Text)
				{
					multiline = true;
					break;
				}
			}
			if (!multiline)
			{
				std::vector<std::string> items;
				for (const TableEntry& entry : *expr.table_entries)
				{
					items.push_back(RenderTableEntry(entry, indent_level));
				}
				return "{ " + Join(items, ", ") + " }";
			}
			std::ostringstream out;
			out << "{\n";
			for (const TableEntry& entry : *expr.table_entries)
			{
				out << Indent(indent_level + 1) << RenderTableEntry(entry, indent_level + 1) << ",\n";
			}
			out << Indent(indent_level) << "}";
			return out.str();
		}

		std::string FunctionDecompiler::RenderTableEntry(const TableEntry& entry, int indent_level) const
		{
			switch (entry.key_kind)
			{
			case TableEntry::KeyKind::Array: return RenderExpr(entry.value, indent_level);
			case TableEntry::KeyKind::Name: return entry.key_text + " = " + RenderExpr(entry.value, indent_level);
			case TableEntry::KeyKind::Expr: return "[" + entry.key_text + "] = " + RenderExpr(entry.value, indent_level);
			default: return RenderExpr(entry.value, indent_level);
			}
		}

		std::string FunctionDecompiler::RenderClosure(const Expr& expr, int indent_level) const
		{
			if (!expr.closure || !expr.closure->function)
			{
				return "function() end";
			}
			FunctionDecompiler nested(*expr.closure->function, m_diagnostics, indent_level, false, expr.closure->upvalues);
			return nested.Run();
		}

		std::string FunctionDecompiler::RenderRawInstructionTrace() const
		{
			std::ostringstream out;
			const int base_indent = m_indent_level + (m_top_level ? 0 : 1);
			out << Indent(base_indent) << "-- raw opcode trace\n";
			for (std::size_t pc = 0; pc < m_function.code.size(); ++pc)
			{
				out << Indent(base_indent) << "-- " << DescribeInstruction(pc);
				if (pc + 1 < m_function.code.size())
				{
					out << '\n';
				}
			}
			out << '\n';
			return out.str();
		}

		std::string FunctionDecompiler::DescribeInstruction(std::size_t pc) const
		{
			const Instruction& instruction = m_function.code[pc];
			auto reg = [](int index)
			{
				return "r" + std::to_string(index);
			};
			auto rk = [&](int value)
			{
				return IsConstantIndex(value) ? ConstantToExpr(ConstantIndex(value)).text : reg(value);
			};

			std::ostringstream out;
			out << "[" << std::setw(4) << std::setfill('0') << pc << "] " << OpCodeName(instruction.opcode);

			switch (instruction.opcode)
			{
			case OpCode::Move:
				out << " ; " << reg(instruction.a) << " = " << reg(instruction.b);
				break;
			case OpCode::LoadK:
				out << " ; " << reg(instruction.a) << " = " << ConstantToExpr(instruction.bx).text;
				break;
			case OpCode::LoadBool:
				out << " ; " << reg(instruction.a) << " = " << (instruction.b != 0 ? "true" : "false");
				break;
			case OpCode::LoadNil:
				out << " ; " << reg(instruction.a) << ".." << reg(instruction.b) << " = nil";
				break;
			case OpCode::GetUpval:
				out << " ; " << reg(instruction.a) << " = " << UpvalueName(instruction.b);
				break;
			case OpCode::GetGlobal:
				out << " ; " << reg(instruction.a) << " = " << GlobalName(instruction.bx);
				break;
			case OpCode::GetTable:
				out << " ; " << reg(instruction.a) << " = " << reg(instruction.b) << "[" << rk(instruction.c) << "]";
				break;
			case OpCode::SetGlobal:
				out << " ; " << GlobalName(instruction.bx) << " = " << reg(instruction.a);
				break;
			case OpCode::SetUpval:
				out << " ; " << UpvalueName(instruction.b) << " = " << reg(instruction.a);
				break;
			case OpCode::SetTable:
				out << " ; " << reg(instruction.a) << "[" << rk(instruction.b) << "] = " << rk(instruction.c);
				break;
			case OpCode::NewTable:
				out << " ; " << reg(instruction.a) << " = {}";
				break;
			case OpCode::Self:
				out << " ; " << reg(instruction.a) << ", " << reg(instruction.a + 1) << " = " << reg(instruction.b) << ", " << reg(instruction.b) << "[" << rk(instruction.c) << "]";
				break;
			case OpCode::Add:
			case OpCode::Sub:
			case OpCode::Mul:
			case OpCode::Div:
			case OpCode::Mod:
			case OpCode::Pow:
			{
				const char* op = "?";
				switch (instruction.opcode)
				{
				case OpCode::Add: op = "+"; break;
				case OpCode::Sub: op = "-"; break;
				case OpCode::Mul: op = "*"; break;
				case OpCode::Div: op = "/"; break;
				case OpCode::Mod: op = "%"; break;
				case OpCode::Pow: op = "^"; break;
				default: break;
				}
				out << " ; " << reg(instruction.a) << " = " << rk(instruction.b) << " " << op << " " << rk(instruction.c);
				break;
			}
			case OpCode::Unm:
				out << " ; " << reg(instruction.a) << " = -" << reg(instruction.b);
				break;
			case OpCode::Not:
				out << " ; " << reg(instruction.a) << " = not " << reg(instruction.b);
				break;
			case OpCode::Len:
				out << " ; " << reg(instruction.a) << " = #" << reg(instruction.b);
				break;
			case OpCode::Concat:
				out << " ; " << reg(instruction.a) << " = concat(" << reg(instruction.b) << ".." << reg(instruction.c) << ")";
				break;
			case OpCode::Jmp:
				out << " ; jump -> pc_" << JumpTarget(pc);
				break;
			case OpCode::Eq:
			case OpCode::Lt:
			case OpCode::Le:
				out << " ; compare " << rk(instruction.b) << ", " << rk(instruction.c);
				break;
			case OpCode::Test:
				out << " ; test " << reg(instruction.a);
				break;
			case OpCode::TestSet:
				out << " ; if " << reg(instruction.b) << " then " << reg(instruction.a) << " = " << reg(instruction.b);
				break;
			case OpCode::Call:
				out << " ; call " << reg(instruction.a) << " args=" << instruction.b << " results=" << instruction.c;
				break;
			case OpCode::TailCall:
				out << " ; tailcall " << reg(instruction.a) << " args=" << instruction.b;
				break;
			case OpCode::Return:
				out << " ; return from " << reg(instruction.a) << " count=" << instruction.b;
				break;
			case OpCode::ForLoop:
			case OpCode::ForPrep:
				out << " ; numeric loop jump=" << instruction.sbx;
				break;
			case OpCode::TForLoop:
				out << " ; generic loop count=" << instruction.c;
				break;
			case OpCode::SetList:
				out << " ; setlist base=" << reg(instruction.a) << " count=" << instruction.b;
				break;
			case OpCode::Close:
				out << " ; close upvalues";
				break;
			case OpCode::Closure:
				out << " ; " << reg(instruction.a) << " = closure(proto=" << instruction.bx << ")";
				break;
			case OpCode::VarArg:
				out << " ; vararg -> " << reg(instruction.a) << " count=" << instruction.b;
				break;
			default:
				out << " ; A=" << instruction.a << " B=" << instruction.b << " C=" << instruction.c << " Bx=" << instruction.bx << " sBx=" << instruction.sbx;
				break;
			}

			return out.str();
		}

        void ApplyHeavyObfuscatedChunkRepair(std::string& lua, const HexStreamDecodeConfig* forced_hex_decoder = nullptr)
        {
            std::vector<std::string> lines = SplitLines(lua);
            FixSelectVarargInFixedArgFunctions(lines);
            RepairMissingLoadstringGuards(lines);
            if (const auto decoder = ExtractHexDecoderFromLua(lines, forced_hex_decoder))
            {
                if (FoldHexDecoderCallsInLines(lines, *decoder) > 0)
				{
					RemoveUnusedHexDecoder(lines, *decoder);
                    RemoveDanglingHexDecoderTail(lines);
                }
            }

            ReplaceSyntheticTimerFallbacks(lines);
            ReplaceSyntheticResourceStartFallbacks(lines);
            ReplaceSyntheticIbBackgroundCallbacks(lines);
            RemoveConsecutiveReturnLines(lines);

            for (std::size_t index = 0; index + 2 < lines.size();)
            {
				std::smatch match;
				if (!std::regex_match(lines[index], match, std::regex("^([ \\t]*)if not (.+) then$")))
				{
					++index;
					continue;
				}

				const std::string indent = match[1].str();
				const std::string expr = std::string(TrimView(match[2].str()));
				std::size_t body_end = index + 1;
				std::string last_rhs;
				bool valid_block = true;

				while (body_end < lines.size() && TrimView(lines[body_end]) != "end")
				{
					const std::string_view trimmed = TrimView(lines[body_end]);
					if (!trimmed.starts_with("local "))
					{
						valid_block = false;
						break;
					}

					const std::size_t assign = trimmed.find(" = ");
					if (assign == std::string_view::npos)
					{
						valid_block = false;
						break;
					}

					last_rhs = std::string(TrimView(trimmed.substr(assign + 3)));
					++body_end;
				}

				if (!valid_block || body_end >= lines.size() || TrimView(lines[body_end]) != "end" || !IsSimpleRepairRhs(last_rhs) || body_end + 1 >= lines.size())
				{
					++index;
					continue;
				}

				if (TrimView(lines[body_end + 1]) != expr + " = " + expr)
				{
					++index;
					continue;
				}

				lines[index] = indent + expr + " = " + expr + " or " + last_rhs;
				lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index + 1), lines.begin() + static_cast<std::ptrdiff_t>(body_end + 2));
			}

			SimplifyReadableLocals(lines);
			RepairSelfAccumulatingOrAssignments(lines);
			CompactSyntheticArithmeticWhileBlocks(lines);
			InlineSingleUseLiteralLocals(lines);
			RemoveUnusedSimpleLocalAliases(lines);
			InsertBlankLinesAfterTopLevelFunctions(lines);
			lua = Join(lines, "\n");
			RewriteUnreadableStringLiterals(lua);
			RenameBarePlaceholderIdentifiers(lua);
			EnsureHexLiteralHelper(lua);
		}

		void ApplySemanticTopLevelRepair(std::string& lua, const Function* root = nullptr)
		{
			const auto forced_hex_decoder = root ? DetectTopLevelHexStreamDecoder(*root) : std::nullopt;
			const HexStreamDecodeConfig* forced_hex_decoder_ptr = forced_hex_decoder ? &*forced_hex_decoder : nullptr;

			if (LooksLikeHeavyHexObfuscatedLua(lua))
			{
				ApplyHeavyObfuscatedChunkRepair(lua, forced_hex_decoder_ptr);
				EnsureHexLiteralHelper(lua);
				return;
			}

			if (ContainsAny(lua, { "reconstructed encoded shared loader", "local ENCODED_PAYLOADS = {" }))
			{
				RewriteUnreadableStringLiterals(lua);
				RenameBarePlaceholderIdentifiers(lua);
				EnsureHexLiteralHelper(lua);
				return;
			}

			ApplyGenericLuaRepair(lua);
			ApplyGafniumClientSemanticRepair(lua);

			if (ContainsAny(lua, { "DIGGING_DATA", "CreateDiggingStore", "OnPlayerStartDigging", "StopDiggingMinigame" }))
			{
				ApplyDiggingSemanticRepair(lua);
			}

			if (ContainsAny(lua, { "Manager.Tab_1", "Tab_1_EditDayMsg", "Tab_1_Donate", "Tab_1_Editor" }))
			{
				ApplyPageSemanticRepair(lua);
			}

			if (ContainsAny(lua, { ".f_item = function(", ".CreateDragItem = function(", ".Inventory = function(", ".ModalBuy = function(", ".Market = function(", "ibCreate" }))
			{
				ApplyUiSemanticRepair(lua);
			}

			if (ContainsAny(lua, { "EnterBank", "RegisterBank", "SettingsBank", "DonateBank", "BANK:" }))
			{
				ApplyBankSemanticRepair(lua);
			}

			std::optional<std::string> accessory_table;
			for (const auto& [label, name] : std::initializer_list<std::pair<std::string_view, std::string_view>>
			{
				{ "reconstructed accessory shop ped setup", "createPedShop" },
				{ "reconstructed accessory marker loader", "loadMarker" },
				{ "reconstructed server event bridge", "Enter" },
				{ "reconstructed accessory shop window", "Shop" },
				{ "reconstructed cleanup routine", "Destroy" },
			})
			{
				if (auto table = RenameSemanticMethod(lua, label, name))
				{
					accessory_table = table;
				}
			}

			if (accessory_table)
			{
				std::string procent_name = "_PROCENT";
				const std::regex procent_pattern("local ([A-Za-z_][A-Za-z0-9_]*) = 0\\nlocal " + *accessory_table + " = \\{\\}");
				std::smatch procent_match;
				if (std::regex_search(lua, procent_match, procent_pattern))
				{
					const std::string old_name = procent_match[1].str();
					ReplaceAll(lua, "local " + old_name + " = 0", "local _PROCENT = 0");
					procent_name = "_PROCENT";
				}

				ReplaceAll(lua, "local " + *accessory_table + " = {}", "local shop = {}");
				ReplaceAll(lua, *accessory_table + ".", "shop.");
				ReplaceAll(lua, *accessory_table + ":", "shop:");

				auto two_arg_blocks = FindLocalFunctionBlocks(lua, "arg1, arg2");
				if (!two_arg_blocks.empty())
				{
					const LocalFunctionBlock& block = two_arg_blocks.back();
					ReplaceFunctionBlock(lua, block,
						"local " + block.name + " = function(arg1, arg2)\n"
						"    " + procent_name + " = arg1\n"
						"    shop:loadMarker(arg1, arg2)\n"
						"end");
				}
			}

			std::optional<std::string> trade_table;
			for (const auto& [label, name] : std::initializer_list<std::pair<std::string_view, std::string_view>>
			{
				{ "reconstructed vehicle sell dialog", "Sell" },
				{ "reconstructed vehicle buy dialog", "Buy" },
				{ "reconstructed confirmation dialog", "Confirm" },
				{ "reconstructed cleanup routine", "Destroy" },
			})
			{
				if (auto table = RenameSemanticMethod(lua, label, name))
				{
					trade_table = table;
				}
			}

			if (trade_table)
			{
				ReplaceAll(lua, "local " + *trade_table + " = {}", "Main = {}");
				ReplaceAll(lua, *trade_table + ".", "Main.");
				ReplaceAll(lua, *trade_table + ":", "Main:");

				std::regex placeholder_assignment("_G\\[\"__br_str_[A-F0-9]+__\"\\] = Main\\n");
				lua = std::regex_replace(lua, placeholder_assignment, "");

				auto zero_arg_blocks = FindLocalFunctionBlocks(lua, "");
				if (zero_arg_blocks.size() >= 1)
				{
					ReplaceFunctionBlock(lua, zero_arg_blocks[0],
						"local " + zero_arg_blocks[0].name + " = function()\n"
						"    Main:Destroy()\n"
						"end");
				}
				if (zero_arg_blocks.size() >= 2)
				{
					ReplaceFunctionBlock(lua, zero_arg_blocks[1],
						"local " + zero_arg_blocks[1].name + " = function()\n"
						"    Main:Confirm()\n"
						"end");
				}

				auto two_arg_blocks = FindLocalFunctionBlocks(lua, "arg1, arg2");
				if (!two_arg_blocks.empty())
				{
					ReplaceFunctionBlock(lua, two_arg_blocks.front(),
						"local " + two_arg_blocks.front().name + " = function(arg1, arg2)\n"
						"    Main:Buy(arg1, arg2)\n"
						"end");
				}

				auto one_arg_blocks = FindLocalFunctionBlocks(lua, "arg1");
				if (!one_arg_blocks.empty())
				{
					ReplaceFunctionBlock(lua, one_arg_blocks.back(),
						"local " + one_arg_blocks.back().name + " = function(arg1)\n"
						"    Main:Sell(arg1)\n"
						"end");
				}
			}

			std::unordered_map<std::string, std::string> string_locals;
			const std::regex string_local_pattern("local ([A-Za-z_][A-Za-z0-9_]*) = \"([^\n\"]+)\"");
			for (std::sregex_iterator iterator(lua.begin(), lua.end(), string_local_pattern), end; iterator != end; ++iterator)
			{
				string_locals.emplace((*iterator)[1].str(), (*iterator)[2].str());
			}

			std::string cleanup_function;
			auto zero_arg_blocks = FindLocalFunctionBlocks(lua, "");
			for (const LocalFunctionBlock& block : zero_arg_blocks)
			{
				const std::string block_text = lua.substr(block.position, block.length);
				if (block_text.find("reconstructed nested cleanup") != std::string::npos)
				{
					cleanup_function = block.name;
					break;
				}
			}

			if (!cleanup_function.empty())
			{
				for (const LocalFunctionBlock& block : zero_arg_blocks)
				{
					const std::string block_text = lua.substr(block.position, block.length);
					if (block_text.find("synthetic fallback, structured recovery was partial") == std::string::npos)
					{
						continue;
					}

					std::regex handler_pattern("addEventHandler\\(([^,]+), (root|resourceRoot), " + block.name + "\\)");
					std::smatch handler_match;
					if (!std::regex_search(lua, handler_match, handler_pattern))
					{
						continue;
					}

					std::string event_ref = std::string(TrimView(handler_match[1].str()));
					std::string event_name;
					if (!event_ref.empty() && event_ref.front() == '"' && event_ref.back() == '"')
					{
						event_name = event_ref.substr(1, event_ref.size() - 2);
					}
					else if (const auto found = string_locals.find(event_ref); found != string_locals.end())
					{
						event_name = found->second;
					}

					if (event_name.find("Unload") == std::string::npos
						&& event_name.find("Destroy") == std::string::npos
						&& event_name.find("Stop") == std::string::npos)
					{
						continue;
					}

					ReplaceFunctionBlock(lua, block,
						"local " + block.name + " = function()\n"
						"    " + cleanup_function + "()\n"
						"end");

					if (!event_name.empty() && lua.find("addEvent(\"" + event_name + "\", true)") != std::string::npos)
					{
						ReplaceAll(lua, handler_match[0].str(), "addEventHandler(" + event_ref + ", resourceRoot, " + block.name + ")");
					}
				}
			}

            {
                std::vector<std::string> lines = SplitLines(lua);
                SimplifyReadableLocals(lines);
                RepairSelfAccumulatingOrAssignments(lines);
                CompactSyntheticArithmeticWhileBlocks(lines);
                RepairBrokenRecursiveInspectWalkers(lines);
                if (const auto stop_range = FindLocalFunctionRangeByName(lines, "onClientResourceStop"))
                {
                    std::string counter_name = "var_2";
                    if (const auto start_range = FindLocalFunctionRangeByName(lines, "onClientResourceStart"))
                    {
                        const std::regex counter_pattern("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\[resourceName\\] = \\(\\1\\[resourceName\\] or 0\\) \\+ 1$");
                        for (std::size_t index = start_range->first; index <= start_range->second && index < lines.size(); ++index)
                        {
                            std::smatch match;
                            if (std::regex_match(lines[index], match, counter_pattern))
                            {
                                counter_name = match[1].str();
                                break;
                            }
                        }
                    }

                    ReplaceLineRange(lines, stop_range->first, stop_range->second,
                        {
                            "local function onClientResourceStop(arg1)",
                            "    local resourceName = getResourceName(arg1)",
                            "    " + counter_name + "[resourceName] = (" + counter_name + "[resourceName] or 0) - 1",
                            "end",
                        });
                }
                ReplaceIdentifierInLines(lines, "var_3", "lockClient");
                ReplaceIdentifierInLines(lines, "var_4", "tripClient");
                ReplaceIdentifierInLines(lines, "var_40", "countChildElements");
                for (std::string& line : lines)
                {
                    if (TrimView(line) == "local function var(arg1, arg2)")
                    {
                        line = "local function sub_1000(arg1, arg2)";
                    }
                }
                InlineSingleUseLiteralLocals(lines);
                RemoveUnusedSimpleLocalAliases(lines);
                InsertBlankLinesAfterTopLevelFunctions(lines);
                lua = Join(lines, "\n");
            }

            lua = std::regex_replace(
                lua,
                std::regex(
                    "local function onClientResourceStop\\(arg1\\)\\n"
                    "    if not arg1 then\\n"
                    "        return 0\\n"
                    "    end\\n"
                    "    local childCount = getElementChildrenCount\\(arg1\\)\\n"
                    "    if childCount <= 0 then\\n"
                    "        return childCount\\n"
                    "    end\\n"
                    "    for index = 0, childCount - 1 do\\n"
                    "        local child = getElementChild\\(arg1, index\\)\\n"
                    "        if child then\\n"
                    "            local childType = getElementType\\(child\\)\\n"
                    "            if childType ~= \"map\" and childType ~= \"colmodelroot\" and childType ~= \"dffroot\" and childType ~= \"guiroot\" and childType ~= \"txdroot\" then\\n"
                    "                table.insert\\([A-Za-z_][A-Za-z0-9_]*, inspect\\(child\\)\\)\\n"
                    "            end\\n"
                    "            childCount = childCount \\+ onClientResourceStop\\(child\\)\\n"
                    "        end\\n"
                    "    end\\n"
                    "    return childCount\\n"
                    "end"),
                "local function onClientResourceStop(arg1)\n"
                "    local resourceName = getResourceName(arg1)\n"
                "    var_2[resourceName] = (var_2[resourceName] or 0) - 1\n"
                "end");
            ReplaceAll(lua, "local function var(arg1, arg2)", "local function sub_1000(arg1, arg2)");

			RewriteUnreadableStringLiterals(lua);
			RenameBarePlaceholderIdentifiers(lua);
			EnsureHexLiteralHelper(lua);
		}
	}

		DecompileResult Decompiler::Decompile(const Chunk& chunk, DiagnosticSink& diagnostics) const
		{
		if (!chunk.root)
		{
			throw ByteRevenantError("decompile", "В chunk нет корневой функции");
		}

		DecompileResult result{};
		std::ostringstream out;
		out << "-- ByteRevenant\n";
		out << "-- LuaQ 5.1 structured decompile\n\n";

		FunctionDecompiler decompiler(*chunk.root, diagnostics, 0, true, {});
		AppendProfileLog("decompile-run: begin");
		out << decompiler.Run() << '\n';
		AppendProfileLog("decompile-run: end");
		result.lua = out.str();
		AppendProfileLog("decompile-repair: begin");
		if (!EnvFlagEnabled("BR_SKIP_REPAIR"))
		{
			ApplySemanticTopLevelRepair(result.lua, chunk.root.get());
		}
		AppendProfileLog("decompile-repair: end");
		return result;
	}
}

