#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace br
{
	enum class Severity
	{
		Info,
		Warning,
		Error,
	};

	struct Diagnostic
	{
		Severity severity = Severity::Info;
		std::string stage;
		std::string message;
		std::optional<std::size_t> offset;
	};

	class DiagnosticSink
	{
	public:
		void Add(Severity severity, std::string stage, std::string message, std::optional<std::size_t> offset = std::nullopt)
		{
			m_items.push_back(Diagnostic{ severity, std::move(stage), std::move(message), offset });
		}

		void Info(std::string stage, std::string message, std::optional<std::size_t> offset = std::nullopt)
		{
			Add(Severity::Info, std::move(stage), std::move(message), offset);
		}

		void Warning(std::string stage, std::string message, std::optional<std::size_t> offset = std::nullopt)
		{
			Add(Severity::Warning, std::move(stage), std::move(message), offset);
		}

		void Error(std::string stage, std::string message, std::optional<std::size_t> offset = std::nullopt)
		{
			Add(Severity::Error, std::move(stage), std::move(message), offset);
		}

		const std::vector<Diagnostic>& Items() const
		{
			return m_items;
		}

	private:
		std::vector<Diagnostic> m_items;
	};

	class ByteRevenantError : public std::runtime_error
	{
	public:
		ByteRevenantError(std::string stage, std::string message, std::optional<std::size_t> offset = std::nullopt)
			: std::runtime_error(message)
			, m_stage(std::move(stage))
			, m_offset(offset)
		{
		}

		const std::string& Stage() const
		{
			return m_stage;
		}

		const std::optional<std::size_t>& Offset() const
		{
			return m_offset;
		}

	private:
		std::string m_stage;
		std::optional<std::size_t> m_offset;
	};

	inline std::string Hex(std::size_t value)
	{
		std::ostringstream stream;
		stream << "0x" << std::hex << std::uppercase << value;
		return stream.str();
	}

	inline std::string PathToUtf8(const std::filesystem::path& path)
	{
		const auto value = path.u8string();
		return std::string(value.begin(), value.end());
	}

	inline std::vector<std::uint8_t> ReadAllBytes(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			throw ByteRevenantError("io", "Не удалось открыть файл: " + PathToUtf8(path));
		}

		file.seekg(0, std::ios::end);
		const auto size = file.tellg();
		file.seekg(0, std::ios::beg);

		if (size < 0)
		{
			throw ByteRevenantError("io", "Не удалось определить размер файла: " + PathToUtf8(path));
		}

		std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
		if (!data.empty())
		{
			file.read(reinterpret_cast<char*>(data.data()), size);
		}

		if (!file.good() && !file.eof())
		{
			throw ByteRevenantError("io", "Не удалось прочитать файл: " + PathToUtf8(path));
		}

		return data;
	}

	inline void WriteAllBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& data)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			throw ByteRevenantError("io", "Не удалось создать файл: " + PathToUtf8(path));
		}

		if (!data.empty())
		{
			file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		}

		if (!file.good())
		{
			throw ByteRevenantError("io", "Не удалось записать файл: " + PathToUtf8(path));
		}
	}

	inline void WriteAllText(const std::filesystem::path& path, const std::string& text)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			throw ByteRevenantError("io", "Не удалось создать файл: " + PathToUtf8(path));
		}

		file.write(text.data(), static_cast<std::streamsize>(text.size()));
		if (!file.good())
		{
			throw ByteRevenantError("io", "Не удалось записать файл: " + PathToUtf8(path));
		}
	}

	inline bool IsIdentifier(std::string_view value)
	{
		if (value.empty())
		{
			return false;
		}

		auto valid_first = [](char ch)
		{
			return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
		};
		auto valid_other = [](char ch)
		{
			return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
		};

		if (!valid_first(value.front()))
		{
			return false;
		}

		for (char ch : value.substr(1))
		{
			if (!valid_other(ch))
			{
				return false;
			}
		}

		return true;
	}

	class BinaryReader
	{
	public:
		explicit BinaryReader(const std::vector<std::uint8_t>& data)
			: m_data(data)
		{
		}

		void SetLittleEndian(bool little_endian)
		{
			m_little_endian = little_endian;
		}

		std::size_t Position() const
		{
			return m_position;
		}

		std::size_t Remaining() const
		{
			return m_data.size() - m_position;
		}

		std::uint8_t ReadU8(std::string_view stage)
		{
			Ensure(stage, 1);
			return m_data[m_position++];
		}

		std::uint64_t ReadUnsigned(std::size_t width, std::string_view stage)
		{
			if (width == 0 || width > 8)
			{
				throw ByteRevenantError(std::string(stage), "Неподдерживаемая ширина целого", m_position);
			}

			Ensure(stage, width);
			std::uint64_t value = 0;
			if (m_little_endian)
			{
				for (std::size_t index = 0; index < width; ++index)
				{
					value |= static_cast<std::uint64_t>(m_data[m_position + index]) << (index * 8);
				}
			}
			else
			{
				for (std::size_t index = 0; index < width; ++index)
				{
					value = (value << 8) | m_data[m_position + index];
				}
			}

			m_position += width;
			return value;
		}

		double ReadNumber(std::size_t width, bool integral, std::string_view stage)
		{
			if (integral)
			{
				return static_cast<double>(static_cast<std::int64_t>(ReadUnsigned(width, stage)));
			}

			Ensure(stage, width);
			double value = 0.0;
			if (width == 8)
			{
				std::array<std::uint8_t, 8> raw{};
				for (std::size_t index = 0; index < 8; ++index)
				{
					raw[index] = m_data[m_position + index];
				}
				if (!m_little_endian)
				{
					std::reverse(raw.begin(), raw.end());
				}
				std::memcpy(&value, raw.data(), sizeof(value));
			}
			else if (width == 4)
			{
				std::array<std::uint8_t, 4> raw{};
				float single = 0.0f;
				for (std::size_t index = 0; index < 4; ++index)
				{
					raw[index] = m_data[m_position + index];
				}
				if (!m_little_endian)
				{
					std::reverse(raw.begin(), raw.end());
				}
				std::memcpy(&single, raw.data(), sizeof(single));
				value = static_cast<double>(single);
			}
			else
			{
				throw ByteRevenantError(std::string(stage), "Неподдерживаемый размер lua_Number", m_position);
			}

			m_position += width;
			return value;
		}

		std::string ReadString(std::size_t size_width, std::string_view stage)
		{
			const auto length = static_cast<std::size_t>(ReadUnsigned(size_width, stage));
			if (length == 0)
			{
				return {};
			}

			Ensure(stage, length);
			std::string result(reinterpret_cast<const char*>(m_data.data() + m_position), length - 1);
			m_position += length;
			return result;
		}

		std::vector<std::uint8_t> ReadBytes(std::size_t count, std::string_view stage)
		{
			Ensure(stage, count);
			std::vector<std::uint8_t> result(
				m_data.begin() + static_cast<std::ptrdiff_t>(m_position),
				m_data.begin() + static_cast<std::ptrdiff_t>(m_position + count));
			m_position += count;
			return result;
		}

	private:
		void Ensure(std::string_view stage, std::size_t count) const
		{
			if (m_position + count > m_data.size())
			{
				throw ByteRevenantError(std::string(stage), "Байт-код оборван, не хватает данных", m_position);
			}
		}

		const std::vector<std::uint8_t>& m_data;
		std::size_t m_position = 0;
		bool m_little_endian = true;
	};
}
