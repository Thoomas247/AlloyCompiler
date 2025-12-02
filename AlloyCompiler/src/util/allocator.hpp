#pragma once
#include <vector>

#include "logger.hpp"

template <typename T, typename... Args>
concept Constructible = requires(Args&&... args)
{
	new T(std::forward<Args>(args)...);
};

class Allocator
{
public:
	Allocator()
		: m_Blocks(1)
	{
	};

	Allocator(Allocator&& other) noexcept
		: m_Blocks(std::move(other.m_Blocks))
	{
	}

	Allocator(const Allocator&) = delete;

	~Allocator() = default;

	template <typename T, typename... Args>
	T* allocate(Args&&... args)
		requires std::constructible_from<T, Args...>
	{
		ASSERT(!m_Blocks.empty());

		T* pObj = m_Blocks.back().allocate<T>(std::forward<Args>(args)...);
		if (!pObj)
		{
			m_Blocks.emplace_back();
			pObj = m_Blocks.back().allocate<T>(std::forward<Args>(args)...);
		}

		return pObj;
	}

private:
	class MemBlock
	{
	public:
		MemBlock()
			: m_pBlock(nullptr), m_Current(0)
		{
			m_pBlock = (std::byte*)std::malloc(BLOCK_SIZE);

			ASSERT(m_pBlock != nullptr);
		}

		MemBlock(const MemBlock&) = delete;

		MemBlock(MemBlock&& other) noexcept
			: m_pBlock(other.m_pBlock), m_Current(other.m_Current)
		{
			other.m_pBlock = nullptr;
			other.m_Current = 0;
		}

		~MemBlock()
		{
			std::free(m_pBlock);
		}

		template <typename T, typename... Args>
		T* allocate(Args&&... args)
		{
			ASSERT(m_pBlock != nullptr);

			// align current as needed by T
			const uintptr_t offset = (uintptr_t)(m_pBlock + m_Current) & (alignof(T) - 1);
			if (offset != 0)
			{
				m_Current += alignof(T) - offset;
			}

			// check if page has remaining space
			if (m_Current + sizeof(T) > BLOCK_SIZE)
			{
				return nullptr;
			}

			T* pObj = new (m_pBlock + m_Current) T(std::forward<Args>(args)...);
			m_Current += sizeof(T);
			return pObj;
		}

	private:
		constexpr static size_t BLOCK_SIZE = 16 * 1024; // 16 KB

		std::byte* m_pBlock;
		size_t m_Current;
	};

	std::vector<MemBlock> m_Blocks;
};