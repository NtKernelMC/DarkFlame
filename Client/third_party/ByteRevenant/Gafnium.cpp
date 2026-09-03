#include "Gafnium.hpp"

#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace br
{
	namespace
	{
		constexpr std::size_t kSignatureSize = 64;
		constexpr std::size_t kHeaderSize = 93;
		constexpr std::size_t kSignedSize = kHeaderSize + kSignatureSize;
		constexpr std::size_t kRawMagicOffset = 81;
		constexpr std::size_t kRawHeaderOffsetOffset = 85;
		constexpr std::size_t kRawChecksumOffset = 89;
		constexpr std::size_t kHeaderXorStart = 8;
		constexpr std::size_t kHeaderXorLength = 73;
		constexpr std::size_t kHeaderCompiledAtOffset = 8;
		constexpr std::size_t kHeaderIpOffset = 18;
		constexpr std::size_t kHeaderServerHostVersionOffset = 22;
		constexpr std::size_t kHeaderServerRunVersionOffset = 35;
		constexpr std::size_t kHeaderClientRunVersionOffset = 48;
		constexpr std::size_t kHeaderVersionLength = 13;
		constexpr std::size_t kHeaderEncryptionFlagOffset = 61;
		constexpr std::size_t kHeaderSigModeOffset = 66;
		constexpr std::uint32_t kSignedMagic = 0xF14A55B7u;
		constexpr std::size_t kRsaBlockSize = 64;
		constexpr std::size_t kRsaPayloadSize = 63;
		constexpr char kObfuscationKeyHex[] =
			"0100010005B20A3E491EDB02B85E1073E4B3BA6EEA2AF02A361F32A6FA56A89FE7B773B11732F9394E8BF6A6D71F34A68B55AF4266A41AEC82363E8E3F37499818DBE0C9";
		constexpr std::array<const char*, 3> kSignatureKeyHexes =
		{
			"01000100998C1C031E148F2DDD9783E6F78542B862E7034983CBB9FD78F6836F1F5F82510270C964E1C538CBCA435BE0A6D3EF3174BA0EADB8B26AF89D8E31E0CF4A644C",
			"0100010097E565431323A6FC9557970DFDE25346E00D633B3B51550E9EA374B3B7ED40E4F5707A382B7E70B5FD2349CE3EFB42161FBA68C17580C559F600908932BEA1B3",
			"0100010045FAE258F357B981F2EE0DEB9EAE7C67B85FF1AE1A13BE6CCAE0F3625F7141E8F42728019978B4F8481E7F704E392B794CE5ED5F19BFEBA1E7D33AF00BF6C599",
		};

		struct ParsedSignedScript
		{
			std::vector<std::uint8_t> raw_scriptcontent;
			std::array<std::uint8_t, kHeaderSize> raw_header{};
			std::array<std::uint8_t, kHeaderSize> decoded_header{};
			SignedScriptInfo info{};
		};

		struct RsaPublicKey
		{
			BCRYPT_ALG_HANDLE alg = nullptr;
			BCRYPT_KEY_HANDLE key = nullptr;
			std::vector<std::uint8_t> blob;
			std::size_t bytesize = 0;
			std::string label;

			RsaPublicKey() = default;
			RsaPublicKey(RsaPublicKey&& other) noexcept
				: alg(std::exchange(other.alg, nullptr))
				, key(std::exchange(other.key, nullptr))
				, blob(std::move(other.blob))
				, bytesize(other.bytesize)
				, label(std::move(other.label))
			{
			}

			RsaPublicKey& operator=(RsaPublicKey&& other) noexcept
			{
				if (this == &other)
				{
					return *this;
				}

				if (key)
				{
					BCryptDestroyKey(key);
				}
				if (alg)
				{
					BCryptCloseAlgorithmProvider(alg, 0);
				}

				alg = std::exchange(other.alg, nullptr);
				key = std::exchange(other.key, nullptr);
				blob = std::move(other.blob);
				bytesize = other.bytesize;
				label = std::move(other.label);
				return *this;
			}

			~RsaPublicKey()
			{
				if (key)
				{
					BCryptDestroyKey(key);
				}
				if (alg)
				{
					BCryptCloseAlgorithmProvider(alg, 0);
				}
			}

			RsaPublicKey(const RsaPublicKey&) = delete;
			RsaPublicKey& operator=(const RsaPublicKey&) = delete;
		};

		std::uint32_t ReadLeU32(const std::vector<std::uint8_t>& data, std::size_t offset)
		{
			if (offset + 4 > data.size())
			{
				return 0;
			}

			return static_cast<std::uint32_t>(data[offset])
				| (static_cast<std::uint32_t>(data[offset + 1]) << 8)
				| (static_cast<std::uint32_t>(data[offset + 2]) << 16)
				| (static_cast<std::uint32_t>(data[offset + 3]) << 24);
		}

		std::uint32_t ReadLeU32(const std::array<std::uint8_t, kHeaderSize>& data, std::size_t offset)
		{
			if (offset + 4 > data.size())
			{
				return 0;
			}

			return static_cast<std::uint32_t>(data[offset])
				| (static_cast<std::uint32_t>(data[offset + 1]) << 8)
				| (static_cast<std::uint32_t>(data[offset + 2]) << 16)
				| (static_cast<std::uint32_t>(data[offset + 3]) << 24);
		}

		std::vector<std::uint8_t> ParseHexString(const char* hex)
		{
			if (!hex)
			{
				return {};
			}

			const std::size_t length = std::char_traits<char>::length(hex);
			if ((length % 2) != 0)
			{
				return {};
			}

			auto hex_value = [](char ch) -> int
			{
				if (ch >= '0' && ch <= '9')
				{
					return ch - '0';
				}
				if (ch >= 'a' && ch <= 'f')
				{
					return 10 + ch - 'a';
				}
				if (ch >= 'A' && ch <= 'F')
				{
					return 10 + ch - 'A';
				}
				return -1;
			};

			std::vector<std::uint8_t> out;
			out.reserve(length / 2);
			for (std::size_t index = 0; index < length; index += 2)
			{
				const int high = hex_value(hex[index]);
				const int low = hex_value(hex[index + 1]);
				if (high < 0 || low < 0)
				{
					return {};
				}

				out.push_back(static_cast<std::uint8_t>((high << 4) | low));
			}

			return out;
		}

		bool BuildRsaPublicKey(std::string_view hex, std::string_view label, RsaPublicKey& out)
		{
			const std::vector<std::uint8_t> key_bytes = ParseHexString(std::string(hex).c_str());
			if (key_bytes.size() <= 4)
			{
				return false;
			}

			RsaPublicKey key{};
			key.bytesize = key_bytes.size() - 4;
			key.label = std::string(label);
			if (key.bytesize != kRsaBlockSize)
			{
				return false;
			}

			const std::uint32_t exponent = ReadLeU32(key_bytes, 0);
			std::vector<std::uint8_t> exponent_bytes;
			for (std::uint32_t value = exponent; value > 0; value >>= 8)
			{
				exponent_bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
			}
			if (exponent_bytes.empty())
			{
				exponent_bytes.push_back(0);
			}
			std::reverse(exponent_bytes.begin(), exponent_bytes.end());

			std::vector<std::uint8_t> modulus(key_bytes.begin() + 4, key_bytes.end());
			std::reverse(modulus.begin(), modulus.end());

			BCRYPT_RSAKEY_BLOB header{};
			header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
			header.BitLength = static_cast<ULONG>(modulus.size() * 8);
			header.cbPublicExp = static_cast<ULONG>(exponent_bytes.size());
			header.cbModulus = static_cast<ULONG>(modulus.size());

			key.blob.resize(sizeof(BCRYPT_RSAKEY_BLOB) + exponent_bytes.size() + modulus.size());
			std::memcpy(key.blob.data(), &header, sizeof(header));
			std::memcpy(key.blob.data() + sizeof(header), exponent_bytes.data(), exponent_bytes.size());
			std::memcpy(key.blob.data() + sizeof(header) + exponent_bytes.size(), modulus.data(), modulus.size());

			if (BCryptOpenAlgorithmProvider(&key.alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0)
			{
				return false;
			}

			if (BCryptImportKeyPair(key.alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key.key, key.blob.data(), static_cast<ULONG>(key.blob.size()), 0) != 0)
			{
				return false;
			}

			out = std::move(key);
			return true;
		}

		std::string TrimAsciiField(const std::array<std::uint8_t, kHeaderSize>& data, std::size_t offset, std::size_t length)
		{
			if (offset + length > data.size())
			{
				return {};
			}

			std::string value;
			value.reserve(length);
			for (std::size_t index = 0; index < length; ++index)
			{
				const unsigned char ch = data[offset + index];
				if (ch == 0)
				{
					break;
				}

				value.push_back((std::isprint(ch) != 0) ? static_cast<char>(ch) : '?');
			}

			while (!value.empty() && (value.back() == ' ' || value.back() == '\0'))
			{
				value.pop_back();
			}

			return value;
		}

		std::string FormatIpv4(const std::array<std::uint8_t, kHeaderSize>& data, std::size_t offset)
		{
			if (offset + 4 > data.size())
			{
				return {};
			}

			return std::to_string(data[offset])
				+ "." + std::to_string(data[offset + 1])
				+ "." + std::to_string(data[offset + 2])
				+ "." + std::to_string(data[offset + 3]);
		}

		std::string FormatCompiledAt(const std::array<std::uint8_t, kHeaderSize>& data)
		{
			if (kHeaderCompiledAtOffset + 6 > data.size())
			{
				return {};
			}

			const unsigned year = 2000u + data[kHeaderCompiledAtOffset + 0];
			const unsigned month = data[kHeaderCompiledAtOffset + 1];
			const unsigned day = data[kHeaderCompiledAtOffset + 2];
			const unsigned hour = data[kHeaderCompiledAtOffset + 3];
			const unsigned minute = data[kHeaderCompiledAtOffset + 4];
			const unsigned second = data[kHeaderCompiledAtOffset + 5];

			std::ostringstream out;
			out << year << '-';
			out << (month < 10 ? "0" : "") << month << '-';
			out << (day < 10 ? "0" : "") << day << ' ';
			out << (hour < 10 ? "0" : "") << hour << ':';
			out << (minute < 10 ? "0" : "") << minute << ':';
			out << (second < 10 ? "0" : "") << second;
			return out.str();
		}

		int MapObfuscationLevel(std::string_view min_server_run_version)
		{
			if (min_server_run_version == "0.0.0-0.00000")
			{
				return 0;
			}
			if (min_server_run_version == "1.3.4-0.00000")
			{
				return 1;
			}
			if (min_server_run_version == "1.5.2-9.07903")
			{
				return 2;
			}
			if (min_server_run_version == "1.5.6-9.18728")
			{
				return 3;
			}
			return -1;
		}

		bool TryParseSignedScript(const std::vector<std::uint8_t>& input, ParsedSignedScript& out)
		{
			if (input.size() < kSignedSize)
			{
				return false;
			}

			const std::size_t footer_offset = input.size() - kSignedSize;
			const std::size_t raw_body_size = input.size() - kSignatureSize;
			if (raw_body_size < kHeaderSize)
			{
				return false;
			}

			ParsedSignedScript parsed{};
			parsed.raw_scriptcontent.assign(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(footer_offset));
			std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(footer_offset), kHeaderSize, parsed.raw_header.begin());
			parsed.decoded_header = parsed.raw_header;

			parsed.info.parsed = true;
			parsed.info.magic_valid = ReadLeU32(parsed.raw_header, kRawMagicOffset) == kSignedMagic;
			parsed.info.header_offset = ReadLeU32(parsed.raw_header, kRawHeaderOffsetOffset);
			parsed.info.header_offset_valid = parsed.info.header_offset == footer_offset;
			parsed.info.checksum = ReadLeU32(parsed.raw_header, kRawChecksumOffset);

			std::uint32_t checksum = 0;
			const std::size_t checksum_end = raw_body_size > 5 ? raw_body_size - 5 : 0;
			for (std::size_t index = 0; index < checksum_end; ++index)
			{
				checksum += input[index];
			}
			parsed.info.checksum_calculated = checksum;
			parsed.info.checksum_valid = parsed.info.checksum == parsed.info.checksum_calculated;

			if (!parsed.info.magic_valid || !parsed.info.header_offset_valid)
			{
				return false;
			}

			for (std::size_t index = 0; index < kHeaderXorLength; ++index)
			{
				const auto mask = static_cast<std::uint8_t>(((index % 12) * (index % 12)) ^ 0x5D ^ (1ull << (index & 7)));
				parsed.decoded_header[kHeaderXorStart + index] ^= mask;
			}

			parsed.info.compiled_at = FormatCompiledAt(parsed.decoded_header);
			parsed.info.uploader_ip = FormatIpv4(parsed.decoded_header, kHeaderIpOffset);
			parsed.info.min_server_host_version = TrimAsciiField(parsed.decoded_header, kHeaderServerHostVersionOffset, kHeaderVersionLength);
			parsed.info.min_server_run_version = TrimAsciiField(parsed.decoded_header, kHeaderServerRunVersionOffset, kHeaderVersionLength);
			parsed.info.min_client_run_version = TrimAsciiField(parsed.decoded_header, kHeaderClientRunVersionOffset, kHeaderVersionLength);
			parsed.info.mode = ReadLeU32(parsed.decoded_header, kHeaderEncryptionFlagOffset);
			parsed.info.encrypted_bytecode = parsed.info.mode == 1;
			parsed.info.sig_mode = parsed.decoded_header[kHeaderSigModeOffset];
			parsed.info.obfuscation_level = MapObfuscationLevel(parsed.info.min_server_run_version);

			out = std::move(parsed);
			return true;
		}

		bool RawRsaTransformBlock(const std::uint8_t* block, std::size_t block_size, const RsaPublicKey& key, std::vector<std::uint8_t>& out)
		{
			if (block_size > key.bytesize || key.bytesize == 0)
			{
				return false;
			}

			std::vector<std::uint8_t> block_be(block, block + block_size);
			std::reverse(block_be.begin(), block_be.end());

			ULONG out_size = 0;
			if (BCryptEncrypt(key.key, block_be.data(), static_cast<ULONG>(block_be.size()), nullptr, nullptr, 0, nullptr, 0, &out_size, BCRYPT_PAD_NONE) != 0)
			{
				return false;
			}

			std::vector<std::uint8_t> transformed_be(out_size);
			if (BCryptEncrypt(key.key, block_be.data(), static_cast<ULONG>(block_be.size()), nullptr, nullptr, 0, transformed_be.data(), static_cast<ULONG>(transformed_be.size()), &out_size, BCRYPT_PAD_NONE) != 0)
			{
				return false;
			}

			transformed_be.resize(out_size);
			std::reverse(transformed_be.begin(), transformed_be.end());
			out = std::move(transformed_be);
			return out.size() == key.bytesize;
		}

		bool DecryptLuaBytecodeExact(const std::vector<std::uint8_t>& raw_scriptcontent, const RsaPublicKey& key, std::vector<std::uint8_t>& out)
		{
			if (raw_scriptcontent.size() < 9)
			{
				return false;
			}

			if (raw_scriptcontent.size() <= 5 + 4)
			{
				return false;
			}

			const std::uint32_t original_length = ReadLeU32(raw_scriptcontent, raw_scriptcontent.size() - 4);
			const std::size_t encrypted_offset = 5;
			const std::size_t encrypted_size = raw_scriptcontent.size() - encrypted_offset - 4;
			if (encrypted_size == 0 || (encrypted_size % key.bytesize) != 0)
			{
				return false;
			}

			std::vector<std::uint8_t> decrypted;
			decrypted.reserve(original_length);

			for (std::size_t offset = 0; offset < encrypted_size; offset += key.bytesize)
			{
				std::vector<std::uint8_t> block_out;
				if (!RawRsaTransformBlock(raw_scriptcontent.data() + encrypted_offset + offset, key.bytesize, key, block_out))
				{
					return false;
				}

				const std::size_t remaining = (original_length > decrypted.size()) ? (original_length - decrypted.size()) : 0;
				if (remaining == 0)
				{
					break;
				}

				const std::size_t take = std::min<std::size_t>(kRsaPayloadSize, remaining);
				decrypted.insert(decrypted.end(), block_out.begin(), block_out.begin() + static_cast<std::ptrdiff_t>(take));
			}

			if (decrypted.size() != original_length)
			{
				return false;
			}

			out = std::move(decrypted);
			return true;
		}

		bool TryDecryptWithKnownKeys(const std::vector<std::uint8_t>& raw_scriptcontent, std::vector<std::uint8_t>& out, std::string& key_label)
		{
			std::vector<std::pair<std::string, std::string>> candidates;
			candidates.emplace_back("obfkey", kObfuscationKeyHex);
			for (std::size_t index = 0; index < kSignatureKeyHexes.size(); ++index)
			{
				candidates.emplace_back("signkey" + std::to_string(index + 1), kSignatureKeyHexes[index]);
			}

			for (const auto& [label, hex] : candidates)
			{
				RsaPublicKey key{};
				if (!BuildRsaPublicKey(hex, label, key))
				{
					continue;
				}

				std::vector<std::uint8_t> candidate;
				if (!DecryptLuaBytecodeExact(raw_scriptcontent, key, candidate))
				{
					continue;
				}

				std::size_t lua_offset = 0;
				if (!GafniumProcessor::LooksLikeLuaBytecode(candidate, &lua_offset))
				{
					continue;
				}

				out = std::move(candidate);
				key_label = label;
				return true;
			}

			return false;
		}

		std::string FormatLevelSuffix(const SignedScriptInfo& info)
		{
			if (info.obfuscation_level < 0)
			{
				return {};
			}

			return ", level=" + std::to_string(info.obfuscation_level);
		}
	}

	bool GafniumProcessor::LooksLikeLuaBytecode(const std::vector<std::uint8_t>& data, std::size_t* offset)
	{
		for (std::size_t index = 0; index + 4 <= data.size() && index < 0x400; ++index)
		{
			if (data[index] == 0x1B && data[index + 1] == 'L' && data[index + 2] == 'u' && data[index + 3] == 'a')
			{
				if (offset)
				{
					*offset = index;
				}
				return true;
			}
		}

		return false;
	}

	DeobfuscationResult GafniumProcessor::Process(const std::vector<std::uint8_t>& input, DiagnosticSink& diagnostics) const
	{
		ParsedSignedScript parsed{};
		if (TryParseSignedScript(input, parsed))
		{
			DeobfuscationResult result{};
			result.had_footer = true;
			result.script = parsed.info;

			if (!parsed.info.checksum_valid)
			{
				diagnostics.Warning("gafnium", "Checksum signed header не сошёлся, продолжаю осторожно", parsed.info.header_offset);
			}

			if (parsed.info.encrypted_bytecode)
			{
				std::vector<std::uint8_t> decrypted;
				std::string key_label;
				if (!TryDecryptWithKnownKeys(parsed.raw_scriptcontent, decrypted, key_label))
				{
					throw ByteRevenantError("gafnium", "Signed footer найден, но точный MTA RSA decrypt не дал валидный chunk", parsed.info.header_offset);
				}

				std::size_t lua_offset = 0;
				if (!LooksLikeLuaBytecode(decrypted, &lua_offset))
				{
					throw ByteRevenantError("gafnium", "RSA отработал, но валидный Lua 5.1 bytecode внутри не найден. Похоже, скрипт не скомпилирован или файл битый", parsed.info.header_offset);
				}

				if (lua_offset > 0)
				{
					diagnostics.Warning("gafnium", "После exact RSA спереди остался мусор, срезал", lua_offset);
					result.data.assign(decrypted.begin() + static_cast<std::ptrdiff_t>(lua_offset), decrypted.end());
				}
				else
				{
					result.data = std::move(decrypted);
				}

				result.mode = DeobfuscationMode::Rsa;
				result.script.rsa_key_label = key_label;
				result.note = "rsa exact-le key=" + key_label + FormatLevelSuffix(result.script);
				diagnostics.Info("gafnium", "Signed script распознан и расшифрован точным little-endian RSA");
				return result;
			}

			std::size_t lua_offset = 0;
			if (LooksLikeLuaBytecode(parsed.raw_scriptcontent, &lua_offset) && lua_offset > 0)
			{
				diagnostics.Warning("gafnium", "Lua header внутри signed script был не с нуля, мусор спереди срезан", lua_offset);
				result.data.assign(parsed.raw_scriptcontent.begin() + static_cast<std::ptrdiff_t>(lua_offset), parsed.raw_scriptcontent.end());
			}
			else
			{
				result.data = std::move(parsed.raw_scriptcontent);
			}

			result.mode = DeobfuscationMode::FooterOnly;
			result.note = "signed footer снят, rsa не нужен" + FormatLevelSuffix(result.script);
			diagnostics.Info("gafnium", "Signed script распознан, RSA не понадобился");
			return result;
		}

		std::size_t direct_offset = 0;
		if (LooksLikeLuaBytecode(input, &direct_offset))
		{
			DeobfuscationResult result{};
			if (direct_offset == 0)
			{
				result.data = input;
				result.note = "lua chunk без rsa";
			}
			else
			{
				result.data.assign(input.begin() + static_cast<std::ptrdiff_t>(direct_offset), input.end());
				result.note = "lua header найден со смещением " + Hex(direct_offset);
				diagnostics.Warning("gafnium", "Нашёл Lua header не с нуля, мусор спереди срезан", direct_offset);
			}
			return result;
		}

		DeobfuscationResult result{};
		result.data = input;
		result.note = "signed footer не найден, rsa не применялся";
		return result;
	}
}
