#pragma once

#include "logger.hpp"

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
		return m_pValue != nullptr;
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

	T* ptr()
	{
		return m_pValue;
	}

private:
	T* m_pValue;
};

namespace AST
{
	template <typename T>
	struct ListNode;
}

template <typename T>
class Optional<AST::ListNode<T>>
{
public:
	Optional()
		: m_pValue(nullptr)
	{
	}

	Optional(AST::ListNode<T>* pValue)
		: m_pValue(pValue)
	{
	}

	bool hasValue() const
	{
		return m_pValue != nullptr;
	}

	const AST::ListNode<T>& value() const
	{
		ASSERT(m_pValue != nullptr);
		return *m_pValue;
	}

	const AST::ListNode<T>* ptr() const
	{
		return m_pValue;
	}

	AST::ListNode<T>* ptr()
	{
		return m_pValue;
	}

	template <typename Function>
	void forEach(Function&& func) const
	{
		if (!hasValue())
		{
			return;
		}

		m_pValue->forEach(func);
	}

private:
	AST::ListNode<T>* m_pValue;
};

template <typename T>
class Required
{
public:
	Required()
		: m_pValue(nullptr)
	{
	}

	Required(T* pValue)
		: m_pValue(pValue)
	{
		ASSERT(m_pValue != nullptr);
	}

	const T& value() const
	{
		ASSERT(m_pValue != nullptr);
		return *m_pValue;
	}

	T& value()
	{
		ASSERT(m_pValue != nullptr);
		return *m_pValue;
	}

	const T* ptr() const
	{
		ASSERT(m_pValue != nullptr);
		return m_pValue;
	}

	T* ptr()
	{
		ASSERT(m_pValue != nullptr);
		return m_pValue;
	}

private:
	T* m_pValue;
};
