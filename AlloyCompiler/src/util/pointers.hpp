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
