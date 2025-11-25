#pragma once
#include <vector>

#include "logger.hpp"

class Allocator
{
public:
	Allocator() = default;
	~Allocator() = default;

	template <typename T, typename... Args>
	T* allocate(Args&&... args)
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

		~MemBlock()
		{
			std::free(m_pBlock);
		}

		template <typename T, typename... Args>
		T* allocate(Args&&... args)
		{
			ASSERT(m_pBlock != nullptr);

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