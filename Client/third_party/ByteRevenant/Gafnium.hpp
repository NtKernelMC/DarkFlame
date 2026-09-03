#pragma once

#include "Common.hpp"

namespace br
{
	enum class DeobfuscationMode
	{
		None,
		FooterOnly,
		Rsa,
	};

	struct SignedScriptInfo
	{
		bool parsed = false;
		bool magic_valid = false;
		bool header_offset_valid = false;
		bool checksum_valid = false;
		bool encrypted_bytecode = false;
		int obfuscation_level = -1;
		std::uint32_t mode = 0;
		std::uint8_t sig_mode = 0;
		std::uint32_t header_offset = 0;
		std::uint32_t checksum = 0;
		std::uint32_t checksum_calculated = 0;
		std::string compiled_at;
		std::string uploader_ip;
		std::string min_server_host_version;
		std::string min_server_run_version;
		std::string min_client_run_version;
		std::string rsa_key_label;
	};

	struct DeobfuscationResult
	{
		std::vector<std::uint8_t> data;
		DeobfuscationMode mode = DeobfuscationMode::None;
		std::string note;
		bool had_footer = false;
		SignedScriptInfo script;
	};

	class GafniumProcessor
	{
	public:
		DeobfuscationResult Process(const std::vector<std::uint8_t>& input, DiagnosticSink& diagnostics) const;
		static bool LooksLikeLuaBytecode(const std::vector<std::uint8_t>& data, std::size_t* offset = nullptr);
	};
}
