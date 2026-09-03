#include "Lua51.hpp"

namespace br::lua51
{
	namespace
	{
		struct ParseContext
		{
			DiagnosticSink& diagnostics;
			std::size_t string_recoveries = 0;
		};

		constexpr int kSizeOp = 6;
		constexpr int kSizeA = 8;
		constexpr int kSizeB = 9;
		constexpr int kSizeC = 9;
		constexpr int kPosOp = 0;
		constexpr int kPosA = kPosOp + kSizeOp;
		constexpr int kPosC = kPosA + kSizeA;
		constexpr int kPosB = kPosC + kSizeC;
		constexpr int kPosBx = kPosC;
		constexpr int kBitRk = 1 << (kSizeB - 1);
		constexpr int kOpMtaNop = 0x26;
		constexpr int kOpMtaXor = 0x27;
		constexpr int kOpMtaFail = 0x28;

		constexpr std::array<const char*, 38> kOpNames = {
			"MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE",
			"SETGLOBAL", "SETUPVAL", "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB",
			"MUL", "DIV", "MOD", "POW", "UNM", "NOT", "LEN", "CONCAT", "JMP",
			"EQ", "LT", "LE", "TEST", "TESTSET", "CALL", "TAILCALL", "RETURN",
			"FORLOOP", "FORPREP", "TFORLOOP", "SETLIST", "CLOSE", "CLOSURE", "VARARG"
		};

		constexpr std::array<OpMode, 38> kModes = {
			OpMode::iABC, OpMode::iABx, OpMode::iABC, OpMode::iABC, OpMode::iABC,
			OpMode::iABx, OpMode::iABC, OpMode::iABx, OpMode::iABC, OpMode::iABC,
			OpMode::iABC, OpMode::iABC, OpMode::iABC, OpMode::iABC, OpMode::iABC,
			OpMode::iABC, OpMode::iABC, OpMode::iABC, OpMode::iABC, OpMode::iABC,
			OpMode::iABC, OpMode::iABC, OpMode::iAsBx, OpMode::iABC, OpMode::iABC,
			OpMode::iABC, OpMode::iABC, OpMode::iABC, OpMode::iABC, OpMode::iABC,
			OpMode::iABC, OpMode::iAsBx, OpMode::iAsBx, OpMode::iABC, OpMode::iABC,
			OpMode::iABC, OpMode::iABx, OpMode::iABC
		};

		constexpr std::array<std::uint8_t, 256> kLuaQCryptTable = {
			0x8E, 0x79, 0x64, 0xBB, 0x77, 0xEA, 0x60, 0xD2, 0xCA, 0x3C, 0x1A, 0x1A, 0xC4, 0xC1, 0x98, 0x1E,
			0x1E, 0x61, 0x99, 0xFD, 0xEB, 0x26, 0xAD, 0xC2, 0x3E, 0x87, 0xDC, 0x4B, 0x63, 0x0A, 0x9B, 0xDA,
			0xBC, 0x9D, 0xA8, 0x90, 0x27, 0xD2, 0xBA, 0xAB, 0xE0, 0x49, 0xC0, 0xE7, 0xE3, 0x1D, 0xB9, 0x4C,
			0x70, 0x64, 0x4B, 0x6B, 0x69, 0x96, 0xD4, 0xFF, 0x61, 0xDD, 0x37, 0x85, 0x64, 0xD6, 0x10, 0x43,
			0xBA, 0x85, 0xE0, 0x24, 0x0B, 0xFB, 0x1F, 0xC8, 0x24, 0x14, 0x8F, 0x1B, 0x8F, 0x66, 0xF3, 0x20,
			0xDF, 0xBA, 0xEF, 0x36, 0x10, 0x71, 0xE0, 0xFB, 0x0D, 0x1D, 0x99, 0x80, 0x10, 0x51, 0x9B, 0x19,
			0xDB, 0xAB, 0x1D, 0x7E, 0x13, 0xC3, 0xC1, 0xCB, 0x4C, 0xBB, 0xFE, 0x2C, 0x69, 0x94, 0xE7, 0x56,
			0xD5, 0x88, 0x63, 0x16, 0xD5, 0xFB, 0xA1, 0xC4, 0x55, 0x91, 0x5D, 0x6D, 0x51, 0xD7, 0x19, 0x3C,
			0x95, 0x43, 0x66, 0x36, 0x7B, 0xAF, 0xD7, 0x99, 0x75, 0xE5, 0x32, 0xA7, 0x13, 0xA7, 0x5E, 0xF8,
			0x39, 0xAB, 0x57, 0x45, 0x87, 0xDC, 0x8B, 0xA1, 0x09, 0x21, 0x0D, 0xB3, 0xBB, 0xE1, 0x57, 0xEE,
			0xDD, 0x62, 0xC7, 0x23, 0xFC, 0x3F, 0x91, 0xBD, 0x7B, 0xA5, 0xAF, 0x3D, 0xEA, 0x7E, 0x75, 0x49,
			0xC1, 0xB2, 0x0D, 0x4D, 0x65, 0xA9, 0x21, 0x9A, 0xF1, 0x05, 0xA7, 0x63, 0x78, 0x6D, 0x83, 0xF9,
			0xC6, 0x5C, 0xF7, 0xF6, 0xCD, 0xCA, 0x76, 0x7A, 0x7A, 0x2D, 0xE7, 0x84, 0x67, 0xDD, 0x65, 0x99,
			0x26, 0x02, 0xCF, 0x95, 0xA1, 0xC0, 0x32, 0x88, 0xC5, 0x04, 0x92, 0x77, 0xB4, 0xB9, 0x7B, 0x4A,
			0x31, 0x42, 0x5D, 0x18, 0x0C, 0x2C, 0xFA, 0x46, 0x34, 0x76, 0xD9, 0xC2, 0xA7, 0xA4, 0xAC, 0x69,
			0xF0, 0xE1, 0x74, 0x79, 0x28, 0xB0, 0x64, 0xEA, 0x6D, 0x84, 0x86, 0xE6, 0x79, 0xEF, 0xB6, 0xB7
		};

		struct NormalizationStats
		{
			bool touched = false;
			std::size_t decrypted_strings = 0;
			std::size_t decrypted_code_blocks = 0;
			std::size_t removed_nops = 0;
			std::size_t fixed_jumps = 0;
		};

		std::uint32_t ReadLeU32At(const std::vector<std::uint8_t>& data, std::size_t offset, std::string_view stage)
		{
			if (offset + 4 > data.size())
			{
				throw ByteRevenantError(std::string(stage), "Выход за границы при чтении uint32", offset);
			}

			return static_cast<std::uint32_t>(data[offset])
				| (static_cast<std::uint32_t>(data[offset + 1]) << 8)
				| (static_cast<std::uint32_t>(data[offset + 2]) << 16)
				| (static_cast<std::uint32_t>(data[offset + 3]) << 24);
		}

		void WriteLeU32At(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value, std::string_view stage)
		{
			if (offset + 4 > data.size())
			{
				throw ByteRevenantError(std::string(stage), "Выход за границы при записи uint32", offset);
			}

			data[offset] = static_cast<std::uint8_t>(value & 0xFFu);
			data[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
			data[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
			data[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
		}

		void EraseBytes(std::vector<std::uint8_t>& data, std::size_t offset, std::size_t count, std::size_t& position, std::string_view stage)
		{
			if (offset + count > data.size())
			{
				throw ByteRevenantError(std::string(stage), "Выход за границы при удалении байтов", offset);
			}

			data.erase(data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + count));
			if (offset < position)
			{
				const std::size_t shrink = std::min(count, position - offset);
				position -= shrink;
			}
		}

		std::uint32_t LuaMask(int bits)
		{
			return (1u << bits) - 1u;
		}

		int LuaGetArg(std::uint32_t raw, int position, int size)
		{
			return static_cast<int>((raw >> position) & LuaMask(size));
		}

		int LuaGetOpcode(std::uint32_t raw)
		{
			return LuaGetArg(raw, kPosOp, kSizeOp);
		}

		int LuaGetArgA(std::uint32_t raw)
		{
			return LuaGetArg(raw, kPosA, kSizeA);
		}

		int LuaGetArgBx(std::uint32_t raw)
		{
			return LuaGetArg(raw, kPosBx, kSizeB + kSizeC);
		}

		int LuaGetArgSBx(std::uint32_t raw)
		{
			return LuaGetArgBx(raw) - (static_cast<int>(LuaMask(kSizeB + kSizeC)) >> 1);
		}

		std::uint32_t LuaSetArg(std::uint32_t raw, int value, int position, int size)
		{
			const std::uint32_t mask = LuaMask(size) << position;
			return (raw & ~mask) | ((static_cast<std::uint32_t>(value) << position) & mask);
		}

		std::uint32_t LuaSetArgBx(std::uint32_t raw, int value)
		{
			return LuaSetArg(raw, value, kPosBx, kSizeB + kSizeC);
		}

		std::uint32_t LuaSetArgSBx(std::uint32_t raw, int value)
		{
			return LuaSetArgBx(raw, value + (static_cast<int>(LuaMask(kSizeB + kSizeC)) >> 1));
		}

		std::string ToLowerAscii(std::string text)
		{
			for (char& ch : text)
			{
				if (ch >= 'A' && ch <= 'Z')
				{
					ch = static_cast<char>(ch - 'A' + 'a');
				}
			}
			return text;
		}

		bool LooksLikeLuaSourceText(const std::vector<std::uint8_t>& data)
		{
			if (data.empty())
			{
				return false;
			}

			const std::size_t sample = std::min<std::size_t>(data.size(), 512);
			std::size_t printable = 0;
			std::size_t zero_bytes = 0;
			std::string ascii;
			ascii.reserve(sample);
			for (std::size_t index = 0; index < sample; ++index)
			{
				const std::uint8_t ch = data[index];
				if (ch == 0)
				{
					++zero_bytes;
				}
				if ((ch >= 0x20 && ch <= 0x7E) || ch == '\t' || ch == '\r' || ch == '\n')
				{
					++printable;
					ascii.push_back(static_cast<char>(ch));
				}
				else if (ch < 0x80)
				{
					ascii.push_back(' ');
				}
			}

			if (zero_bytes != 0 || printable * 100 < sample * 85)
			{
				return false;
			}

			const std::string lowered = ToLowerAscii(ascii);
			return lowered.find("function") != std::string::npos
				|| lowered.find("local ") != std::string::npos
				|| lowered.find("return ") != std::string::npos
				|| lowered.find("addeventhandler") != std::string::npos
				|| lowered.find("triggerserverevent") != std::string::npos
				|| lowered.find("exports.") != std::string::npos
				|| lowered.find("--") != std::string::npos;
		}

		class MutableLuaQNormalizer
		{
		public:
			MutableLuaQNormalizer(std::vector<std::uint8_t>& data, DiagnosticSink& diagnostics)
				: m_data(data)
				, m_diagnostics(diagnostics)
			{
			}

			bool Run(NormalizationStats& stats)
			{
				if (m_data.size() < 12)
				{
					return false;
				}

				if (!(m_data[0] == 0x1B && m_data[1] == 'L' && m_data[2] == 'u' && m_data[3] == 'a'))
				{
					return false;
				}

				if (m_data[4] != 0x51)
				{
					return false;
				}

				m_position = 0;
				ParseHeader();
				if (!m_header_supported)
				{
					return false;
				}

				ParseFunction();
				stats = m_stats;
				return m_stats.touched;
			}

		private:
			void ParseHeader()
			{
				Skip(4, "normalize.signature");
				Skip(1, "normalize.version");
				Skip(1, "normalize.format");

				const std::uint8_t endianness = ReadU8("normalize.endian");
				const std::uint8_t int_size = ReadU8("normalize.int_size");
				const std::uint8_t size_t_size = ReadU8("normalize.size_t_size");
				const std::uint8_t instruction_size = ReadU8("normalize.instruction_size");
				const std::uint8_t number_size = ReadU8("normalize.number_size");
				const std::uint8_t integral_numbers = ReadU8("normalize.integral");

				m_header_supported =
					endianness == 1
					&& int_size == 4
					&& size_t_size == 4
					&& instruction_size == 4
					&& number_size == 8
					&& integral_numbers == 0;
			}

			void ParseFunction()
			{
				ParseString("normalize.function.source");
				Skip(4, "normalize.function.linedefined");
				Skip(4, "normalize.function.lastlinedefined");
				Skip(1, "normalize.function.nups");
				Skip(1, "normalize.function.numparams");
				Skip(1, "normalize.function.is_vararg");
				Skip(1, "normalize.function.maxstacksize");

				const std::size_t code_length = ParseCode();
				ParseConstants();
				ParseDebug(code_length);
			}

			void ParseString(std::string_view stage)
			{
				const std::size_t size_offset = m_position;
				const std::uint32_t raw_size = ReadU32(stage);
				if (raw_size == 0)
				{
					return;
				}

				const std::uint8_t crypt_key_start = static_cast<std::uint8_t>(raw_size >> 24);
				const std::size_t size = static_cast<std::size_t>(raw_size & 0x00FFFFFFu);
				if (m_position + size > m_data.size())
				{
					throw ByteRevenantError(std::string(stage), "Строка выходит за границы чанка", size_offset);
				}

				if (crypt_key_start != 0)
				{
					WriteLeU32At(m_data, size_offset, static_cast<std::uint32_t>(size), stage);

					std::uint8_t crypt_key = crypt_key_start;
					for (std::size_t index = 0; index < size; ++index)
					{
						m_data[m_position + index] ^= kLuaQCryptTable[crypt_key];
						crypt_key = static_cast<std::uint8_t>((crypt_key + 1) & 0xFFu);
					}

					m_stats.touched = true;
					m_stats.decrypted_strings++;
				}

				m_position += size;
			}

			std::size_t ParseCode()
			{
				const std::size_t size_offset = m_position;
				std::size_t size = static_cast<std::size_t>(ReadU32("normalize.code.size"));
				const std::size_t code_offset = m_position;

				if (size > 0)
				{
					const std::uint32_t first_instruction = ReadLeU32At(m_data, code_offset, "normalize.code.first");
					if (LuaGetOpcode(first_instruction) == kOpMtaXor)
					{
						const std::size_t crypt_size = static_cast<std::size_t>(LuaGetArgBx(first_instruction)) * 4;
						if (crypt_size < 4 || code_offset + crypt_size > m_data.size())
						{
							throw ByteRevenantError("normalize.code", "Некорректный размер MTA XOR-блока", code_offset);
						}

						const int crypt_key = LuaGetArgA(first_instruction);
						std::size_t back_ptr = crypt_size - 1;
						for (std::size_t index = 0; index + 4 < crypt_size; ++index)
						{
							m_data[code_offset + back_ptr] ^= static_cast<std::uint8_t>((index ^ crypt_key ^ (5 * index)) & 0xFFu);
							--back_ptr;
						}

						if (ReadLeU32At(m_data, code_offset + crypt_size - 4, "normalize.code.return") != 0x0080001Eu)
						{
							throw ByteRevenantError("normalize.code", "Расшифрованный MTA XOR-блок не оканчивается на RETURN", code_offset);
						}

						size--;
						WriteLeU32At(m_data, size_offset, static_cast<std::uint32_t>(size), "normalize.code.size");
						EraseBytes(m_data, code_offset, 4, m_position, "normalize.code.remove_xor");
						m_stats.touched = true;
						m_stats.decrypted_code_blocks++;
					}
				}

				std::size_t index = 0;
				while (index < size)
				{
					const std::size_t instruction_offset = code_offset + 4 * index;
					std::uint32_t instruction = ReadLeU32At(m_data, instruction_offset, "normalize.code.inst");
					const int opcode = LuaGetOpcode(instruction);

					if (opcode == kOpMtaNop || opcode == kOpMtaFail)
					{
						size--;
						WriteLeU32At(m_data, size_offset, static_cast<std::uint32_t>(size), "normalize.code.size");
						EraseBytes(m_data, instruction_offset, 4, m_position, "normalize.code.remove_nop");
						m_stats.touched = true;
						m_stats.removed_nops++;
						continue;
					}

					if (opcode == static_cast<int>(OpCode::Jmp) || opcode == static_cast<int>(OpCode::ForLoop) || opcode == static_cast<int>(OpCode::ForPrep))
					{
						int target = LuaGetArgSBx(instruction);
						if (target > 60000)
						{
							target = 120000 - target;
							instruction = LuaSetArgSBx(instruction, target);
							WriteLeU32At(m_data, instruction_offset, instruction, "normalize.code.jump");
							m_stats.touched = true;
							m_stats.fixed_jumps++;
						}
					}

					index++;
				}

				m_position = code_offset + size * 4;
				return size;
			}

			void ParseConstants()
			{
				const std::size_t constant_count = static_cast<std::size_t>(ReadU32("normalize.constants.count"));
				for (std::size_t index = 0; index < constant_count; ++index)
				{
					const std::uint8_t type = ReadU8("normalize.constant.type");
					switch (type)
					{
					case 0:
						break;
					case 1:
						Skip(1, "normalize.constant.boolean");
						break;
					case 3:
						Skip(8, "normalize.constant.number");
						break;
					case 4:
						ParseString("normalize.constant.string");
						break;
					default:
						throw ByteRevenantError("normalize.constants", "Неизвестный тип константы в LuaQ", m_position);
					}
				}

				const std::size_t proto_count = static_cast<std::size_t>(ReadU32("normalize.prototypes.count"));
				for (std::size_t index = 0; index < proto_count; ++index)
				{
					ParseFunction();
				}
			}

			void ParseDebug(std::size_t code_length)
			{
				const std::size_t lineinfo_size_offset = m_position;
				std::size_t lineinfo_size = static_cast<std::size_t>(ReadU32("normalize.lineinfo.count"));
				if (lineinfo_size == code_length + 1 && m_position + 4 <= m_data.size())
				{
					WriteLeU32At(m_data, lineinfo_size_offset, static_cast<std::uint32_t>(code_length), "normalize.lineinfo.count");
					EraseBytes(m_data, m_position, 4, m_position, "normalize.lineinfo.shift");
					lineinfo_size--;
					m_stats.touched = true;
				}

				Skip(lineinfo_size * 4, "normalize.lineinfo");

				const std::size_t local_count = static_cast<std::size_t>(ReadU32("normalize.locals.count"));
				for (std::size_t index = 0; index < local_count; ++index)
				{
					ParseString("normalize.local.name");
					Skip(4, "normalize.local.startpc");
					Skip(4, "normalize.local.endpc");
				}

				const std::size_t upvalue_count = static_cast<std::size_t>(ReadU32("normalize.upvalues.count"));
				for (std::size_t index = 0; index < upvalue_count; ++index)
				{
					ParseString("normalize.upvalue.name");
				}
			}

			std::uint8_t ReadU8(std::string_view stage)
			{
				if (m_position >= m_data.size())
				{
					throw ByteRevenantError(std::string(stage), "Выход за границы при чтении byte", m_position);
				}

				return m_data[m_position++];
			}

			std::uint32_t ReadU32(std::string_view stage)
			{
				const std::uint32_t value = ReadLeU32At(m_data, m_position, stage);
				m_position += 4;
				return value;
			}

			void Skip(std::size_t count, std::string_view stage)
			{
				if (m_position + count > m_data.size())
				{
					throw ByteRevenantError(std::string(stage), "Выход за границы при пропуске данных", m_position);
				}

				m_position += count;
			}

			std::vector<std::uint8_t>& m_data;
			DiagnosticSink& m_diagnostics;
			std::size_t m_position = 0;
			bool m_header_supported = false;
			NormalizationStats m_stats;
		};

		std::optional<NormalizationStats> TryNormalizeLuaQ(std::vector<std::uint8_t>& data, DiagnosticSink& diagnostics)
		{
			if (data.size() < 12 || data[0] != 0x1B || data[1] != 'L' || data[2] != 'u' || data[3] != 'a' || data[4] != 0x51)
			{
				return std::nullopt;
			}

			std::vector<std::uint8_t> candidate = data;
			NormalizationStats stats{};

			try
			{
				MutableLuaQNormalizer normalizer(candidate, diagnostics);
				if (!normalizer.Run(stats))
				{
					return std::nullopt;
				}
			}
			catch (const ByteRevenantError& error)
			{
				diagnostics.Warning("normalize", "Пропускаю MTA LuaQ normalizer: " + std::string(error.what()), error.Offset());
				return std::nullopt;
			}

			data = std::move(candidate);
			return stats;
		}

		std::string ReadChunkString(br::BinaryReader& reader, std::size_t size_width, ParseContext& context, std::string_view stage)
		{
			const std::size_t length_offset = reader.Position();
			std::uint64_t raw_length = reader.ReadUnsigned(size_width, stage);
			if (raw_length == 0)
			{
				return {};
			}

			std::size_t length = static_cast<std::size_t>(raw_length);
			if (length > reader.Remaining())
			{
				bool recovered = false;
				if (size_width == 4)
				{
					const std::size_t candidate24 = static_cast<std::size_t>(raw_length & 0x00FFFFFFu);
					const std::size_t candidate16 = static_cast<std::size_t>(raw_length & 0x0000FFFFu);
					const std::size_t candidate8 = static_cast<std::size_t>(raw_length & 0x000000FFu);

					if (candidate24 > 0 && candidate24 <= reader.Remaining())
					{
						length = candidate24;
						recovered = true;
						context.string_recoveries++;
						context.diagnostics.Warning(std::string(stage), "Битая длина строки, использую младшие 24 бита", length_offset);
					}
					else if (candidate16 > 0 && candidate16 <= reader.Remaining())
					{
						length = candidate16;
						recovered = true;
						context.string_recoveries++;
						context.diagnostics.Warning(std::string(stage), "Битая длина строки, использую младшие 16 бит", length_offset);
					}
					else if (candidate8 > 0 && candidate8 <= reader.Remaining())
					{
						length = candidate8;
						recovered = true;
						context.string_recoveries++;
						context.diagnostics.Warning(std::string(stage), "Битая длина строки, использую младший байт", length_offset);
					}
				}

				if (!recovered)
				{
					throw ByteRevenantError(std::string(stage), "Битая длина строки", length_offset);
				}
			}

			const std::vector<std::uint8_t> bytes = reader.ReadBytes(length, stage);
			if (bytes.empty())
			{
				return {};
			}

			if (bytes.back() == 0)
			{
				return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1);
			}

			context.diagnostics.Warning(std::string(stage), "Строка без нулевого терминатора, читаю как есть", length_offset);
			return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		}

		std::unique_ptr<Function> ParseFunctionImpl(br::BinaryReader& reader, const Header& header, ParseContext& context);
	}

	const char* OpCodeName(OpCode opcode)
	{
		const std::size_t index = static_cast<std::size_t>(opcode);
		if (index >= kOpNames.size())
		{
			return "INVALID_OPCODE";
		}
		return kOpNames[index];
	}

	OpMode GetOpMode(OpCode opcode)
	{
		const std::size_t index = static_cast<std::size_t>(opcode);
		if (index >= kModes.size())
		{
			return OpMode::iABC;
		}
		return kModes[index];
	}

	bool IsConstantIndex(int value)
	{
		return (value & kBitRk) != 0;
	}

	int ConstantIndex(int value)
	{
		return value & ~kBitRk;
	}

	std::uint32_t Parser::Mask(std::uint32_t bits)
	{
		return (1u << bits) - 1u;
	}

	int Parser::GetArg(std::uint32_t raw, int position, int size)
	{
		return static_cast<int>((raw >> position) & Mask(size));
	}

	Instruction Parser::DecodeInstruction(std::uint32_t raw)
	{
		Instruction instruction{};
		instruction.raw = raw;
		instruction.opcode = static_cast<OpCode>(GetArg(raw, kPosOp, kSizeOp));
		instruction.a = GetArg(raw, kPosA, kSizeA);
		instruction.b = GetArg(raw, kPosB, kSizeB);
		instruction.c = GetArg(raw, kPosC, kSizeC);
		instruction.bx = GetArg(raw, kPosBx, kSizeB + kSizeC);
		instruction.sbx = instruction.bx - (Mask(kSizeB + kSizeC) >> 1);
		return instruction;
	}

	std::unique_ptr<Function> Parser::ParseFunction(br::BinaryReader& reader, const Header& header, DiagnosticSink& diagnostics)
	{
		ParseContext context{ diagnostics };
		return ParseFunctionImpl(reader, header, context);
	}

	namespace
	{
		std::unique_ptr<Function> ParseFunctionImpl(br::BinaryReader& reader, const Header& header, ParseContext& context)
	{
		auto function = std::make_unique<Function>();
		function->source_name = ReadChunkString(reader, header.size_t_size, context, "function.source");
		function->line_defined = static_cast<std::uint32_t>(reader.ReadUnsigned(header.int_size, "function.line_defined"));
		function->last_line_defined = static_cast<std::uint32_t>(reader.ReadUnsigned(header.int_size, "function.last_line_defined"));
		function->upvalue_count = reader.ReadU8("function.upvalue_count");
		function->parameter_count = reader.ReadU8("function.parameter_count");
		function->is_vararg = reader.ReadU8("function.is_vararg");
		function->max_stack_size = reader.ReadU8("function.max_stack");

		const auto code_size = static_cast<std::size_t>(reader.ReadUnsigned(header.int_size, "function.code_size"));
		function->code.reserve(code_size);
		for (std::size_t index = 0; index < code_size; ++index)
		{
			const auto raw = static_cast<std::uint32_t>(reader.ReadUnsigned(header.instruction_size, "function.code"));
			function->code.push_back(Parser::DecodeInstruction(raw));
		}

		const auto constant_count = static_cast<std::size_t>(reader.ReadUnsigned(header.int_size, "function.constants"));
		function->constants.reserve(constant_count);
		for (std::size_t index = 0; index < constant_count; ++index)
		{
			Constant constant{};
			constant.type = static_cast<ConstantType>(reader.ReadU8("constant.type"));
			switch (constant.type)
			{
			case ConstantType::Nil:
				break;
			case ConstantType::Boolean:
				constant.boolean = reader.ReadU8("constant.boolean") != 0;
				break;
			case ConstantType::Number:
				constant.number = reader.ReadNumber(header.number_size, header.integral_numbers, "constant.number");
				break;
			case ConstantType::String:
				constant.string = ReadChunkString(reader, header.size_t_size, context, "constant.string");
				break;
			default:
				throw ByteRevenantError("parse", "Неизвестный тип константы", reader.Position());
			}
			function->constants.push_back(std::move(constant));
		}

		const auto proto_count = static_cast<std::size_t>(reader.ReadUnsigned(header.int_size, "function.prototypes"));
		function->prototypes.reserve(proto_count);
		for (std::size_t index = 0; index < proto_count; ++index)
		{
			function->prototypes.push_back(ParseFunctionImpl(reader, header, context));
		}

		const auto line_count = static_cast<std::size_t>(reader.ReadUnsigned(header.int_size, "function.lines"));
		function->lines.reserve(line_count);
		for (std::size_t index = 0; index < line_count; ++index)
		{
			function->lines.push_back(static_cast<std::uint32_t>(reader.ReadUnsigned(header.int_size, "function.line")));
		}

		const auto local_count = static_cast<std::size_t>(reader.ReadUnsigned(header.int_size, "function.locals"));
		function->locals.reserve(local_count);
		for (std::size_t index = 0; index < local_count; ++index)
		{
			Local local{};
			local.name = ReadChunkString(reader, header.size_t_size, context, "local.name");
			local.start_pc = static_cast<std::uint32_t>(reader.ReadUnsigned(header.int_size, "local.start"));
			local.end_pc = static_cast<std::uint32_t>(reader.ReadUnsigned(header.int_size, "local.end"));
			function->locals.push_back(std::move(local));
		}

		const auto upvalue_name_count = static_cast<std::size_t>(reader.ReadUnsigned(header.int_size, "function.upvalue_names"));
		function->upvalue_names.reserve(upvalue_name_count);
		for (std::size_t index = 0; index < upvalue_name_count; ++index)
		{
			function->upvalue_names.push_back(ReadChunkString(reader, header.size_t_size, context, "upvalue.name"));
		}

		return function;
	}
	}

	Chunk Parser::Parse(const std::vector<std::uint8_t>& data, DiagnosticSink& diagnostics) const
	{
		std::vector<std::uint8_t> normalized = data;
		if (const auto stats = TryNormalizeLuaQ(normalized, diagnostics))
		{
			diagnostics.Info(
				"normalize",
				"MTA LuaQ normalizer: strings=" + std::to_string(stats->decrypted_strings)
				+ ", xor_blocks=" + std::to_string(stats->decrypted_code_blocks)
				+ ", nop=" + std::to_string(stats->removed_nops)
				+ ", jumps=" + std::to_string(stats->fixed_jumps));
		}

		br::BinaryReader reader(normalized);
		const auto signature = reader.ReadBytes(4, "header.signature");
		if (signature.size() != 4 || signature[0] != 0x1B || signature[1] != 'L' || signature[2] != 'u' || signature[3] != 'a')
		{
			if (LooksLikeLuaSourceText(normalized))
			{
				throw ByteRevenantError("parse", "Скрипт и так не скомпилирован: это обычный Lua source, а не Lua 5.1 bytecode");
			}

			throw ByteRevenantError("parse", "Lua 5.1 bytecode header не найден. Похоже, скрипт не скомпилирован или файл битый");
		}

		const auto version = reader.ReadU8("header.version");
		if (version != 0x51)
		{
			throw ByteRevenantError("parse", "Поддерживается только Lua 5.1");
		}

		const auto format = reader.ReadU8("header.format");
		if (format != 0)
		{
			diagnostics.Warning("parse", "Формат chunk не равен 0, всё равно пробую читать");
		}

		Chunk chunk{};
		chunk.header.little_endian = reader.ReadU8("header.endian") != 0;
		chunk.header.int_size = reader.ReadU8("header.int_size");
		chunk.header.size_t_size = reader.ReadU8("header.size_t_size");
		chunk.header.instruction_size = reader.ReadU8("header.instruction_size");
		chunk.header.number_size = reader.ReadU8("header.number_size");
		chunk.header.integral_numbers = reader.ReadU8("header.integral") != 0;

		reader.SetLittleEndian(chunk.header.little_endian);

		if (chunk.header.instruction_size != 4)
		{
			throw ByteRevenantError("parse", "Неподдерживаемый размер Instruction");
		}

		ParseContext context{ diagnostics };
		chunk.root = ParseFunctionImpl(reader, chunk.header, context);
		chunk.string_recoveries = context.string_recoveries;

		auto bind_captures = [&](auto&& self, Function& parent) -> void
		{
			std::size_t child_index = 0;
			for (std::size_t pc = 0; pc < parent.code.size() && child_index < parent.prototypes.size(); ++pc)
			{
				const Instruction& instruction = parent.code[pc];
				if (instruction.opcode != OpCode::Closure)
				{
					continue;
				}

				Function& child = *parent.prototypes[child_index++];
				child.captures.clear();
				for (std::size_t capture = 0; capture < child.upvalue_count; ++capture)
				{
					const std::size_t bind_pc = pc + 1 + capture;
					if (bind_pc >= parent.code.size())
					{
						break;
					}

					const Instruction& bind = parent.code[bind_pc];
					UpvalueCapture info{};
					info.from_register = bind.opcode == OpCode::Move;
					info.index = bind.b;
					child.captures.push_back(info);
				}
			}

			for (const auto& child : parent.prototypes)
			{
				self(self, *child);
			}
		};

		bind_captures(bind_captures, *chunk.root);
		return chunk;
	}
}
