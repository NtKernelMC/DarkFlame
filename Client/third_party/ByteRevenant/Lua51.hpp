#pragma once

#include "Common.hpp"

namespace br::lua51
{
	enum class ConstantType : std::uint8_t
	{
		Nil = 0,
		Boolean = 1,
		Number = 3,
		String = 4,
	};

	enum class OpCode : std::uint8_t
	{
		Move,
		LoadK,
		LoadBool,
		LoadNil,
		GetUpval,
		GetGlobal,
		GetTable,
		SetGlobal,
		SetUpval,
		SetTable,
		NewTable,
		Self,
		Add,
		Sub,
		Mul,
		Div,
		Mod,
		Pow,
		Unm,
		Not,
		Len,
		Concat,
		Jmp,
		Eq,
		Lt,
		Le,
		Test,
		TestSet,
		Call,
		TailCall,
		Return,
		ForLoop,
		ForPrep,
		TForLoop,
		SetList,
		Close,
		Closure,
		VarArg,
	};

	enum class OpMode : std::uint8_t
	{
		iABC,
		iABx,
		iAsBx,
	};

	struct Header
	{
		bool little_endian = true;
		std::size_t int_size = 4;
		std::size_t size_t_size = 4;
		std::size_t instruction_size = 4;
		std::size_t number_size = 8;
		bool integral_numbers = false;
	};

	struct Constant
	{
		ConstantType type = ConstantType::Nil;
		bool boolean = false;
		double number = 0.0;
		std::string string;
	};

	struct Instruction
	{
		std::uint32_t raw = 0;
		OpCode opcode = OpCode::Move;
		int a = 0;
		int b = 0;
		int c = 0;
		int bx = 0;
		int sbx = 0;
	};

	struct Local
	{
		std::string name;
		std::uint32_t start_pc = 0;
		std::uint32_t end_pc = 0;
	};

	struct UpvalueCapture
	{
		bool from_register = false;
		int index = 0;
	};

	struct Function
	{
		std::string source_name;
		std::uint32_t line_defined = 0;
		std::uint32_t last_line_defined = 0;
		std::uint8_t upvalue_count = 0;
		std::uint8_t parameter_count = 0;
		std::uint8_t is_vararg = 0;
		std::uint8_t max_stack_size = 0;
		std::vector<Instruction> code;
		std::vector<Constant> constants;
		std::vector<std::unique_ptr<Function>> prototypes;
		std::vector<std::uint32_t> lines;
		std::vector<Local> locals;
		std::vector<std::string> upvalue_names;
		std::vector<UpvalueCapture> captures;
	};

	struct Chunk
	{
		Header header;
		std::unique_ptr<Function> root;
		std::size_t string_recoveries = 0;
	};

	class Parser
	{
	public:
		Chunk Parse(const std::vector<std::uint8_t>& data, DiagnosticSink& diagnostics) const;
		static Instruction DecodeInstruction(std::uint32_t raw);

	private:
		static std::uint32_t Mask(std::uint32_t bits);
		static int GetArg(std::uint32_t raw, int position, int size);
		static std::unique_ptr<Function> ParseFunction(br::BinaryReader& reader, const Header& header, DiagnosticSink& diagnostics);
	};

	const char* OpCodeName(OpCode opcode);
	OpMode GetOpMode(OpCode opcode);
	bool IsConstantIndex(int value);
	int ConstantIndex(int value);
}
