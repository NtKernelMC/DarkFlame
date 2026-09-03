#pragma once

#include "Lua51.hpp"

namespace br::lua51
{
	struct DecompileResult
	{
		std::string lua;
	};

	class Decompiler
	{
	public:
		DecompileResult Decompile(const Chunk& chunk, DiagnosticSink& diagnostics) const;
	};
}
