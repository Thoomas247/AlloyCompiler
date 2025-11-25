#pragma once

#include <string>
#include <vector>
#include <variant>

#include "source.hpp"

namespace AST
{

	template <typename T>
	class Optional
	{
	public:
		Optional()
			: m_pValue(nullptr)
		{
		}
		Optional(T* pValue)
			: m_pValue(pValue)
		{
		}

		bool hasValue() const
		{
			return m_Ptr != nullptr;
		}

		const T& value() const
		{
			ASSERT(m_pValue != nullptr);
			return *m_pValue;
		}

		const T* ptr() const
		{
			return m_pValue;
		}

	private:
		T* m_pValue;
	};

	template <typename T>
	struct ListNode
	{
		T item;
		Optional<ListNode<T>> next;

		template <typename Function>
		void forEach(Function&& func) const
		{
			const ListNode<T>* current = this;
			while (current)
			{
				func(current->item);
				current = current->next.ptr();
			}
		}
	};

#pragma region Type Nodes

	using Type = std::variant<NamedType, StructType, EnumType, ArrayType>;

	struct NamedType
	{
		std::string_view typeName;
		Type underlyingType;
	};

	struct StructType
	{
		struct Member
		{
			std::string_view name;
			Type type;
		};

		ListNode<Member> members;
	};

	struct EnumType
	{
		struct Member
		{
			std::string_view name;
			Optional<Type> payloadType = nullptr;
		};

		ListNode<Member> members;
	};

	struct ArrayType
	{
		Type elementType;
		size_t size;
	};

#pragma endregion

	struct Statement
	{
		// TODO
	};

	struct StatementBlock
	{
		ListNode<Statement> statements;
	};

	struct FunctionParameter
	{
		std::string_view name;
		Type type;
	};

	struct Function
	{
		ListNode<FunctionParameter> parameters;
		Optional<Type> returnType;
		StatementBlock body;
	};

	struct FunctionDefinition
	{
		std::string_view name;
		Function function;
	};

	struct ExternDefinition
	{
		std::string_view name;
		ListNode<FunctionParameter> parameters;
		bool isVariadic;
	};

	struct Program
	{
		ListNode<std::string_view> imports;
	};
}

Result<bool> parse(const Source& source, const std::vector<std::string>& tokens);