#pragma once

enum class Status : uint8_t
{
	Error = 0,
	Ok
};

constexpr Status operator&=(Status& lhs, Status rhs) {
	static_assert(std::to_underlying(Status::Error) == 0);

	lhs = static_cast<Status>(std::to_underlying(lhs) & std::to_underlying(rhs));

	return lhs;
}

template<typename T>
struct Result
{
	Status status;
	T value;
};
