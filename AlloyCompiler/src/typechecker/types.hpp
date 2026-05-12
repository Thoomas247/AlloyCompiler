#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "../builtins/builtins.hpp"
#include "../parser/AST.hpp"
#include "../tokenizer/tokenizer.hpp"

using TypeId = uint32_t;
static constexpr TypeId INVALID_TYPE_ID = UINT32_MAX;

// well-known primitive TypeIds (pre-allocated at indices 0-10).
static constexpr TypeId TYPE_U8 = 0;
static constexpr TypeId TYPE_U16 = 1;
static constexpr TypeId TYPE_U32 = 2;
static constexpr TypeId TYPE_U64 = 3;
static constexpr TypeId TYPE_I8 = 4;
static constexpr TypeId TYPE_I16 = 5;
static constexpr TypeId TYPE_I32 = 6;
static constexpr TypeId TYPE_I64 = 7;
static constexpr TypeId TYPE_F32 = 8;
static constexpr TypeId TYPE_F64 = 9;
static constexpr TypeId TYPE_BOOL = 10;
static constexpr TypeId TYPE_PRIMITIVE_COUNT = 11;

// Synthetic sentinels for literal expressions — never stored in InternedTypes::table.
// Assignable to any integer primitive (UNTYPED_INT) or float primitive (UNTYPED_FLOAT),
// including named types whose underlying chain reaches the right primitive class.
static constexpr TypeId TYPE_UNTYPED_INT   = UINT32_MAX - 1;
static constexpr TypeId TYPE_UNTYPED_FLOAT = UINT32_MAX - 2;

struct TypeInfo
{
	enum class Kind
	{
		Primitive,   // u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool
		Pointer,     // *T
		Reference,   // &T
		PtrMut,      // *var T
		RefMut,      // &var T
		Slice,       // [T]    (ArrayType with size == 0)
		Array,       // [T; N] (ArrayType with size > 0)
		Struct,      // { name: T, ... }  declaration-order members
		Enum,        // { A; B: T; ... }
		Function,    // (T...) -> R
		Named,       // user-defined alias: name + underlying TypeId
		TypeParam,   // generic type variable (T, U, ...)
	};

	Kind kind = Kind::Primitive;

	struct PrimitiveData
	{
		std::string_view name;
		bool isFloat = false;
		bool isSigned = false;  // meaningless for float
		uint8_t byteWidth = 0;
	};

	struct IndirectionData
	{
		TypeId inner = INVALID_TYPE_ID;
	};

	struct SliceData
	{
		TypeId elem = INVALID_TYPE_ID;
	};

	struct ArrayData
	{
		TypeId elem = INVALID_TYPE_ID;
		size_t size = 0;
	};

	struct StructMember
	{
		std::string_view name;
		TypeId type = INVALID_TYPE_ID;
	};

	struct StructData
	{
		std::vector<StructMember> members;  // declaration order
	};

	struct EnumVariant
	{
		std::string_view name;
		std::optional<TypeId> payloadType;
	};

	struct EnumData
	{
		std::vector<EnumVariant> variants;
	};

	struct FunctionData
	{
		std::vector<TypeId> params;
		std::optional<TypeId> ret;
	};

	struct NamedData
	{
		std::string_view name;
		TypeId underlying = INVALID_TYPE_ID;
	};

	struct TypeParamData
	{
		std::string_view name;
		std::optional<BuiltinInterface> constraint;
	};

	std::variant<
		PrimitiveData,
		IndirectionData,
		SliceData,
		ArrayData,
		StructData,
		EnumData,
		FunctionData,
		NamedData,
		TypeParamData
	> data;

	const PrimitiveData& asPrimitive()  const { return std::get<PrimitiveData>(data); }
	const IndirectionData& asIndirection()const { return std::get<IndirectionData>(data); }
	const SliceData& asSlice()      const { return std::get<SliceData>(data); }
	const ArrayData& asArray()      const { return std::get<ArrayData>(data); }
	const StructData& asStruct()     const { return std::get<StructData>(data); }
	const EnumData& asEnum()       const { return std::get<EnumData>(data); }
	const FunctionData& asFunction()   const { return std::get<FunctionData>(data); }
	const NamedData& asNamed()      const { return std::get<NamedData>(data); }
	const TypeParamData& asTypeParam()  const { return std::get<TypeParamData>(data); }

	bool isIndirection() const
	{
		return kind == Kind::Pointer || kind == Kind::Reference ||
			kind == Kind::PtrMut || kind == Kind::RefMut;
	}

	bool isMutableIndirection() const
	{
		return kind == Kind::PtrMut || kind == Kind::RefMut;
	}

	bool isPointerKind() const
	{
		return kind == Kind::Pointer || kind == Kind::PtrMut;
	}
};

struct InternedTypes
{
	// all unique type infos, indexed by TypeId
	std::vector<TypeInfo> table;

	// every explicit AST::Type annotation to its TypeId
	std::unordered_map<const AST::Type*, TypeId> astTypes;

	// TypeDefinition name token to Named TypeId
	std::unordered_map<const Token*, TypeId> namedTypeIds;

	// TypeParameter name token to TypeParam TypeId
	std::unordered_map<const Token*, TypeId> typeParamIds;

	const TypeInfo& get(TypeId id) const { return table[id]; }

	// returns the innermost value type: strips one level of *, &, *var, &var
	// if the type is not an indirection, returns id unchanged
	TypeId valueType(TypeId id) const
	{
		const auto& info = get(id);
		if (info.isIndirection())
		{
			return info.asIndirection().inner;
		}
		return id;
	}

	// checks whether TypeId is a primitive matching the given name
	bool isPrimitive(TypeId id, std::string_view name) const
	{
		const auto& info = get(id);
		return info.kind == TypeInfo::Kind::Primitive && info.asPrimitive().name == name;
	}
};
